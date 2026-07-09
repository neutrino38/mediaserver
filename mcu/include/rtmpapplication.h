#ifndef _RTMP_APPLICATION_H_
#define _RTMP_APPLICATION_H_
#include "config.h"
#include "rtmpnetconnection.h"
#include <string>
#include <memory>

class RTMPApplication
{
public:
	virtual std::shared_ptr<RTMPNetConnection> Connect(const std::wstring& appName,RTMPNetConnection::Listener *listener) = 0;
};

#endif
