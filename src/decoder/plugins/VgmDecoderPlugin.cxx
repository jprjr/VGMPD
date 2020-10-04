#include "VgmDecoderPlugin.hxx"
#include "../DecoderAPI.hxx"
#include "CheckAudioFormat.hxx"
#include "input/InputStream.hxx"
#include "tag/Handler.hxx"
#include "util/ScopeExit.hxx"

#include <vgm/player/playerbase.hpp>
#include <vgm/player/vgmplayer.hpp>
#include <vgm/player/s98player.hpp>
#include <vgm/player/droplayer.hpp>
#include <vgm/utils/DataLoader.h>
#include <vgm/utils/MemoryLoader.h>

#include <cstring>

static constexpr unsigned VGM_CHANNELS = 2;
static constexpr unsigned VGM_BUFFER_FRAMES = 2048;
static constexpr unsigned VGM_BUFFER_SAMPLES =
	VGM_BUFFER_FRAMES * VGM_CHANNELS;
static constexpr signed VGM_MAX_SAMPLE = 8388607;
static constexpr signed VGM_MIN_SAMPLE = -8388608;

static unsigned vgm_sample_rate;
static unsigned vgm_bit_depth;
static unsigned vgm_byte_width;
static unsigned vgm_fade_len;
static void (*PackFrames)(void *dest, WAVE_32BS *src, unsigned int count);

static void
FadeFrames(WAVE_32BS *data, unsigned int frames_rem, unsigned int frames_fade, unsigned int frame_count) {
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
        data[i].L *= fade;
        data[i].R *= fade;
        i++;
    }

    return;
}

static void
PackFrames16(void *d, WAVE_32BS *src, unsigned int count) {
	unsigned int i = 0;
	unsigned int j = 0;
	int16_t *dest = (int16_t *)d;

	while(i<count) {

		/* check for clipping */
		if(src[i].L < VGM_MIN_SAMPLE) {
			src[i].L = VGM_MIN_SAMPLE;
		} else if(src[i].L > VGM_MAX_SAMPLE) {
			src[i].L = VGM_MAX_SAMPLE;
		}
		if(src[i].R < VGM_MIN_SAMPLE) {
			src[i].R = VGM_MIN_SAMPLE;
		} else if(src[i].R > VGM_MAX_SAMPLE) {
			src[i].R = VGM_MAX_SAMPLE;
		}

		dest[j]   = (int16_t)((uint32_t)src[i].L >> 8);
		dest[j+1] = (int16_t)((uint32_t)src[i].R >> 8);

		i++;
		j+=2;
	}
}

static void
PackFrames24(void *d, WAVE_32BS *src, unsigned int count) {
	unsigned int i = 0;
	unsigned int j = 0;
	int32_t *dest = (int32_t *)d;
	while(i<count) {

		/* check for clipping */
		if(src[i].L < VGM_MIN_SAMPLE) {
			src[i].L = VGM_MIN_SAMPLE;
		} else if(src[i].L > VGM_MAX_SAMPLE) {
			src[i].L = VGM_MAX_SAMPLE;
		}
		if(src[i].R < VGM_MIN_SAMPLE) {
			src[i].R = VGM_MIN_SAMPLE;
		} else if(src[i].R > VGM_MAX_SAMPLE) {
			src[i].R = VGM_MAX_SAMPLE;
		}

		dest[j]    = src[i].L;
		dest[j+1]  = src[i].R;

		i++;
		j+=2;
	}
}

static bool
GetPlayerForFile(DATA_LOADER *loader, PlayerBase** retPlayer) {
	PlayerBase *player;
	if(!VGMPlayer::PlayerCanLoadFile(loader)) player = new VGMPlayer();
	else if(!S98Player::PlayerCanLoadFile(loader)) player = new S98Player();
	else if(!DROPlayer::PlayerCanLoadFile(loader)) player = new DROPlayer();
	else return false;
	if(player->LoadFile(loader)) {
		delete player;
		return false;
	}
	player->SetSampleRate(vgm_sample_rate);
	*retPlayer = player;
	return true;
}

static PlayerBase*
LoadVgm(InputStream &is, DATA_LOADER **out_loader, uint8_t **out_data) {
	PlayerBase *player;
	DATA_LOADER *loader;
	size_t size = 0;
	size_t read = 0;
	uint8_t *d;
	uint8_t *t;
	uint8_t *data = (uint8_t *)malloc(4096);
	if(data == nullptr) return nullptr;
	d = &data[size];

	do {
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
	} while(read == 4096 && (size + 4096) < ((size_t)(-1)));

	loader = MemoryLoader_Init(data,size);
	if(loader == nullptr) {
		free(data);
		return nullptr;
	}
	*out_loader = loader;
	*out_data = data;

	DataLoader_SetPreloadBytes(loader,0x100);
	if(DataLoader_Load(loader) || (!GetPlayerForFile(loader,&player))) {
		DataLoader_CancelLoading(loader);
		DataLoader_Deinit(loader);
		free(data);
		return nullptr;
	}

	return player;
}

