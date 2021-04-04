#include "TechnicallyFlacEncoderPlugin.hxx"
#include "../OggEncoder.hxx"
#include "util/ByteOrder.hxx"
#include "util/StringUtil.hxx"

#include "technicallyflac.h"
#include <ogg/ogg.h>

#include <stdint.h>
#include <stdlib.h>

typedef unsigned (*pcm_conv)(int32_t *, const void *, unsigned, unsigned, unsigned, unsigned);

static unsigned
pcm8_to_pcm32(int32_t *out, const void *_in, unsigned int blocksize, unsigned offset, unsigned num_frames, unsigned channels)
{
	const int8_t *in = (const int8_t *)_in;
    unsigned int i;
    unsigned int c;
    for(i=0; i < num_frames; i++) {
        for(c = 0; c < channels; c++) {
            out[(c * blocksize) + i + offset] = in[(i*channels)+c];
        }
    }
    return num_frames * channels * sizeof(int8_t);
}

static unsigned
pcm16_to_pcm32(int32_t *out, const void *_in, unsigned int blocksize, unsigned offset, unsigned num_frames, unsigned channels)
{
	const int16_t *in = (const int16_t *)_in;
    unsigned int i;
    unsigned int c;
    for(i=0; i < num_frames; i++) {
        for(c = 0; c < channels; c++) {
            out[(c * blocksize) + i + offset] = in[(i*channels)+c];
        }
    }
    return num_frames * channels * sizeof(int16_t);
}

static unsigned
pcm32_to_pcm32(int32_t *out, const void *_in, unsigned int blocksize, unsigned offset, unsigned num_frames, unsigned channels)
{
	const int32_t *in = (const int32_t *)_in;
    unsigned int i;
    unsigned int c;
    for(i=0; i < num_frames; i++) {
        for(c = 0; c < channels; c++) {
            out[(c * blocksize) + i + offset] = in[(i*channels)+c];
        }
    }
    return num_frames * channels * sizeof(int32_t);
}

class TechnicallyFlacEncoder final : public OggEncoder {
	const AudioFormat audio_format;
	technicallyflac *enc;
    uint8_t* buffer;
    uint32_t bufferlen;
	int32_t* pcm_buffer;
    int32_t* channel_buffer[8];
	pcm_conv conv;
	size_t frames_position = 0;
	ogg_int64_t packetno = 0;
	ogg_int64_t granulepos = 0;

public:
	TechnicallyFlacEncoder(AudioFormat &_audio_format, technicallyflac *_enc, uint8_t *buffer, uint32_t bufferlen);
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

TechnicallyFlacEncoder::TechnicallyFlacEncoder(AudioFormat &_audio_format, technicallyflac *_enc, uint8_t *_buffer, uint32_t _bufferlen)
	:OggEncoder(true),
	audio_format(_audio_format),
	enc(_enc),
    buffer(_buffer),
    bufferlen(_bufferlen),
	pcm_buffer(new int32_t[enc->blocksize * enc->channels])
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
    for(unsigned int i = 0; i<enc->channels; i++) {
        channel_buffer[i] = &pcm_buffer[i * enc->blocksize];
    }
}

TechnicallyFlacEncoder::~TechnicallyFlacEncoder() noexcept
{
	delete enc;
	delete pcm_buffer;
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

		size_t rbytes = conv(pcm_buffer,data,enc->blocksize,frames_position,nframes,enc->channels);

		length -= rbytes;
		frames_position  += nframes;
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
    /* 9 bytes of ogg-specific header
     * 4 bytes for fLaC streammarker
     * 4 bytes for streaminfo header
     * 34 bytes for streaminfo */
	uint8_t header[51];
    uint32_t len;

	header[0] = 0x7F;
	header[1] = 'F';
	header[2] = 'L';
	header[3] = 'A';
	header[4] = 'C';

	/* 1-byte major version number (1.0) */
	header[5] = 0x01;
	/* 1-byte minor version number (1.0) */
	header[6] = 0x00;

	/* 2-byte, big endian number of header packets (not including first) */
	header[7] = 0x00;
	header[8] = 0x01;

    len = 51 - 9;
	technicallyflac_streammarker(enc,&header[9],&len);
    len = 51 - 13;
	technicallyflac_streaminfo(enc,&header[13],&len,0);

	ogg_packet packet;
	packet.packet = header;
	packet.bytes = 51;
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

	unsigned char *comments = new unsigned char[comments_size];
    uint32_t metadata_len = 4 + comments_size;
	unsigned char *metadata_block = new unsigned char[metadata_len];
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

	technicallyflac_metadata(enc,metadata_block,&metadata_len,1,4,comments_size,comments);

	ogg_packet packet;
	packet.packet = metadata_block;
	packet.bytes = 4 + comments_size;
	packet.b_o_s = false;
	packet.e_o_s = false;
	packet.granulepos = 0;
	packet.packetno = packetno++;
	stream.PacketIn(packet);
	Flush();

	delete comments;
	delete metadata_block;
}


void
TechnicallyFlacEncoder::DoEncode(bool eos)
{
	if(frames_position > 0) {
        uint32_t tmp_bufferlen = bufferlen;
		technicallyflac_frame(enc,buffer,&tmp_bufferlen,frames_position,channel_buffer);
		granulepos += frames_position;

		ogg_packet packet;
		packet.packet = buffer;
		packet.bytes  = tmp_bufferlen;
		packet.b_o_s = false;
		packet.e_o_s = eos;
		packet.granulepos = granulepos;
		packet.packetno = packetno++;
		stream.PacketIn(packet);
		frames_position = 0;
	}
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
    uint8_t *buffer;
    uint32_t bufferlen;
    uint8_t bitdepth;
    uint32_t blocksize = audio_format.sample_rate / ( 1000 / frame_size_ms);

	switch(audio_format.format) {
	case SampleFormat::S8:
		bitdepth = 8; break;
	case SampleFormat::S16:
		bitdepth = 16; break;
	case SampleFormat::S24_P32:
		bitdepth = 24; break;
	default:
		audio_format.format = SampleFormat::S32;
		bitdepth = 32;
	}

    technicallyflac_init(enc,blocksize,audio_format.sample_rate,audio_format.channels,bitdepth);
    bufferlen = technicallyflac_size_frame(enc->blocksize,enc->channels,enc->bitdepth);
    buffer = new uint8_t[bufferlen];

	return new TechnicallyFlacEncoder(audio_format, enc, buffer, bufferlen);
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
