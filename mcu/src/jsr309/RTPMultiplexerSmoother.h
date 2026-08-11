/* 
 * File:   RTPSmoother.h
 * Author: Sergio
 *
 * Created on 7 de noviembre de 2011, 12:18
 */

#ifndef RTPMULTIPLEXERSMOOTHER_H
#define	RTPMULTIPLEXERSMOOTHER_H

#include "config.h"
#include "worker.h"
#include "waitqueue.h"
#include "rtp.h"
#include "RTPMultiplexer.h"


class RTPMultiplexerSmoother :
	public RTPMultiplexer,
	public Worker
{
public:
	RTPMultiplexerSmoother();
	virtual ~RTPMultiplexerSmoother();
	int Start();
	int SmoothFrame(MediaFrame* frame,DWORD duration);
	int Cancel();
	int Wait();
	int Stop();

protected:
	//Corps du Worker
	virtual int Run();

private:
	bool		inited;
	WaitQueue<RTPPacketSched*> queue;
};

#endif	/* RTPMULTIPLEXERSMOOTHER_H */