static bool
vgm_plugin_init(const ConfigBlock &block)
{
	auto sample_rate = block.GetBlockParam("sample_rate");
	auto bit_depth = block.GetBlockParam("bit_depth");
	auto fade_len = block.GetBlockParam("fade_len");

	vgm_sample_rate = sample_rate != nullptr
		? sample_rate->GetUnsignedValue() : 44100;

	vgm_bit_depth = bit_depth != nullptr
		? bit_depth->GetUnsignedValue() : 16;

	switch(vgm_bit_depth) {
		case 16: {
			PackFrames = PackFrames16;
			vgm_byte_width = 2;
			break;
		}
		case 24: {
			PackFrames = PackFrames24;
			vgm_byte_width = 4;
			break;
		}
		default: {
			PackFrames = PackFrames16;
			vgm_bit_depth = 16;
			vgm_byte_width = 2;
			break;
		}
	}
	
	vgm_fade_len = fade_len != nullptr
		? fade_len->GetUnsignedValue() : 8;

	return true;
}

static void
vgm_stream_decode(DecoderClient &client, InputStream &is)
{
	PlayerBase *player;
	DATA_LOADER *loader;
	uint8_t *vgm_data;
	size_t packed_size;

	player = LoadVgm(is,&loader,&vgm_data);
	if(player == nullptr) return;

	AtScopeExit(loader) {
		DataLoader_CancelLoading(loader);
		DataLoader_Deinit(loader);
	};

	AtScopeExit(vgm_data) {
		free(vgm_data);
	};

	AtScopeExit(player) {
	    player->UnloadFile();
		delete player;
	};

	player->Start();

	packed_size = VGM_BUFFER_SAMPLES * vgm_byte_width;

	uint64_t total_frames = (uint64_t)player->Tick2Sample(player->GetTotalPlayTicks(2));
	unsigned int frames_fade = 0;
	if(player->GetLoopTicks()) {
		frames_fade = (vgm_sample_rate * vgm_fade_len);
		total_frames += frames_fade;
	}

	uint64_t frames = total_frames;
	uint64_t millis  = total_frames;
	millis *= 1000;
	millis /= vgm_sample_rate;

	const SignedSongTime song_len = SignedSongTime::FromMS(millis);

	const auto audio_format = CheckAudioFormat(vgm_sample_rate,
							vgm_bit_depth == 16 ? SampleFormat::S16 : SampleFormat::S24_P32,
							VGM_CHANNELS);
	client.Ready(audio_format, true, song_len);

	DecoderCommand cmd;
	do {
		WAVE_32BS buffer[VGM_BUFFER_FRAMES];
		int32_t  packed[VGM_BUFFER_SAMPLES];

		memset(buffer,0,sizeof(buffer));
		memset(packed,0,sizeof(packed));

		unsigned int fc = frames > VGM_BUFFER_FRAMES ? VGM_BUFFER_FRAMES : frames;
		player->Render(fc,buffer);
		FadeFrames(buffer,frames,frames_fade,fc);
		PackFrames(packed,buffer,fc);
		frames -= fc;

		cmd = client.SubmitData(nullptr,packed,packed_size,0);
		if(cmd == DecoderCommand::SEEK) {
			uint64_t where = client.GetSeekTime().ToMS();
			where *= vgm_sample_rate;
			where /= 1000;
			if(player->Seek(PLAYPOS_SAMPLE,where)) {
				client.SeekError();
			} else {
				frames = total_frames - where;
				client.CommandFinished();
			}
		}

		if(frames == 0) break;
	} while (cmd != DecoderCommand::STOP);

}

static bool
vgm_scan_stream(InputStream &is, TagHandler &handler) noexcept
{
	PlayerBase *player;
	DATA_LOADER *loader;
	uint8_t *vgm_data;
	player = LoadVgm(is,&loader,&vgm_data);
	if(player == nullptr) return false;

	AtScopeExit(loader) {
		DataLoader_CancelLoading(loader);
		DataLoader_Deinit(loader);
	};

	AtScopeExit(vgm_data) {
		free(vgm_data);
	};

	AtScopeExit(player) {
	    player->UnloadFile();
		delete player;
	};

	player->Start();

	const char* const* tagList = player->GetTags();
	for(const char* const *t = tagList; *t; t += 2) {
		if(!strcmp(t[0],"TITLE"))
			handler.OnTag(TAG_TITLE,t[1]);
		else if(!strcmp(t[0],"ARTIST"))
			handler.OnTag(TAG_ARTIST,t[1]);
		else if(!strcmp(t[0],"GAME"))
			handler.OnTag(TAG_ALBUM,t[1]);
		else if(!strcmp(t[0],"DATE"))
			handler.OnTag(TAG_DATE,t[1]);
		else if(!strcmp(t[0],"COMMENT"))
			handler.OnTag(TAG_COMMENT,t[1]);
	}

	uint64_t frames = (uint64_t)player->Tick2Sample(player->GetTotalPlayTicks(2));
	if(player->GetLoopTicks()) {
		frames += (vgm_sample_rate * vgm_fade_len);
	}

	uint64_t millis = frames;
	millis *= 1000;
	millis /= vgm_sample_rate;

	handler.OnDuration(SongTime::FromMS(millis));

	return true;
}

static const char *const vgm_suffixes[] = {
	"dro", "s98", "vgm", "vgz",
	nullptr
};

const struct DecoderPlugin vgm_decoder_plugin = {
	"vgm",
	vgm_plugin_init,
	nullptr,
	vgm_stream_decode,
	nullptr,
	nullptr,
	vgm_scan_stream,
	nullptr,
	vgm_suffixes,
	nullptr,
};
