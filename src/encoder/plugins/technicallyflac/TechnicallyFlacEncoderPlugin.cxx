#include "TechnicallyFlacEncoderPlugin.hxx"
#include "../OggEncoder.hxx"
#include "util/Alloc.hxx"
#include "util/ByteOrder.hxx"
#include "util/StringUtil.hxx"

#include "technicallyflac.h"
#include <ogg/ogg.h>

#include <stdint.h>
#include <stdlib.h>

struct tf_membuffer_s {
	uint32_t pos;
	uint32_t len;
	uint8_t *buf;
};

typedef struct tf_membuffer_s tf_membuffer;

static int
tf_membuffer_write(uint8_t *bytes, uint32_t len, void *userdata) {
	tf_membuffer *mem = (tf_membuffer *)userdata;
	if(mem->pos + len > mem->len) return -1;
	memcpy(&mem->buf[mem->pos],bytes,len);
	mem->pos += len;
	return 0;
}

typedef unsigned (*pcm_conv)(int32_t *, const void *, unsigned);

static unsigned
pcm8_to_pcm32(int32_t *out, const void *_in, unsigned num_samples)
{
	const int8_t *in = (const int8_t *)_in;
	const unsigned r = num_samples;
	while (num_samples > 0) {
		*out++ = *in++;
		--num_samples;
	}
	return r;
}

static unsigned
pcm16_to_pcm32(int32_t *out, const void *_in, unsigned num_samples)
{
	const int16_t *in = (const int16_t *)_in;
	const unsigned r = num_samples * sizeof(int16_t);
	while (num_samples > 0) {
		*out++ = *in++;
		--num_samples;
	}
	return r;
}

static unsigned
pcm32_to_pcm32(int32_t *out, const void *_in, unsigned num_samples)
{
	const int32_t *in = (const int32_t *)_in;
	const unsigned r = num_samples * sizeof(int32_t);
	while (num_samples > 0) {
		*out++ = *in++;
		--num_samples;
	}
	return r;
}

class TechnicallyFlacEncoder final : public OggEncoder {
	const AudioFormat audio_format;
	technicallyflac *enc;
	tf_membuffer *mem;
	int32_t *const buffer;
	pcm_conv conv;
	size_t frames_position = 0;
	size_t samples_position = 0;
	ogg_int64_t packetno = 0;
	ogg_int64_t granulepos = 0;

public:
	TechnicallyFlacEncoder(AudioFormat &_audio_format, technicallyflac *_enc, tf_membuffer *_mem);
	~TechnicallyFlacEncoder() noexcept override;

	void End() override;
	void Write(const void *data, size_t length) override;
	void PreTag() override;
	void SendTag(const Tag &tag) override;
private:
	void DoEncode(bool eos);
	void GenerateHeaders(const Tag *tag);
	void GenerateHead();
	void GenerateTags(const Tag *tag);
};

class PreparedTechnicallyFlacEncoder final : public PreparedEncoder {
	unsigned int frame_size_ms;

public:
	explicit PreparedTechnicallyFlacEncoder(const ConfigBlock &block);
	Encoder *Open(AudioFormat &audio_format) override;

	[[nodiscard]] const char *GetMimeType() const noexcept override {
		return "audio/ogg";
	}
};

TechnicallyFlacEncoder::TechnicallyFlacEncoder(AudioFormat &_audio_format, technicallyflac *_enc, tf_membuffer *_mem)
	:OggEncoder(true),
	audio_format(_audio_format),
	enc(_enc),
	mem(_mem),
	buffer((int32_t *)xalloc(sizeof(int32_t) * enc->blocksize * enc->channels))
{
	switch(audio_format.format) {
	case SampleFormat::S8:
		conv = pcm8_to_pcm32; break;
	case SampleFormat::S16:
		conv = pcm16_to_pcm32; break;
	default:
		conv = pcm32_to_pcm32;
	}
	GenerateHeaders(nullptr);
}

TechnicallyFlacEncoder::~TechnicallyFlacEncoder() noexcept
{
	delete mem->buf;
	delete mem;
	delete enc;
	delete buffer;
}

void
TechnicallyFlacEncoder::Write(const void *_data, size_t length)
{
	const uint8_t *data = (const uint8_t *)_data;

	while(length > 0) {
		const unsigned num_frames = length / audio_format.GetFrameSize();

		size_t nframes = enc->blocksize - frames_position;
		if(nframes > num_frames)
			nframes = num_frames;


		const unsigned nsamples = nframes * audio_format.channels;

		size_t rbytes = conv(&buffer[samples_position],data,nsamples);

		length -= rbytes;
		frames_position  += nframes;
		samples_position += nsamples;
		data += rbytes;

		if(frames_position == enc->blocksize) {
			DoEncode(false);
		}
	}
}

void
TechnicallyFlacEncoder::GenerateHeaders(const Tag *tag)
{
	GenerateHead();
	GenerateTags(tag);
}

void
TechnicallyFlacEncoder::GenerateHead()
{
	uint8_t header[51];
	tf_membuffer tmp;

	tmp.pos = 13;
	tmp.len = 51;
	tmp.buf = header;

	tmp.buf[0] = 0x7F;
	tmp.buf[1] = 'F';
	tmp.buf[2] = 'L';
	tmp.buf[3] = 'A';
	tmp.buf[4] = 'C';

	/* 1-byte major version number (1.0) */
	tmp.buf[5] = 0x01;
	/* 1-byte minor version number (1.0) */
	tmp.buf[6] = 0x00;

	/* 2-byte, big endian number of header packets (not including first) */
	tmp.buf[7] = 0x00;
	tmp.buf[8] = 0x01;

	/* 4-byte ascii "fLaC" */
	tmp.buf[9]  = 'f';
	tmp.buf[10] = 'L';
	tmp.buf[11] = 'a';
	tmp.buf[12] = 'C';

	enc->userdata = &tmp;
	technicallyflac_streaminfo(enc,0);
	enc->userdata = mem;

	ogg_packet packet;
	packet.packet = tmp.buf;
	packet.bytes = tmp.len;
	packet.b_o_s = true;
	packet.e_o_s = false;
	packet.granulepos = 0;
	packet.packetno = packetno++;
	stream.PacketIn(packet);
}

