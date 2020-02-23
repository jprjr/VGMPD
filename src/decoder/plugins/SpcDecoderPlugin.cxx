#include "SpcDecoderPlugin.hxx"
#include "../DecoderAPI.hxx"
#include "CheckAudioFormat.hxx"
#include "input/InputStream.hxx"
#include "tag/Handler.hxx"
#include "util/ScopeExit.hxx"
#include "util/Domain.hxx"
#include "Log.hxx"

#include <id666/id666.h>
#include <snes_spc/spc.h>

#include <cstring>
#include <cstdio>

static constexpr Domain spc_domain("spc");

static const char* SPC_HEADER = "SNES-SPC700 Sound File Data v0.30";
static constexpr unsigned SPC_CHANNELS = 2;
static constexpr unsigned SPC_BUFFER_FRAMES = 2048;
static constexpr unsigned SPC_BUFFER_SAMPLES =
	SPC_BUFFER_FRAMES * SPC_CHANNELS;

static unsigned spc_gain;

static void
FadeFrames(int16_t *data, unsigned int frames_rem, unsigned int frames_fade, unsigned int frame_count) {
    unsigned int i = 0;
    unsigned int f = frames_fade;
    double fade;

    if(frames_rem - frame_count > frames_fade) return;
    if(frames_rem > frames_fade) {
        i = frames_rem - frames_fade;
        f += i;
    } else {
        f = frames_rem;
    }

    while(i<frame_count) {
        fade = (double)(f-i) / (double)frames_fade;
        data[(i*2)+0] *= fade;
        data[(i*2)+1] *= fade;
        i++;
    }

    return;
}

static uint8_t*
LoadStream(InputStream &is,size_t *out_size)
{
	bool first = true;
	size_t size = 0;
	size_t read = 0;
	uint8_t *d;
	uint8_t *t;
	uint8_t *data = (uint8_t *)malloc(4096);
	if(data == nullptr) return nullptr;
	d = &data[size];

	do {
		/* check header */
		read = is.LockRead(d,4096);
		size += read;
		if(read == 4096) {
			t = (uint8_t *)realloc(data,size+4096);
			if(t == nullptr) {
				free(data);
				return nullptr;
			}
			data = t;
			d = &data[size];
		}
		if(first) {
			first = false;
			if(size < 0x100) {
				free(data);
				return nullptr;
			}
			if(memcmp(data,SPC_HEADER,strlen(SPC_HEADER)) != 0) {
				free(data);
				return nullptr;
			}
		}
	} while(read == 4096 && (size + 4096) < ((size_t)(-1)));
	*out_size = size;
	return data;
}

static bool
spc_plugin_init(const ConfigBlock &block)
{
	auto gain = block.GetBlockParam("gain");

	spc_gain = gain != nullptr
		? gain->GetUnsignedValue() : 0x100;

	return true;
}

