/* 
 * File:   RTPSmoother.h
 * Author: Sergio
 *
 * Created on 7 de noviembre de 2011, 12:18
 */

#ifndef RTPSMOOTHER_H
#define	RTPSMOOTHER_H

#include "config.h"
#include "waitqueue.h"
#include "rtp.h"
#include "rtpsession.h"

class RTPSmoother
{
public:
	RTPSmoother();
	~RTPSmoother();
	int Init(RTPSession *session);
	int SendFrame(MediaFrame* frame,DWORD duration);
	int Cancel();
	int End();

protected:
	int Run();

private:
	//Funciones propias
	static void *run(void *par);
private:
	RTPSession	*session;
	pthread_t	thread;
	//Sommeil cadencé annulable entre deux paquets d'une même trame
	Wait		pacer;
	bool		inited;
	WaitQueue<RTPPacketSched*> queue;
};

#endif	/* RTPSMOOTHER_H */

