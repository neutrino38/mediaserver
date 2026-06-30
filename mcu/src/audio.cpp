#include "log.h"
#include "audio.h"
#include "g711/g711codec.h"
#include "gsm/gsmcodec.h"
#include "speex/speexcodec.h"
#ifdef OPUS_SUPPORT
#include "opus/opusencoder.h"
#include "opus/opusdecoder.h"
#endif
// G.722 fourni par libmedkit (FfAudioEncoder/Decoder, ffmpeg 5). Chevrons pour
// viser l'en-tete du sous-module ($(MEDKITDIR)/g722/g722codec.h) et non le
// fichier homonyme mcu/src/g722/g722codec.h (resolu en premier par les guillemets).
#include <g722/g722codec.h>
// AMR-NB / AMR-WB fournis par libmedkit (FfAudioEncoder/Decoder, ffmpeg). Chevrons
// pour viser le sous-module ($(MEDKITDIR)/amr/amrcodec.h).
#include <amr/amrcodec.h>
// NellyMoser 8 kHz / 11,025 kHz fourni par libmedkit (ffmpeg AV_CODEC_ID_NELLYMOSER).
#include <nelly/nellycodec.h>
#include "g722/g7221codec.h"

AudioEncoder* AudioCodecFactory::CreateEncoder(AudioCodec::Type codec)
{
	//Empty properties
	Properties properties;

	//Create codec
	return CreateEncoder(codec,properties);
}

AudioEncoder* AudioCodecFactory::CreateEncoder(AudioCodec::Type codec, const Properties &properties)
{
	Log("-CreateAudioEncoder [%d,%s]\n",codec,AudioCodec::GetNameFor(codec));

	//Creamos uno dependiendo del tipo
	switch(codec)
	{
		case AudioCodec::GSM:
			return new GSMEncoder(properties);
		case AudioCodec::PCMA:
			return new PCMAEncoder(properties);
		case AudioCodec::PCMU:
			return new PCMUEncoder(properties);
		case AudioCodec::SPEEX16:
			return new SpeexEncoder(properties);
#ifdef OPUS_SUPPORT
		case AudioCodec::OPUS:
			return new OpusEncoder(properties);
#endif
		case AudioCodec::G722:
			return new G722Encoder(properties);

                case AudioCodec::G7221:
			return new G7221Encoder(properties);
		case AudioCodec::AMR:
			return new AMRNBEncoder(properties);
		case AudioCodec::AMRWB:
			return new AMRWBEncoder(properties);
		case AudioCodec::NELLY8:
			return new NellyEncoder(properties);
		case AudioCodec::NELLY11:
			return new NellyEncoder11Khz(properties);
		default:
			Error("Codec not found [%d]\n",codec);
	}

	return NULL;
}

AudioDecoder* AudioCodecFactory::CreateDecoder(AudioCodec::Type codec)
{
	Log("-CreateAudioDecoder [%d,%s]\n",codec,AudioCodec::GetNameFor(codec));

	//Creamos uno dependiendo del tipo
	switch(codec)
	{
		case AudioCodec::GSM:
			return new GSMDecoder();
		case AudioCodec::PCMA:
			return new PCMADecoder();
		case AudioCodec::PCMU:
			return new PCMUDecoder();
		case AudioCodec::SPEEX16:
			return new SpeexDecoder();
#ifdef OPUS_SUPPORT
		case AudioCodec::OPUS:
			return new OpusDecoder();
#endif
		case AudioCodec::G722:
			return new G722Decoder();
		case AudioCodec::AMR:
			return new AMRNBDecoder();
		case AudioCodec::AMRWB:
			return new AMRWBDecoder();
		case AudioCodec::NELLY8:
			return new NellyDecoder();
		case AudioCodec::NELLY11:
			return new NellyDecoder11Khz();
		default:
			Error("Codec not found [%d]\n",codec);
	}

	return NULL;
}

// Definie ici (et non tiree de libmedkit/audio.o) pour eviter de pulser l'objet
// medkit qui redefinirait AudioCodecFactory. Porte depuis libmedikit/audio.cpp.
bool AudioFrame::Packetize(unsigned int mtu)
{
	unsigned int paksize = packetization;
	if (paksize > mtu && mtu > 0) paksize = mtu;

	ClearRTPPacketizationInfo();
	for (unsigned int i=0; i<GetLength(); i+= paksize )
	{
		unsigned int rtplen = GetLength() - i;

		if (rtplen > paksize ) rtplen = paksize;
		AddRtpPacket(i, rtplen, 0, NULL, false);
	}
	return true;
}
