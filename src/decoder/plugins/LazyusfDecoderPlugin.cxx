/* vim: set tabstop=4 softtabstop=4 shiftwidth=4 noexpandtab : */

#include "config.h"
#include "LazyusfDecoderPlugin.hxx"
#include "../DecoderAPI.hxx"
#include "pcm/CheckAudioFormat.hxx"
#include "tag/Handler.hxx"
#include "tag/Builder.hxx"
#include "fs/Path.hxx"
#include "util/ScopeExit.hxx"
#include "util/Domain.hxx"
#include "util/StringFormat.hxx"
#include "util/StringView.hxx"
#include "Log.hxx"

#include <psflib.h>
#include <usf.h>
#include <stdint.h>

static constexpr Domain lazyusf_domain("lazyusf");

static constexpr unsigned LAZYUSF_CHANNELS = 2;
static constexpr unsigned LAZYUSF_BUFFER_FRAMES = 1024;
static constexpr unsigned LAZYUSF_BUFFER_SAMPLES =
  LAZYUSF_BUFFER_FRAMES * LAZYUSF_CHANNELS;

static const char *LazyUSF_separators = "\\/:|";

#define MIN(a,b) (a < b ? a : b)

static int8_t enable_hle;
static int32_t sample_rate;

/**
 * applies a fade to an audio sample
 */

gcc_pure
static int16_t
FadeUSFSample(int16_t s, int32_t n, int32_t d)
{
	int64_t r = (int64_t) s;
	r *= n;
	r /= d;
	if (r > 32767 )
	{
		r = 32767;
	}
	else if(r < -32768)
	{
		r = -32768;
	}
	return (int16_t) r;
}

/**
 * parses H:M:S into milliseconds
 */
gcc_pure
static unsigned
ParseUSFTime(const char *ts)
{
	unsigned int i = 0;
	unsigned int t = 0;
	unsigned int c = 0;
	unsigned int m = 1000;
	for(i=0;i<strlen(ts);i++)
	{
		if(ts[i] == ':')
		{
			t *= 60;
			t += c*60;
			c = 0;
		}
		else if(ts[i] == '.') {
			m = 1;
			t += c;
			c = 0;
		}
		else
		{
			if(ts[i] < 48 || ts[i] > 57)
			{
				return 0;
			}
			c *= 10;
			c += (ts[i] - 48) * m;
		}
	}
	return c + t;
}

static void
*LazyUSF_fopen(void *context, const char *path)
{
	(void)context;
	return fopen(path,"rb");
}

static size_t
LazyUSF_fread(void *buffer, size_t size, size_t count, void *handle)
{
	FILE *f = (FILE *)handle;
	return fread(buffer,size,count,f);
}

static int
LazyUSF_fseek(void * handle, int64_t offset, int whence)
{
	FILE *f = (FILE *)handle;
	return fseek(f,offset,whence);
}

static int
LazyUSF_fclose(void * handle)
{
	FILE *f = (FILE *)handle;
	return fclose(f);
}

static long
LazyUSF_ftell(void * handle)
{
	FILE *f = (FILE *)handle;
	return ftell(f);
}

static psf_file_callbacks LazyUSF_psf_callbacks =
{
	LazyUSF_separators,
	nullptr,
	LazyUSF_fopen,
	LazyUSF_fread,
	LazyUSF_fseek,
	LazyUSF_fclose,
	LazyUSF_ftell,
};

struct LazyUSF_TagHolder
{
	unsigned int length = 0;
	unsigned int fade = 0;
	unsigned int enable_compare = 0;
	unsigned int enable_fifo_full = 0;
	TagHandler *handler = nullptr;
	LazyUSF_TagHolder(TagHandler *h) {
		handler = h;
	}
};

