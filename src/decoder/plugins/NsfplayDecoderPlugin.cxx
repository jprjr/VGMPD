#include "NsfplayDecoderPlugin.hxx"
#include "../DecoderAPI.hxx"
#include "CheckAudioFormat.hxx"
#include "song/DetachedSong.hxx"
#include "tag/Handler.hxx"
#include "tag/Builder.hxx"
#include "fs/Path.hxx"
#include "fs/AllocatedPath.hxx"
#include "util/ScopeExit.hxx"
#include "util/StringFormat.hxx"
#include "util/UriUtil.hxx"
#include "util/Domain.hxx"
#include "Log.hxx"

#include <nsfplay/nsfplay.h>

#include <cstring>

#define SUBTUNE_PREFIX "tune_"

static constexpr Domain nsfplay_domain("nsfplay");

static constexpr unsigned NSFPLAY_CHANNELS = 2;
static constexpr unsigned NSFPLAY_BUFFER_FRAMES = 2048;
static constexpr unsigned NSFPLAY_BUFFER_SAMPLES =
	NSFPLAY_BUFFER_FRAMES * NSFPLAY_CHANNELS;

struct NsfplayContainerPath {
	AllocatedPath path;
	unsigned track;
};

static unsigned nsfplay_gain;
static unsigned nsfplay_sample_rate;
static unsigned nsfplay_default_length;
static unsigned nsfplay_default_fade;
static unsigned nsfplay_default_loops;

gcc_pure
static unsigned
ParseSubtuneName(const char *base) noexcept
{
	if (memcmp(base, SUBTUNE_PREFIX, sizeof(SUBTUNE_PREFIX) - 1) != 0)
		return 0;

	base += sizeof(SUBTUNE_PREFIX) - 1;

	char *endptr;
	auto track = strtoul(base, &endptr, 10);
	if (endptr == base || *endptr != '.')
		return 0;

	return track;
}

static NsfplayContainerPath
ParseContainerPath(Path path_fs)
{
	const Path base = path_fs.GetBase();
	unsigned track;
	if (base.IsNull() ||
	    (track = ParseSubtuneName(base.c_str())) < 1)
		return { AllocatedPath(path_fs), 0 };

	return { path_fs.GetDirectoryName(), track - 1 };
}

static xgm::NSF*
LoadNSF(Path path_fs)
{
	xgm::NSF *nsf = new xgm::NSF();
	fprintf(stderr,"loading: %s\n",path_fs.c_str());
	if(!nsf->LoadFile(path_fs.c_str())) {
		LogWarning(nsfplay_domain,nsf->LoadError());
		delete nsf;
		return nullptr;
	}

	nsf->SetDefaults(nsfplay_default_length,
		nsfplay_default_fade,
		nsfplay_default_loops);

	return nsf;
}

static bool
nsfplay_plugin_init(const ConfigBlock &block)
{
	auto gain = block.GetBlockParam("gain");

	nsfplay_gain = gain != nullptr
		? gain->GetUnsignedValue() : 0x100;

	nsfplay_default_length = 180 * 1000;
	nsfplay_default_fade   = 8   * 1000;
	nsfplay_default_loops  = 2;
	nsfplay_sample_rate    = 48000;

	return true;
}

static void
nsfplay_file_decode(DecoderClient &client, Path path_fs)
{
	const auto container = ParseContainerPath(path_fs);
	xgm::NSF* nsf;
	xgm::NSFPlayer* player;
	xgm::NSFPlayerConfig* config;
	uint8_t track = 0;

	nsf = LoadNSF(container.path);
	if(nsf == nullptr) return;

	AtScopeExit(nsf) {
		delete nsf;
	};

	player = new xgm::NSFPlayer();
	config = new xgm::NSFPlayerConfig();

	AtScopeExit(player) {
		delete player;
	};

	AtScopeExit(config) {
		delete config;
	};

	if(nsf->nsfe_plst_size > 0) {
		if(container.track >= nsf->nsfe_plst_size) {
			track = nsf->nsfe_plst[nsf->nsfe_plst_size-1];
		} else {
			track = nsf->nsfe_plst[container.track];
		}
	} else {
		track = container.track;
	}

	nsf->SetSong(track);

	const int length = nsf->GetLength();
	uint64_t total_frames = (uint64_t)length;
	total_frames *= nsfplay_sample_rate;
	total_frames /= 1000;
	uint64_t frames = total_frames;

	(*config)["MASTER_VOLUME"] = nsfplay_gain;

	player->SetConfig(config);
	if(!player->Load(nsf)) {
		return;
	}

	player->SetChannels(NSFPLAY_CHANNELS);
	player->SetPlayFreq(nsfplay_sample_rate);
	player->Reset();

	const auto audio_format = CheckAudioFormat(nsfplay_sample_rate,
							SampleFormat::S16,
							NSFPLAY_CHANNELS);
	client.Ready(audio_format,true,SongTime::FromMS(length));

	DecoderCommand cmd;
	do {
		int16_t buffer[NSFPLAY_BUFFER_SAMPLES];
		memset(buffer,0,sizeof(buffer));
		
		unsigned int fc = frames > NSFPLAY_BUFFER_FRAMES ? NSFPLAY_BUFFER_FRAMES : frames;
		player->Render(buffer,fc);
		frames -= fc;

		cmd = client.SubmitData(nullptr,buffer,sizeof(buffer),0);
		if(cmd == DecoderCommand::SEEK) {
			uint64_t where = client.GetSeekTime().ToMS();
			where *= nsfplay_sample_rate;
			where /= 1000;

			uint64_t cur = total_frames - frames;
			if(where > cur) {
				player->Skip(where - cur);
			} else {
				player->Reset();
				player->Skip(where);
			}
			frames = total_frames - where;
			client.CommandFinished();
		}

		if(frames == 0) break;
	} while(cmd != DecoderCommand::STOP);
}

