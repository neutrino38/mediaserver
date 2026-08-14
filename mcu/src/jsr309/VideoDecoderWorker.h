/* 
 * File:   VideoDecoderWorker.h
 * Author: Sergio
 *
 * Created on 2 de noviembre de 2011, 23:38
 */

#ifndef VIDEODECODERWORKER_H
#define	VIDEODECODERWORKER_H

#include "medkit/codecs.h"
#include "video.h"
#include "worker.h"
#include "waitqueue.h"
#include "Joinable.h"

class VideoDecoderJoinableWorker:
	public Joinable::Listener,
	public Worker
{
public:
	VideoDecoderJoinableWorker(bool useThread = true);
	virtual ~VideoDecoderJoinableWorker();

	int Init(VideoOutput *output);
	int End();

	//Virtuals from Joinable::Listener
	virtual void onRTPPacket(RTPPacket &packet);
	virtual void onResetStream();
	virtual void onEndStream();

	//Attach
	int Attach(const std::shared_ptr<Joinable> & join);
	int Dettach();

	//Mode pont : pose la source SANS s'y inscrire comme listener (c'est le
	//transcodeur qui recoit les paquets et ne nous les passe qu'en mode
	//transcodage). Sans elle, joined reste vide et les demandes de FPU amont
	//(perte de paquets, erreur de decodage) echouent en silence : la source ne
	//recoit plus jamais de FIR et le decodeur attend une intra sans fin.
	void SetSource(const std::shared_ptr<Joinable> & join) { joined = join; }

	//Publics comme leurs équivalents d'AudioDecoderJoinableWorker, et pour le
	//même appelant : en mode pont, VideoTranscoder démarre ce worker SANS
	//l'attacher à la source (c'est lui qui reçoit les paquets et n'en passe au
	//décodeur que quand l'arbitrage retombe sur le transcodage).
	int Start();
	int Stop();
protected:
	int Decode();
	//Corps du Worker
	virtual int Run() { return Decode(); }

private:
        void DecodePacket(RTPPacket* pkt);

private:
	VideoOutput *output;
        VideoInput  *input;
	WaitQueue<RTPPacket*> packets;
	bool decoding;

        /* decoding variables */
	VideoDecoder*	videoDecoder ;
	VideoCodec::Type type;
        timeval	lastFPURequest;
	DWORD	lostCount;
	DWORD	frameSeqNum;
	DWORD	lastSeq;
	bool	waitIntra;

	// Lien retour NON possédant vers la source (weak_ptr → lock() au site d'usage) :
	// une source détruite fait échouer le lock() (C-13, lien A).
	std::weak_ptr<Joinable> joined;
        bool useThread;
};

#endif	/* VIDEODECODERWORKER_H */