static int
LazyUSF_TagHandler(void *context, const char *name, const char *value)
{
	struct LazyUSF_TagHolder *holder = (struct LazyUSF_TagHolder *)context;

	if(strcasecmp(name,"title") == 0)
	{
		holder->handler->OnTag(TAG_TITLE,value);
	} else if(strcasecmp(name,"artist") == 0)
	{
		holder->handler->OnTag(TAG_ARTIST,value);
	} else if(strcasecmp(name,"game") == 0)
	{
		holder->handler->OnTag(TAG_ALBUM,value);
	} else if(strcasecmp(name,"year") == 0)
	{
		holder->handler->OnTag(TAG_DATE,value);
	} else if(strcasecmp(name,"track") == 0)
	{
		holder->handler->OnTag(TAG_TRACK,value);
	}

	if(strcasecmp(name,"length") == 0)
	{
		holder->length = ParseUSFTime(value);
	} else if(strcasecmp(name,"fade") == 0)
	{
		holder->fade = ParseUSFTime(value);
	} else if(strcasecmp(name,"_enablecompare") == 0 && strlen(value)) {
		holder->enable_compare = 1;
	} else if(strcasecmp(name,"_enablefifofull") == 0 && strlen(value)) {
		holder->enable_fifo_full = 1;
	}

	return 0;
}

static int
LazyUSF_Loader(void *context, const uint8_t *exe,
		size_t exe_size, const uint8_t *reserved,
		size_t reserved_size)
{
	(void)exe;
	(void)exe_size;
	usf_state_t *usf = (usf_state_t *)context;
	return usf_upload_section(usf, reserved, reserved_size);
}

static bool
LazyUSF_openfile(void *context, Path path_fs,
    struct LazyUSF_TagHolder *holder)
{
	usf_state_t *usf = (usf_state_t *)context;

	usf_clear(usf);

	if(psf_load(path_fs.c_str(),
	  &LazyUSF_psf_callbacks, 0x21,
	  LazyUSF_Loader, usf,
	  LazyUSF_TagHandler,holder,0,
	  nullptr,nullptr) < 0)
	{
		LogWarning(lazyusf_domain,"error loading file");
		return false;
	}

	usf_set_compare(usf,holder->enable_compare);
	usf_set_fifo_full(usf,holder->enable_fifo_full);
	usf_set_hle_audio(usf,enable_hle);

	if(holder->length > 0)
	{
		holder->handler->OnDuration(
				SongTime::FromMS(holder->length + holder->fade));
	}

	return true;
}

static int
LazyUSF_ApplyFade(int16_t *buf, int64_t song_samples,
	int64_t rem_samples, int64_t fade_samples)
{
	unsigned int i = 0;
	if(song_samples > 0)
	{
		return 0;
	}

	for(i = 0 - song_samples; i<LAZYUSF_BUFFER_FRAMES; i++)
	{
		if(i > rem_samples)
		{
			buf[i*2] = 0;
			buf[i*2 + 1] = 0;
		}
		else {
			buf[i*2] = FadeUSFSample(buf[i*2],rem_samples - i - song_samples,fade_samples);
			buf[i*2 + 1] = FadeUSFSample(buf[i*2 + 1],rem_samples - i - song_samples,fade_samples);
		}
	}
	return LAZYUSF_BUFFER_FRAMES;
}

static bool
lazyusf_plugin_init(const ConfigBlock &block)
{
	auto hle = block.GetBlockParam("hle");
	auto sr = block.GetBlockParam("sample_rate");
	enable_hle = hle != nullptr
		? (int)hle->GetBoolValue() : 1;
	sample_rate = sr != nullptr
		? sr->GetIntValue() : 0;

	return true;

}

static bool
lazyusf_scan_file(Path path_fs, TagHandler &handler)
{
	struct LazyUSF_TagHolder holder(&handler);

	usf_state_t *usf =
		(usf_state_t *)malloc(usf_get_state_size());
	if(!usf)
	{
		LogWarning(lazyusf_domain,"out of memory");
		return false;
	}

	AtScopeExit(usf)
	{
		usf_shutdown(usf);
		free(usf);
	};

	return LazyUSF_openfile(usf,path_fs,&holder);
}

