/* 
 * File:   RTPSmoother.h
 * Author: Sergio
 *
 * Created on 7 de noviembre de 2011, 12:18
 */

#ifndef RTPSMOOTHER_H
#define	RTPSMOOTHER_H

#include "config.h"
#include "worker.h"
#include "waitqueue.h"
#include "rtp.h"
#include "rtpsession.h"

class RTPSmoother : public Worker
{
public:
	RTPSmoother();
	~RTPSmoother();
	int Init(RTPSession *session);
	int SendFrame(MediaFrame* frame,DWORD duration);
	int Cancel();
	int End();

protected:
	//Corps du Worker
	virtual int Run();

private:
	RTPSession	*session;
	bool		inited;
	WaitQueue<RTPPacketSched*> queue;
};

#endif	/* RTPSMOOTHER_H */

