/* vim: set tabstop=4 softtabstop=4 shiftwidth=4 noexpandtab : */

#include "config.h"
#include "LazyusfDecoderPlugin.hxx"
#include "../DecoderAPI.hxx"
#include "CheckAudioFormat.hxx"
#include "tag/TagHandler.hxx"
#include "tag/TagBuilder.hxx"
#include "fs/Path.hxx"
#include "util/ScopeExit.hxx"
#include "util/Domain.hxx"
#include "util/StringFormat.hxx"
#include "Log.hxx"

#include <psflib/psflib.h>
#include <lazyusf/usf.h>
#include <stdint.h>

static constexpr Domain lazyusf_domain("lazyusf");

static constexpr unsigned LAZYUSF_CHANNELS = 2;
static constexpr unsigned LAZYUSF_BUFFER_FRAMES = 1024;
static constexpr unsigned LAZYUSF_BUFFER_SAMPLES =
  LAZYUSF_BUFFER_FRAMES * LAZYUSF_CHANNELS;

static const char *LazyUSF_separators = "\\/:|";

#define MIN(a,b) (a < b ? a : b)

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
 * parses H:M:S into seconds
 */
gcc_pure
static unsigned
ParseUSFTime(const char *ts)
{
	unsigned int i = 0;
	unsigned int t = 0;
	unsigned int c = 0;
	for(i=0;i<strlen(ts);i++)
	{
		if(ts[i] == ':')
		{
			t *= 60;
			t += c*60;
			c = 0;
		}
		else
		{
			if(ts[i] < 48 || ts[i] > 57)
			{
				return 0;
			}
			c *= 10;
			c += ts[i] - 48;
		}
	}
	return c + t;
}

static void
*LazyUSF_fopen(const char *path)
{
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
	LazyUSF_fopen,
	LazyUSF_fread,
	LazyUSF_fseek,
	LazyUSF_fclose,
	LazyUSF_ftell,
};

struct LazyUSF_TagHolder
{
	unsigned int length;
	unsigned int fade;
	unsigned int track;
	unsigned int enable_compare;
	unsigned int enable_fifo_full;
    const TagHandler &handler;
	void *handler_ctx;
};

static int
LazyUSF_TagHandler(void *context, const char *name, const char *value)
{
	struct LazyUSF_TagHolder *holder = (struct LazyUSF_TagHolder *)context;

	if(holder->handler_ctx != nullptr)
	{
		if(strcasecmp(name,"title") == 0)
		{
			tag_handler_invoke_tag(holder->handler, holder->handler_ctx,
				TAG_TITLE, value);
		} else if(strcasecmp(name,"artist") == 0)
		{
			tag_handler_invoke_tag(holder->handler, holder->handler_ctx,
				TAG_ARTIST, value);
		} else if(strcasecmp(name,"game") == 0)
		{
			tag_handler_invoke_tag(holder->handler, holder->handler_ctx,
				TAG_ALBUM, value);
		} else if(strcasecmp(name,"year") == 0)
		{
			tag_handler_invoke_tag(holder->handler, holder->handler_ctx,
				TAG_DATE, value);
		}
	}

	if(strcasecmp(name,"length") == 0)
	{
		holder->length = ParseUSFTime(value) * 1000;
	} else if(strcasecmp(name,"fade") == 0)
	{
		holder->fade = ParseUSFTime(value) * 1000;
	} else if(strcasecmp(name,"track") == 0)
	{
		holder->track = ParseUSFTime(value);
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

	holder->length = 0;
	holder->fade = 0;
	holder->track = 0;
	holder->enable_compare = 0;
	holder->enable_fifo_full = 0;

	if(psf_load(path_fs.c_str(),
	  &LazyUSF_psf_callbacks, 0x21,
	  LazyUSF_Loader, usf,
	  LazyUSF_TagHandler,holder,0) < 0)
	{
		LogWarning(lazyusf_domain,"error loading file");
		return false;
	}

	usf_set_compare(usf,holder->enable_compare);
	usf_set_fifo_full(usf,holder->enable_fifo_full);

	if(holder->handler_ctx != nullptr)
	{
		if(holder->length > 0)
		{
			tag_handler_invoke_duration(holder->handler,holder->handler_ctx,
					SongTime::FromMS(holder->length + holder->fade));
		}
		if(holder->track > 0)
		{
			tag_handler_invoke_tag(holder->handler, holder->handler_ctx, TAG_TRACK,
					StringFormat<16>("%u",holder->track));
		}
	}

	return true;

}

static bool
lazyusf_scan_file(Path path_fs,
       const TagHandler &handler, void *handler_ctx)
{
	struct LazyUSF_TagHolder holder =
	{
		.handler = handler,
		.handler_ctx = handler_ctx,
	};

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

static void
lazyusf_file_decode(DecoderClient &client, Path path_fs)
{
	struct LazyUSF_TagHolder holder =
	{
		.handler = add_tag_handler,
		.handler_ctx = nullptr,
	};
	int32_t sample_rate = 0;
	const char *usf_err = nullptr;

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
	usf_err = usf_render(usf,nullptr,0,&sample_rate);
	if(usf_err != nullptr)
	{
		LogWarning(lazyusf_domain,usf_err);
		return;
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
	int64_t song_samples = (holder.length / 1000) * sample_rate;
	int64_t fade_samples = (holder.fade / 1000) * sample_rate;
	int64_t rem_samples = fade_samples;

	do
	{
		usf_err = usf_render(usf,buf,LAZYUSF_BUFFER_FRAMES,&sample_rate);
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

			int64_t where = (holder.length - client.GetSeekTime().ToMS()) / 1000 * sample_rate;

			if(where > song_samples) {
			    usf_restart(usf);
	            song_samples = (holder.length / 1000) * sample_rate;
			    fade_samples = (holder.fade / 1000) * sample_rate;
			    rem_samples = fade_samples;
			}

			while(song_samples > where && song_samples >= 0) {
				usf_err = usf_render(usf,buf,LAZYUSF_BUFFER_FRAMES,&sample_rate);
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

const struct DecoderPlugin lazyusf_decoder_plugin = {
	"lazyusf", /* name */
	nullptr,   /* init function */
	nullptr,   /* finish function */
	nullptr,   /* stream_decode */
	lazyusf_file_decode,   /* file_decode */
	lazyusf_scan_file,   /* scan_file */
	nullptr,   /* scan_stream */
	nullptr,   /* container_scan */
	lazyusf_suffixes,
	nullptr,
};