static void
lazyusf_file_decode(DecoderClient &client, Path path_fs)
{
	TagBuilder tag_builder;
	AddTagHandler h(tag_builder);
	struct LazyUSF_TagHolder holder(&h);

	const char *usf_err = nullptr;
	int8_t resample = true;

	usf_state_t *usf =
		(usf_state_t *)malloc(usf_get_state_size());

	if(!usf)
	{
		LogWarning(lazyusf_domain,"out of memory");
		return;
	}

	AtScopeExit(usf)
	{
		usf_shutdown(usf);
		free(usf);
	};

	if(!LazyUSF_openfile(usf,path_fs,&holder)) {
		return;
	}

	/* get sample rate */
    	if(sample_rate <= 0) {
		resample = false;
		usf_err = usf_render(usf,nullptr,0,&sample_rate);
		if(usf_err != nullptr)
		{
			LogWarning(lazyusf_domain,usf_err);
			return;
		}
	}

	const SignedSongTime song_len = holder.length > 0
		? SignedSongTime::FromMS(holder.length + holder.fade)
		: SignedSongTime::Negative();

	const auto audio_format = CheckAudioFormat(sample_rate,
		SampleFormat::S16,
		LAZYUSF_CHANNELS);

	client.Ready(audio_format,true,song_len);

	DecoderCommand cmd;
	int16_t buf[LAZYUSF_BUFFER_SAMPLES];
	int64_t song_samples = (int64_t)((((uint64_t)holder.length) * sample_rate) / 1000);
	int64_t fade_samples = (int64_t)((((uint64_t)holder.fade) * sample_rate) / 1000);
	int64_t rem_samples = fade_samples;

	do
	{
		usf_err = resample
		  ? usf_render_resampled(usf,buf,LAZYUSF_BUFFER_FRAMES,sample_rate)
		  : usf_render(usf,buf,LAZYUSF_BUFFER_FRAMES,&sample_rate);
		if(usf_err != nullptr)
		{
			LogWarning(lazyusf_domain,usf_err);
		}

		if(song_samples > 0)
		{
		  song_samples -= LAZYUSF_BUFFER_FRAMES;
		}

		if(song_samples <= 0)
		{
			rem_samples -= 
				LazyUSF_ApplyFade(buf,song_samples,rem_samples,fade_samples);
			song_samples = 0;
		}

		if(song_samples == 0 && rem_samples <= 0) break;

		cmd = client.SubmitData(nullptr,buf,sizeof(buf),0);

		if (cmd == DecoderCommand::SEEK)
		{

			int64_t where = (int64_t)((((uint64_t)holder.length - client.GetSeekTime().ToMS()) * sample_rate) / 1000);

			if(where > song_samples) {
				usf_restart(usf);
				song_samples = (int64_t)((((uint64_t)holder.length) * sample_rate) / 1000);
				fade_samples = (int64_t)((((uint64_t)holder.fade) * sample_rate) / 1000);
				rem_samples = fade_samples;
			}

			while(song_samples > where && song_samples >= 0) {
				usf_err = resample
				  ? usf_render_resampled(usf,buf,LAZYUSF_BUFFER_FRAMES,sample_rate)
				  : usf_render(usf,buf,LAZYUSF_BUFFER_FRAMES,&sample_rate);
				if(usf_err != nullptr) {
					LogWarning(lazyusf_domain,usf_err);
					return;
				}
				song_samples -= LAZYUSF_BUFFER_FRAMES;
			}

			if(where < 0) {
				rem_samples += where;
				song_samples = 0;
			}
			client.CommandFinished();

		}
	} while(cmd != DecoderCommand::STOP);

}

static const char *const lazyusf_suffixes[] = {
	"miniusf", nullptr
};

constexpr DecoderPlugin lazyusf_decoder_plugin =
	DecoderPlugin("lazyusf", lazyusf_file_decode, lazyusf_scan_file)
	.WithInit(lazyusf_plugin_init)
	.WithSuffixes(lazyusf_suffixes);