void
TechnicallyFlacEncoder::GenerateTags(const Tag *tag)
{
	const char *version = "technicallyflac 0.0.0";
	size_t version_length = strlen(version);

	// 4 byte version length + len(version) + 4 byte tag count
	uint32_t comments_size = 4 + version_length + 4;
	uint32_t tag_count = 0;
	if (tag) {
		for (const auto &item: *tag) {
			++tag_count;
			// 4 byte length + len(tagname) + len('=') + len(value)
			comments_size += 4 + strlen(tag_item_names[item.type]) + 1 + strlen(item.value);
		}
	}

	unsigned char *comments = (unsigned char *)xalloc(comments_size);
	unsigned char *metadata_block = (unsigned char *)xalloc(4 + comments_size);
	unsigned char *p = comments;

	*(uint32_t *)(comments) = ToLE32(version_length);
	p += 4;

	memcpy(p, version, version_length);
	p += version_length;

	tag_count = ToLE32(tag_count);
	memcpy(p, &tag_count, 4);
	p += 4;

	if (tag) {
		for (const auto &item: *tag) {
			size_t tag_name_len = strlen(tag_item_names[item.type]);
			size_t tag_val_len = strlen(item.value);
			uint32_t tag_len_le = ToLE32(tag_name_len + 1 + tag_val_len);

			memcpy(p, &tag_len_le, 4);
			p += 4;

			ToUpperASCII((char *)p, tag_item_names[item.type], tag_name_len + 1);
			p += tag_name_len;

			*p++ = '=';

			memcpy(p, item.value, tag_val_len);
			p += tag_val_len;
		}
	}
	assert(comments + comments_size == p);

	tf_membuffer tmp;
	tmp.pos = 0;
	tmp.len = 4 + comments_size;
	tmp.buf = metadata_block;

	enc->userdata = &tmp;
	technicallyflac_metadata_block(enc,1,4,comments,comments_size);
	enc->userdata = mem;

	ogg_packet packet;
	packet.packet = metadata_block;
	packet.bytes = 4 + comments_size;
	packet.b_o_s = false;
	packet.e_o_s = false;
	packet.granulepos = 0;
	packet.packetno = packetno++;
	stream.PacketIn(packet);
	Flush();

	free(comments);
	free(metadata_block);
}


void
TechnicallyFlacEncoder::DoEncode(bool eos)
{
	if(frames_position > 0) {
		int b = technicallyflac_encode_interleaved(enc,buffer,frames_position);
		if( b < 0 ) {
			/* throw runtime error */
		}
		granulepos += frames_position;

		ogg_packet packet;
		packet.packet = mem->buf;
		packet.bytes  = mem->pos;
		packet.b_o_s = false;
		packet.e_o_s = eos;
		packet.granulepos = granulepos;
		packet.packetno = packetno++;
		stream.PacketIn(packet);
		frames_position = 0;
		samples_position = 0;
	}

	mem->pos = 0;
}

void
TechnicallyFlacEncoder::End()
{
	DoEncode(true);
	Flush();
}

void
TechnicallyFlacEncoder::PreTag()
{
	End();
	packetno = 0;
	granulepos = 0; // not really required, but useful to prevent wraparound
}

void
TechnicallyFlacEncoder::SendTag(const Tag &tag)
{
	stream.Reinitialize(GenerateOggSerial());
	GenerateHeaders(&tag);
}

PreparedTechnicallyFlacEncoder::PreparedTechnicallyFlacEncoder(const ConfigBlock &block)
	:frame_size_ms(block.GetBlockValue("frame_size",20U))
{
}

Encoder *
PreparedTechnicallyFlacEncoder::Open(AudioFormat &audio_format) {

	if(audio_format.sample_rate % ( 1000 / frame_size_ms) != 0) {
		/* throw an error */
	}

	technicallyflac *enc = new technicallyflac;
	tf_membuffer *mem = new tf_membuffer;
	enc->samplerate = audio_format.sample_rate;
	enc->channels = audio_format.channels;
	enc->blocksize = audio_format.sample_rate / ( 1000 / frame_size_ms);

	switch(audio_format.format) {
	case SampleFormat::S8:
		enc->bitdepth = 8; break;
	case SampleFormat::S16:
		enc->bitdepth = 16; break;
	case SampleFormat::S24_P32:
		enc->bitdepth = 24; break;
	default:
		audio_format.format = SampleFormat::S32;
		enc->bitdepth = 32;
	}

	mem->pos = 0;
	mem->len = technicallyflac_frame_size(enc->channels, enc->bitdepth, enc->blocksize);
	mem->buf = new uint8_t[mem->len];

	enc->userdata = mem;
	enc->write = tf_membuffer_write;

	technicallyflac_init(enc);

	return new TechnicallyFlacEncoder(audio_format, enc, mem);
}


static PreparedEncoder *
technicallyflac_encoder_init(const ConfigBlock &block)
{
	return new PreparedTechnicallyFlacEncoder(block);
}

const EncoderPlugin technicallyflac_encoder_plugin = {
	"technicallyflac",
	technicallyflac_encoder_init,
};

#define TECHNICALLYFLAC_IMPLEMENTATION
#include "technicallyflac.h"
