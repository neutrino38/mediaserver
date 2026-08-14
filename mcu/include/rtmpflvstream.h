#ifndef _RTMPFLVSTREAM_H_
#define _RTMPFLVSTREAM_H_
#include "worker.h"
#include "config.h"
#include "rtmp.h"
#include "rtmpstream.h"
#include "rtmpmessage.h"
#include <string>

class RTMPFLVStream : public RTMPMediaStream, public Worker
{
public:
	RTMPFLVStream(DWORD id);
	virtual ~RTMPFLVStream();
	virtual bool Play(std::wstring& url);
	virtual bool Seek(DWORD time);
	virtual bool Publish(std::wstring& url);
	virtual void PublishMediaFrame(RTMPMediaFrame *frame);
	virtual void PublishMetaData(RTMPMetaData *meta);
	virtual bool Close();

protected:
	int PlayFLV();

private:
	//Corps du Worker
	virtual int Run() { return PlayFLV(); }

protected:
	int	fd;
	bool	recording;
	bool	playing;
	QWORD 	first;
};

#endif
