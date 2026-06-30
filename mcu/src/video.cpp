#include "log.h"
#include "video.h"
// FfVideoEncoder/FfVideoDecoder (libmedkit). Inclut <libavcodec/avcodec.h> et,
// via les gardes d'include partagées (_VIDEO_H_/_CODECS_H_/_MEDIA_H_ déjà
// définies par "video.h" ci-dessus), se lie aux types mcu — ABI-identiques.
#include "ffvideocodec.h"
// Décodeurs medkit portant leur propre dépaquetisation RTP (chevrons : on vise
// le sous-module $(MEDKITDIR)/..., pas les homonymes mcu/src).
#include <h264/h264decoder.h>
#include <h263/h263codec.h>
#include <vp8/vp8decoder.h>
#include <vp8/vp8encoder.h>
// Encodeur H264 reste côté mcu (libx264).
#include "h264/h264encoder.h"

VideoDecoder* VideoCodecFactory::CreateDecoder(VideoCodec::Type codec)
{
	Log("-CreateVideoDecoder[%d,%s]\n",codec,VideoCodec::GetNameFor(codec));

	//Depending on the codec
	switch(codec)
	{
		case VideoCodec::SORENSON:
			return new FfVideoDecoder(AV_CODEC_ID_FLV1, VideoCodec::SORENSON);
		case VideoCodec::H263_1998:
			return new H263Decoder();
		case VideoCodec::H263_1996:
			// H263-1996 : pas de dépaquetiseur dédié -> défaut FfVideoDecoder.
			return new FfVideoDecoder(AV_CODEC_ID_H263, VideoCodec::H263_1996);
		case VideoCodec::MPEG4:
			return new FfVideoDecoder(AV_CODEC_ID_MPEG4, VideoCodec::MPEG4);
		case VideoCodec::H264:
			return new H264Decoder();
		case VideoCodec::VP6:
			return new FfVideoDecoder(AV_CODEC_ID_VP6F, VideoCodec::VP6);
		case VideoCodec::VP8:
			// Décodeur VP8 natif ffmpeg (pas libvpx) avec sa dépaquetisation propre.
			return new VP8Decoder();
		default:
			Error("Video decoder not found [%d]\n",codec);
	}
	return NULL;
}

VideoEncoder* VideoCodecFactory::CreateEncoder(VideoCodec::Type codec)
{
	//Empty properties
	Properties properties;

	//Create codec
	return CreateEncoder(codec,properties);
}


VideoEncoder* VideoCodecFactory::CreateEncoder(VideoCodec::Type codec,const Properties& properties)
{
	Log("-CreateVideoEncoder[%d,%s]\n",codec,VideoCodec::GetNameFor(codec));

	//Depending on the codec
	switch(codec)
	{
		case VideoCodec::SORENSON:
			return new FfVideoEncoder(properties, AV_CODEC_ID_FLV1, VideoCodec::SORENSON);
		case VideoCodec::H263_1998:
			return new FfVideoEncoder(properties, AV_CODEC_ID_H263P, VideoCodec::H263_1998);
		case VideoCodec::H263_1996:
			return new FfVideoEncoder(properties, AV_CODEC_ID_H263, VideoCodec::H263_1996);
		case VideoCodec::MPEG4:
			return new FfVideoEncoder(properties, AV_CODEC_ID_MPEG4, VideoCodec::MPEG4);
		case VideoCodec::H264:
			return new H264Encoder(properties);
		case VideoCodec::VP8:
			// Encodeur VP8 via le wrapper libvpx de ffmpeg (libmedkit).
			return new VP8Encoder(properties);
		default:
			Error("Video Encoder not found\n");
	}
	return NULL;
}


/******************************************************************************
 * VideoFrame : packetisation RTP. Définie ici (et non dans libmedkit/video.cpp)
 * car l'objet video.o de mcu fournit déjà VideoCodecFactory ; l'archive medkit
 * ne doit donc pas être tirée pour ces symboles (sinon double définition).
 * Porté depuis third_party/fontventa/libmedikit/video.cpp.
 ******************************************************************************/

bool VideoFrame::Packetize(unsigned int mtu)
{
	//Depending on the codec
	switch(codec)
	{
		case VideoCodec::H263_1998:
			return PacketizeH263(mtu);

		case VideoCodec::H264:
			return PacketizeH264(mtu);

		default:
			Error("Dont know how to packetize video frame for codec [%d]\n",codec);
	}
	return false;
}

