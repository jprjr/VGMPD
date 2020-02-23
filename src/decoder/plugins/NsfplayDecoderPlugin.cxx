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

/* extension to xgm::NSFPlayerConfig to load from a ConfigBlock */
class NSFPlayerConfigPlugin : public xgm::NSFPlayerConfig
{
	public:
	NSFPlayerConfigPlugin() : NSFPlayerConfig() {}
	~NSFPlayerConfigPlugin() {}
	void Load(const ConfigBlock& block) {
		std::map<std::string, vcm::Value>::iterator it;
		for(it=data.begin(); it != data.end(); it++) {
			auto param = block.GetBlockParam(it->first.c_str());
			if(param != nullptr) {
				data[it->first.c_str()] = param->GetUnsignedValue();
			}
		}
	}
};


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

static NSFPlayerConfigPlugin nsfplay_config;

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

	nsf->SetDefaults(nsfplay_config["PLAY_TIME"],
		nsfplay_config["FADE_TIME"],
		nsfplay_config["LOOP_NUM"]);

	return nsf;
}

static bool
nsfplay_plugin_init(const ConfigBlock &block)
{

	/* set defaults */
	nsfplay_config["RATE"]	  = 48000;
	nsfplay_config["PLAY_TIME"] = 180 * 1000;
	nsfplay_config["FADE_TIME"] = 8   * 1000;
	nsfplay_config["LOOP_NUM"]  = 2;

	nsfplay_config.Load(block);

	/* ensure certain options aren't set */
	nsfplay_config["BPS"] = 16;
	nsfplay_config["NCH"] =  NSFPLAY_CHANNELS;
	nsfplay_config["AUTO_STOP"] = 0;
	nsfplay_config["AUTO_DETECT"] = 0;
	nsfplay_config["NSFE_PLAYLIST"] = 1;
	nsfplay_config["LOG_CPU"] = 0;

	return true;
}

static void
nsfplay_file_decode(DecoderClient &client, Path path_fs)
{
	const auto container = ParseContainerPath(path_fs);
	xgm::NSF* nsf;
	xgm::NSFPlayer* player;

	nsf = LoadNSF(container.path);
	if(nsf == nullptr) return;

	AtScopeExit(nsf) {
		delete nsf;
	};

	player = new xgm::NSFPlayer();

	AtScopeExit(player) {
		delete player;
	};

	nsf->SetSong(container.track);

	const int length = nsf->GetLength();
	uint64_t total_frames = (uint64_t)length;
	total_frames *= nsfplay_config["RATE"];
	total_frames /= 1000;
	uint64_t frames = 0;

	player->SetConfig(&nsfplay_config);
	if(!player->Load(nsf)) {
		return;
	}

	player->SetChannels(NSFPLAY_CHANNELS);
	player->SetPlayFreq(nsfplay_config["RATE"]);
	player->Reset();

	const auto audio_format = CheckAudioFormat(nsfplay_config["RATE"],
							SampleFormat::S16,
							NSFPLAY_CHANNELS);
	client.Ready(audio_format,true,SongTime::FromMS(length));

	DecoderCommand cmd;
	do {
		int16_t buffer[NSFPLAY_BUFFER_SAMPLES];
		memset(buffer,0,sizeof(buffer));
		
		player->Render(buffer,NSFPLAY_BUFFER_FRAMES);
		frames += NSFPLAY_BUFFER_FRAMES;

		cmd = client.SubmitData(nullptr,buffer,sizeof(buffer),0);
		if(cmd == DecoderCommand::SEEK) {
			uint64_t where = client.GetSeekTime().ToMS();
			where *= nsfplay_config["RATE"];
			where /= 1000;

			if(where > frames) {
				player->Skip(where - frames);
			} else {
				player->Reset();
				player->Skip(where);
			}
			frames = where;
			client.CommandFinished();
		}

		if(player->IsStopped()) break;

	} while(cmd != DecoderCommand::STOP);
}

static void
ScanMusic(xgm::NSF *nsf, unsigned track, TagHandler &handler) noexcept
{
	uint8_t nsfe_track;
	unsigned int track_max;

	if(nsf->nsfe_plst_size > 0) {
		track_max = nsf->nsfe_plst_size;
		if(track >= (unsigned)nsf->nsfe_plst_size) {
			nsfe_track = nsf->nsfe_plst[nsf->nsfe_plst_size-1];
		} else {
			nsfe_track = nsf->nsfe_plst[track];
		}
	} else {
		nsfe_track = track;
		track_max = nsf->GetSongNum();
	}

	nsf->SetSong(track);

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
