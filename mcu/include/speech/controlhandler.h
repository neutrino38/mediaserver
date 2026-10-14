#ifndef SPEECH_CONTROLHANDLER_H
#define SPEECH_CONTROLHANDLER_H

#include <string>
#include "xmlrpcserver.h"
#include "speech/speechconfig.h"
#include "speech/speechstatus.h"
#include "speech/tokenstore.h"
#include "speech/usage.h"

/**
 * The control port (SPEC §6), mounted on "/": the IP filter applies to every
 * path before anything else, the request body included.
 *
 *   POST /control/token    creates a streaming token
 *   GET  /control/voices   the voices served by tts
 *   GET  /status/general   JSON, or text with ?format=text
 *
 * The server must be an AF_INET XmlRpcServer: Abyss truncates IPv6 peers.
 */
class ControlHandler : public Handler
{
public:
	static constexpr int MaxBodySize = 4096;

	ControlHandler(SpeechConfigHolder& config, TokenStore& tokens, UsageCounters& usage, SpeechStatus& status);

	virtual int ProcessRequest(TRequestInfo* req, TSession* const ses) override;

private:
	int CreateToken(TSession* ses, const IPAddress& peer);
	int ListVoices(TSession* ses);
	int Status(TRequestInfo* req, TSession* ses);

	SpeechConfigHolder&	config;
	TokenStore&		tokens;
	UsageCounters&		usage;
	SpeechStatus&		status;
};

#endif
