#include "VgmDecoderPlugin.hxx"
#include "../DecoderAPI.hxx"
#include "CheckAudioFormat.hxx"
#include "fs/Path.hxx"
#include "tag/Handler.hxx"
#include "util/ScopeExit.hxx"

#include <vgm/player/playerbase.hpp>
#include <vgm/player/vgmplayer.hpp>
#include <vgm/player/s98player.hpp>
#include <vgm/player/droplayer.hpp>
#include <vgm/utils/DataLoader.h>
#include <vgm/utils/FileLoader.h>

#include <cstring>

static constexpr unsigned VGM_SAMPLE_RATE = 44100;
static constexpr unsigned VGM_CHANNELS = 2;
static constexpr unsigned VGM_BUFFER_FRAMES = 2048;
static constexpr unsigned VGM_BUFFER_SAMPLES =
	VGM_BUFFER_FRAMES * VGM_CHANNELS;
static constexpr unsigned VGM_FADE_LEN = 8;

static void
FadeSamples(WAVE_32BS *data, unsigned int samples_rem, unsigned int samples_fade, unsigned int sample_count) {
    unsigned int i = 0;
    unsigned int f = samples_fade;
    double fade;

    if(samples_rem - sample_count > samples_fade) return;
    if(samples_rem > samples_fade) {
        i = samples_rem - samples_fade;
        f += i;
    } else {
        f = samples_rem;
    }

    while(i<sample_count) {
        fade = (double)(f-i) / (double)samples_fade;
        data[i].L *= fade;
        data[i].R *= fade;
        i++;
    }

    return;
}

static void
PackSamples(int16_t *dest, WAVE_32BS *src, unsigned int count) {
	unsigned int i = 0;
	unsigned int j = 0;
	while(i<count) {
		/* output is 24-bit, shift to 16-bit */
		src[i].L >>= 8;
		src[i].R >>= 8;

		/* check for clipping */
		if(src[i].L < -0x8000) {
			src[i].L = -0x8000;
		} else if(src[i].L > +0x7FFF) {
			src[i].L = +0x7FFF;
		}
		if(src[i].R < -0x8000) {
			src[i].R = -0x8000;
		} else if(src[i].R > +0x7FFF) {
			src[i].R = +0x7FFF;
		}

		dest[j]   = (int16_t)src[i].L;
		dest[j+1] = (int16_t)src[i].R;

		i++;
		j+=2;
	}
}

static bool
GetPlayerForFile(DATA_LOADER *loader, PlayerBase** retPlayer) {
	PlayerBase *player;
	if(!VGMPlayer::IsMyFile(loader)) player = new VGMPlayer();
	else if(!S98Player::IsMyFile(loader)) player = new S98Player();
	else if(!DROPlayer::IsMyFile(loader)) player = new DROPlayer();
	else return false;
	if(player->LoadFile(loader)) {
		delete player;
		return false;
	}
	player->SetSampleRate(VGM_SAMPLE_RATE);
	*retPlayer = player;
	return true;
}

static PlayerBase*
LoadVgm(Path path_fs, DATA_LOADER **out_loader) {
	PlayerBase *player;
	DATA_LOADER *loader = FileLoader_Init(path_fs.c_str());
	if(loader == nullptr) return nullptr;
	*out_loader = loader;

	DataLoader_SetPreloadBytes(loader,0x100);
	if(DataLoader_Load(loader) || (!GetPlayerForFile(loader,&player))) {
		DataLoader_CancelLoading(loader);
		DataLoader_Deinit(loader);
		return nullptr;
	}

	return player;
}


static void
vgm_file_decode(DecoderClient &client, Path path_fs)
{
	PlayerBase *player;
	DATA_LOADER *loader;
	player = LoadVgm(path_fs,&loader);
	if(player == nullptr) return;

	AtScopeExit(loader) {
		DataLoader_CancelLoading(loader);
		DataLoader_Deinit(loader);
	};

	AtScopeExit(player) {
	    player->UnloadFile();
		delete player;
	};

	player->Start();

	uint64_t total_samples = (uint64_t)player->Tick2Sample(player->GetTotalPlayTicks(2));
	unsigned int samples_fade = 0;
	if(player->GetLoopTicks()) {
		samples_fade = (VGM_SAMPLE_RATE * VGM_FADE_LEN);
		total_samples += samples_fade;
	}
	
	uint64_t samples = total_samples;
	uint64_t millis  = total_samples;
	millis *= 1000;
	millis /= VGM_SAMPLE_RATE;

	const SignedSongTime song_len = SignedSongTime::FromMS(millis);

	const auto audio_format = CheckAudioFormat(VGM_SAMPLE_RATE,
							SampleFormat::S16,
							VGM_CHANNELS);
	client.Ready(audio_format, true, song_len);

	DecoderCommand cmd;
	do {
		WAVE_32BS buffer[VGM_BUFFER_SAMPLES];
		int16_t  packed[VGM_BUFFER_SAMPLES * VGM_CHANNELS];

		memset(buffer,0,sizeof(buffer));
		memset(packed,0,sizeof(packed));

		unsigned int fc = samples > VGM_BUFFER_SAMPLES ? VGM_BUFFER_SAMPLES : samples;
		player->Render(fc,buffer);
		FadeSamples(buffer,samples,samples_fade,fc);
		PackSamples(packed,buffer,fc);
		samples -= fc;

		cmd = client.SubmitData(nullptr,packed,sizeof(packed),0);
		if(cmd == DecoderCommand::SEEK) {
			uint64_t where = client.GetSeekTime().ToMS();
			where *= VGM_SAMPLE_RATE;
			where /= 1000;
			if(player->Seek(PLAYPOS_SAMPLE,where)) {
				client.SeekError();
			} else {
				samples = total_samples - where;
				client.CommandFinished();
			}
		}

		if(samples == 0) break;
	} while (cmd != DecoderCommand::STOP);

}

static bool
vgm_scan_file(Path path_fs, TagHandler &handler) noexcept
{
	PlayerBase *player;
	DATA_LOADER *loader;
	player = LoadVgm(path_fs,&loader);
	if(player == nullptr) return false;

	AtScopeExit(loader) {
		DataLoader_CancelLoading(loader);
		DataLoader_Deinit(loader);
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

	uint64_t samples = (uint64_t)player->Tick2Sample(player->GetTotalPlayTicks(2));
	if(player->GetLoopTicks()) {
		samples += (VGM_SAMPLE_RATE * VGM_FADE_LEN);
	}

	uint64_t millis = samples;
	millis *= 1000;
	millis /= VGM_SAMPLE_RATE;

	handler.OnDuration(SongTime::FromMS(millis));

	return true;
}

static const char *const vgm_suffixes[] = {
	"dro", "s98", "vgm", "vgz",
	nullptr
};

const struct DecoderPlugin vgm_decoder_plugin = {
	"vgm",
	nullptr,
	nullptr,
	nullptr,
	vgm_file_decode,
	vgm_scan_file,
	nullptr,
	nullptr,
	vgm_suffixes,
	nullptr,
};
