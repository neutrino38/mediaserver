/* 
 * File:   AudioEncoderWorker.h
 * Author: Sergio
 *
 * Created on 4 de octubre de 2011, 20:42
 */

#ifndef AUDIOENCODERWORKER_H
#define	AUDIOENCODERWORKER_H
#include "medkit/codecs.h"
#include "audio.h"
#include "RTPMultiplexer.h"


class AudioEncoderMultiplexerWorker :
	public RTPMultiplexer
{
public:
	AudioEncoderMultiplexerWorker();
	virtual ~AudioEncoderMultiplexerWorker();

	int Init(AudioInput *input);
	int SetCodec(AudioCodec::Type codec);
	int End();
	//Joinable interface
	virtual void AddListener(Listener *listener);
	virtual void Update();
	virtual void RemoveListener(Listener *listener);
	//Phase 5 (nego_fmtp §6.3) : bornes négociées par code codec, passées à
	//AudioCodecFactory::CreateEncoder à l'ouverture (Opus : useinbandfec,
	//usedtx, maxaveragebitrate, cbr déclarés par le pair).
	virtual void SetNegotiatedCodecProperties(const std::map<int,Properties>& byCodec);

private:
	int Start();
	int Stop();
protected:
	int Encode();

private:
	static void *startEncoding(void *par);

private:
	AudioInput *input;
	AudioCodec::Type codec;
	pthread_t thread;
	bool encoding;
	//Bornes négociées par code codec (phase 5), fusionnées à l'ouverture.
	std::map<int,Properties> negotiated;
};

#endif	/* AUDIOENCODERWORKER_H */

