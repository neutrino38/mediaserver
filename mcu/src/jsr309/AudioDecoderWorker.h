/* 
 * File:   AudioDecoderWorker.h
 * Author: Sergio
 *
 * Created on 4 de octubre de 2011, 20:06
 */

#ifndef AUDIODECODERWORKER_H
#define	AUDIODECODERWORKER_H
#include "medkit/codecs.h"
#include "audio.h"
#include "pipeaudioinput.h"
#include "worker.h"
#include "waitqueue.h"
#include "Joinable.h"

class AudioDecoderJoinableWorker:
	public Joinable::Listener,
	public Worker
{
public:
	AudioDecoderJoinableWorker();
	virtual ~AudioDecoderJoinableWorker();

	int Init(AudioOutput *output);
        int Init(PipeAudioInput * input);
	int End();

	//Virtuals from Joinable::Listener
	virtual void onRTPPacket(RTPPacket &packet);
	virtual void onResetStream();
	virtual void onEndStream();

	//Attach
	int Attach(const std::shared_ptr<Joinable> & join);
	int Dettach();
	int Start();
	int Stop();

protected:
	int Decode();
	//Corps du Worker
	virtual int Run() { return Decode(); }

private:
	AudioOutput *output;
        PipeAudioInput  *input;
	WaitQueue<RTPPacket*> packets;
	// Lien retour NON possédant vers la source (weak_ptr → lock() au site d'usage) :
	// une source détruite fait échouer le lock() (C-13, lien A).
	std::weak_ptr<Joinable> joined;

};

#endif	/* AUDIODECODERWORKER_H */