static void
spc_stream_decode(DecoderClient &client, InputStream &is)
{
	snes_spc_t *spc;
	spc_filter_t *filter;
	id666 *id6;
	uint8_t *spc_data;
	size_t spc_size;
	spc_err_t err;

	spc_data = LoadStream(is,&spc_size);
	if(spc_data == nullptr) return;

	AtScopeExit(spc_data) {
		free(spc_data);
	};

	id6 = (id666 *)malloc(sizeof(id666));
	if(id6 == nullptr) return;

	AtScopeExit(id6) {
		free(id6);
	};

	spc = spc_new();
	if(spc == nullptr) return;

	AtScopeExit(spc) {
		spc_delete(spc);
	};

	filter = spc_filter_new();
	if(filter == nullptr) return;

	AtScopeExit(filter) {
		spc_filter_delete(filter);
	};

	if(id666_parse(id6,spc_data, spc_size)) return;
	
	err = spc_load_spc(spc,spc_data, spc_size);
	if(err != nullptr) {
		LogWarning(spc_domain,err);
		return;
	}

	spc_clear_echo(spc);
	spc_filter_clear(filter);
	spc_filter_set_gain(filter,spc_gain);

	/* total_len is in 1/64000 sec (a "tick").
	 * samplerate is 32000.
	 *
	 * 1 sec          32000 samples   1 sample
	 * --------     * ------        = --------
	 * 64000 ticks    1 sec           2 ticks
	 *
	 * Just divide by 2 to get total frames
	*/
	uint64_t total_frames = (uint64_t)id6->total_len;
	total_frames /= 2;

	uint64_t frames_fade = (uint64_t)id6->fade;
	frames_fade /= 2;

	uint64_t frames = total_frames;

	uint64_t millis = total_frames;
	millis *= 1000;
	millis /= spc_sample_rate;

	const SongTime song_len = SongTime::FromMS(millis);

	const auto audio_format = CheckAudioFormat(spc_sample_rate,
								SampleFormat::S16,
								SPC_CHANNELS);
	client.Ready(audio_format, true, song_len);

	DecoderCommand cmd;
	do {
		int16_t buffer[SPC_BUFFER_SAMPLES];
		memset(buffer,0,sizeof(buffer));

		unsigned int fc = frames > SPC_BUFFER_FRAMES ? SPC_BUFFER_FRAMES : frames;
		spc_play(spc,fc * 2,buffer);
		spc_filter_run(filter,buffer, fc * 2);
		FadeFrames(buffer,frames,frames_fade,fc);
		frames -= fc;

		cmd = client.SubmitData(nullptr,buffer,sizeof(buffer),0);
		if(cmd == DecoderCommand::SEEK) {
			uint64_t where = client.GetSeekTime().ToMS();
			where *= spc_sample_rate;
			where /= 1000;

			uint64_t cur = total_frames - frames;

			if(where > cur) {
				spc_skip(spc,(where - cur) * 2);
			} else {
				spc_load_spc(spc,spc_data,spc_size);
				spc_skip(spc,where * 2);
			}
			frames = total_frames - where;
			client.CommandFinished();
		}

		if(frames == 0) break;
	} while(cmd != DecoderCommand::STOP);
}


static bool
spc_scan_stream(InputStream &is, TagHandler &handler) noexcept
{
	id666 *id6;
	uint8_t *spc_data;
	size_t spc_size;
	char year[5];

	spc_data = LoadStream(is,&spc_size);
	if(spc_data == nullptr) return false;

	id6 = (id666 *)malloc(sizeof(id666));
	if(id6 == nullptr) return false;

	AtScopeExit(id6) {
		free(id6);
	};

	if(id666_parse(id6,spc_data, spc_size)) return false;

	if(id6->song[0]) {
		handler.OnTag(TAG_TITLE,id6->song);
	}

	if(id6->game[0]) {
		handler.OnTag(TAG_ALBUM,id6->game);
	}

	if(id6->comment[0]) {
		handler.OnTag(TAG_COMMENT,id6->comment);
	}

	if(id6->artist[0]) {
		handler.OnTag(TAG_ARTIST,id6->artist);
	}

	if(id6->year != -1) {
		snprintf(year,5,"%d",id6->year);
		handler.OnTag(TAG_DATE,year);
	}

	uint64_t total_frames = (uint64_t)id6->total_len;
	total_frames /= 2;

	uint64_t frames_fade = (uint64_t)id6->fade;
	frames_fade /= 2;

	uint64_t millis = total_frames;
	millis *= 1000;
	millis /= spc_sample_rate;

	const SongTime song_len = SongTime::FromMS(millis);

	handler.OnDuration(song_len);

	return true;
}

static const char *const spc_suffixes[] = {
	"spc", nullptr,
};

const struct DecoderPlugin spc_decoder_plugin = {
	"spc",
	spc_plugin_init,
	nullptr,
	spc_stream_decode,
	nullptr,
	nullptr,
	spc_scan_stream,
	nullptr,
	spc_suffixes,
	nullptr,
};