DWORD VideoFrame::ReadNaluSize(BYTE * data)
{
	switch(naluSizeLen)
	{
		case 0:
			return 0;

		case 1:
			return data[0];

		case 2:
			return (data[0] << 8) | data[1];

		case 3:
			return (data[0] << 16) |(data[1] << 8) | data[2];

		default:
			return (data[0] << 24) |(data[1] << 16) |(data[2] << 8) | data[3];
	}
}

DWORD VideoFrame::DetectNaluBoundary(BYTE * p, DWORD sz)
{
	DWORD l;

	for (l = 0; l+4 < sz; l++)
	{
		if (p[l] == 0 && p[l+1] == 0)
		{
			if (p[l+2] == 1)
			{
				return l;
			}
		}
		else if(p[l+2] == 0 && p[l+3] == 1)
		{
			return l;;
		}
	}

	if (l+3 < sz)
	{
		if (p[l] == 0 && p[l+1] == 0)
		{
			if (p[l+2] == 1)
			{
				return l;
			}
		}
	}

	return 0;
}

#define H264_FUA_HEADER_SIZE				2

bool VideoFrame::PacketizeH264(unsigned int mtu)
{
	BYTE * p = GetData();
	unsigned int l = 0;
	DWORD naluSz;

	ClearRTPPacketizationInfo();

	// Skip header (if needed)
	if (p[l] == 0 && p[l+1] == 0)
	{
		if (p[l+2] == 1)
		{
			l+= 3;
		}
	}
	else if(p[l+2] == 0 && p[l+3] == 1)
	{
		l+= 4;
	}

	while (l < GetLength() )
	{
		if (useStartCode)
			naluSz = DetectNaluBoundary(p + l, GetLength() - l );
		else
			naluSz = ReadNaluSize(p + l);

		if (naluSz == 0 || naluSz > GetLength() ) return false;
		bool last = (l + naluSz >= GetLength());
		PacketizeH264Nalu(mtu, l, naluSz, last);
		l += naluSz;
	}
	return true;
}

void VideoFrame::PacketizeH264Nalu(unsigned int mtu, DWORD offset, DWORD naluSz, bool last)
{
	BYTE * p = GetData();
	p += offset;
	unsigned int l = 0;

	// Single NAL packet
	if ( naluSz <= mtu )
	{
		AddRtpPacket(l, naluSz, 0L, 0, last );
		return;
	}

	uint8_t fua_hdr[H264_FUA_HEADER_SIZE];
	fua_hdr[0] = p[l] & 0x60; /* NRI */
	fua_hdr[0] |= 28; //fu_a
	fua_hdr[1] = 0x80; /* S=1,E=0,R=0 */
	fua_hdr[1] |= p[l] & 0x1f; /* type */

	while (l < naluSz )
	{
		unsigned long pktSize = naluSz - l;

		if (pktSize > mtu)
		{
			pktSize = mtu;
		}
		else
		{
			// Last fragment -> set E bit
			fua_hdr[1] |= 0x40;
		}

		AddRtpPacket(offset + l, pktSize, fua_hdr, H264_FUA_HEADER_SIZE,
			     pktSize + l >= naluSz);

		// reset "S" bit (that marks the first fragment)
		fua_hdr[1] &= 0x7F;
		l += pktSize;
	}
}

bool VideoFrame::PacketizeH263(unsigned int mtu)
{
	// Packetisation RFC 2429 (H.263+) simple : fragmentation du flux complet en
	// paquets <= mtu, chacun précédé d'un en-tête de payload 2 octets. Le premier
	// paquet de la trame porte le bit P (début d'image), les suivants P=0.
	// (symétrique de FfVideoDecoder::DecodePacket / de la packetisation faite par
	//  l'encodeur ffmpeg)
	const unsigned int H263P_HEADER = 2;
	unsigned int payload = (mtu > H263P_HEADER) ? (mtu - H263P_HEADER) : 1;

	ClearRTPPacketizationInfo();

	BYTE prefix[H263P_HEADER];
	bool first = true;

	for (unsigned int i = 0; i < GetLength(); i += payload)
	{
		unsigned int len = GetLength() - i;
		bool last = (len <= payload);
		if (!last) len = payload;

		// En-tête RFC 2429 : RR=0 ; P=1 sur le premier fragment ; V=0 ; PLEN=0.
		prefix[0] = first ? 0x04 : 0x00;
		prefix[1] = 0x00;

		AddRtpPacket(i, len, prefix, H263P_HEADER, last);
		first = false;
	}
	return true;
}
