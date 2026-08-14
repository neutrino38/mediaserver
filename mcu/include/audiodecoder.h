/* 
 * File:   audiodecoder.h
 * Author: Sergio
 *
 * Created on 1 de agosto de 2012, 13:34
 */

#ifndef AUDIODECODER_H
#define	AUDIODECODER_H
#include "medkit/codecs.h"
#include "audio.h"
#include "worker.h"
#include "waitqueue.h"
#include "rtp.h"

class AudioDecoderWorker : public Worker
{
public:
	AudioDecoderWorker();
	virtual ~AudioDecoderWorker();

	int Init(AudioOutput *output);
	int Start();
	void onRTPPacket(RTPPacket &packet);
	int Stop();
	int End();

protected:
	int Decode();
	//Corps du Worker
	virtual int Run() { return Decode(); }

private:
	AudioOutput *output;
	WaitQueue<RTPPacket*> packets;
	bool decoding;
};

#endif	/* AUDIODECODER_H */