static void
ScanMusic(xgm::NSF *nsf, unsigned track, TagHandler &handler) noexcept
{
	uint8_t nsfe_track;
	unsigned int track_max;
	if(nsf->nsfe_plst_size > 0) {
		track_max = nsf->nsfe_plst_size;
		if(track >= nsf->nsfe_plst_size) {
			nsfe_track = nsf->nsfe_plst[nsf->nsfe_plst_size-1];
		} else {
			nsfe_track = nsf->nsfe_plst[track];
		}
	} else {
		nsfe_track = track;
		track_max = nsf->GetSongNum();
	}
	nsf->SetSong(nsfe_track);

	handler.OnDuration(SongTime::FromMS(nsf->GetLength()));
	handler.OnTag(TAG_TRACK, StringFormat<16>("%u",track + 1));

	if(nsf->artist[0])
		handler.OnTag(TAG_ARTIST,nsf->artist);

	if(nsf->title[0])
		handler.OnTag(TAG_ALBUM,nsf->title);
	
	if(nsf->nsfe_entry[nsfe_track].tlbl[0]) {
		handler.OnTag(TAG_TITLE,nsf->nsfe_entry[nsfe_track].tlbl);
	}
	else {
		if(nsf->title[0]) {
			const auto title =
				StringFormat<1024>("%s (%u/%u)",
					nsf->title,
					track+1,
					track_max);
			handler.OnTag(TAG_TITLE,title);
		}
	}

}

static bool
nsfplay_scan_file(Path path_fs, TagHandler &handler) noexcept
{
	const auto container = ParseContainerPath(path_fs);
	xgm::NSF* nsf = LoadNSF(container.path);
	if(nsf == nullptr) return false;

	AtScopeExit(nsf) {
		delete nsf;
	};

	ScanMusic(nsf,container.track,handler);
	return true;
}

static std::forward_list<DetachedSong>
nsfplay_container_scan(Path path_fs)
{
	std::forward_list<DetachedSong> list;
	const auto container = ParseContainerPath(path_fs);
	xgm::NSF* nsf = LoadNSF(container.path);
	if(nsf == nullptr) {
		return list;
	}

	AtScopeExit(nsf) {
		delete nsf;
	};

	const unsigned total = nsf->nsfe_plst_size > 0
		? nsf->nsfe_plst_size : nsf->songs;

	const char *subtune_suffix = uri_get_suffix(path_fs.c_str());

	TagBuilder tag_builder;

	auto tail = list.before_begin();
	for(unsigned int i = 0; i < total; ++i) {
		AddTagHandler h(tag_builder);
		ScanMusic(nsf,i,h);

		const auto track_name =
			StringFormat<64>(SUBTUNE_PREFIX "%03u.%s",i+1,subtune_suffix);
		tail = list.emplace_after(tail,track_name,
			tag_builder.Commit());
	}
	return list;
}

static const char *const nsfplay_suffixes[] = {
	"nsf", "nsfe", nullptr
};

const struct DecoderPlugin nsfplay_decoder_plugin = {
	"nsfplay",
	nsfplay_plugin_init,
	nullptr,
	nullptr,
	nsfplay_file_decode,
	nsfplay_scan_file,
	nullptr,
	nsfplay_container_scan,
	nsfplay_suffixes,
	nullptr,
};
