#include "speech/controlhandler.h"

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <set>
#include <nlohmann/json.hpp>
#include "log.h"

using json = nlohmann::json;

namespace
{

int SendJSON(TSession* ses, short code, const json& body)
{
	const std::string text = body.dump() + "\n";
	ResponseContentType(ses, (char*)"application/json; charset=utf-8");
	return XmlRpcServer::SendResponse(ses, code, text.c_str(), text.length());
}

int SendProblem(TSession* ses, short code, const std::string& reason)
{
	return SendJSON(ses, code, json{{"error", reason}});
}

bool IsAccount(const std::string& text)
{
	if (text.empty() || text.size() > 64)
		return false;
	for (char c : text)
		if (!isalnum((unsigned char)c) && c != '.' && c != '_' && c != '-')
			return false;
	return true;
}

}

ControlHandler::ControlHandler(SpeechConfigHolder& config, TokenStore& tokens, UsageCounters& usage, SpeechStatus& status) :
	config(config),
	tokens(tokens),
	usage(usage),
	status(status)
{
}

int ControlHandler::ProcessRequest(TRequestInfo* req, TSession* const ses)
{
	struct abyss_unix_chaninfo* channel = nullptr;
	SessionGetChannelInfo(ses, (void**)&channel);
	const IPAddress peer = channel ? IPAddress::FromSockaddr(&channel->peerAddr) : IPAddress();

	if (!config.Get()->controlAllow.Allows(peer))
	{
		Log("-ControlHandler: 403 [peer:%s,uri:%s]\n", peer.ToString().c_str(), req->uri);
		return SendProblem(ses, 403, "address not allowed");
	}

	const std::string uri = req->uri;

	if (uri == "/control/token")
		return req->method == m_post ? CreateToken(ses, peer) : SendProblem(ses, 405, "POST expected");
	if (uri == "/control/voices")
		return req->method == m_get ? ListVoices(ses) : SendProblem(ses, 405, "GET expected");
	if (uri == "/status/general")
		return req->method == m_get ? Status(req, ses) : SendProblem(ses, 405, "GET expected");

	return SendProblem(ses, 404, "not found");
}

int ControlHandler::CreateToken(TSession* ses, const IPAddress& peer)
{
	const char* type = RequestHeaderValue(ses, (char*)"content-type");
	if (!type || strncasecmp(type, "application/json", 16) != 0)
		return SendProblem(ses, 415, "application/json expected");

	const char* length = RequestHeaderValue(ses, (char*)"content-length");
	const long size = length ? strtol(length, nullptr, 10) : 0;
	if (size <= 0)
		return SendProblem(ses, 400, "empty body");
	if (size > MaxBodySize)
		return SendProblem(ses, 413, "body too large");

	std::string body(size, '\0');
	if (!XmlRpcServer::GetBody(ses, &body[0], size))
		return SendProblem(ses, 400, "truncated body");

	const json request = json::parse(body, nullptr, false);
	if (request.is_discarded() || !request.is_object())
		return SendProblem(ses, 400, "invalid JSON");

	static const std::set<std::string> known = {"function", "ttl", "account"};
	for (auto it = request.begin(); it != request.end(); ++it)
		if (!known.count(it.key()))
			return SendProblem(ses, 400, "unknown field " + it.key());

	SpeechFunction function;
	if (!request.contains("function") || !request["function"].is_string()
	    || !ParseSpeechFunction(request["function"].get<std::string>(), function))
		return SendProblem(ses, 400, "function must be asr, tts or comp");

	int ttl = 3600;
	if (request.contains("ttl"))
	{
		const json& value = request["ttl"];
		if (!value.is_number_integer() || value.get<long long>() < 1 || value.get<long long>() > 3600)
			return SendProblem(ses, 400, "ttl must be from 1 to 3600");
		ttl = value.get<int>();
	}

	std::string account = "-";
	if (request.contains("account"))
	{
		if (!request["account"].is_string() || !IsAccount(request["account"].get<std::string>()))
			return SendProblem(ses, 400, "account must be 1 to 64 characters of [A-Za-z0-9._-]");
		account = request["account"].get<std::string>();
	}

	const TokenStore::Token token = tokens.Create(function, ttl, account, time(NULL));
	if (token.value.empty())
	{
		Error("-ControlHandler: random generator failure, no token issued\n");
		return SendProblem(ses, 500, "internal error");
	}
	usage.Register(account);

	Log("-ControlHandler: token issued [function:%s,account:%s,token:%.6s,ttl:%d,peer:%s]\n",
	    SpeechFunctionName(function), account.c_str(), token.value.c_str(), ttl, peer.ToString().c_str());

	return SendJSON(ses, 201, json{
		{"token", token.value},
		{"function", SpeechFunctionName(function)},
		{"account", account},
		{"expiresAt", SpeechIsoUtc(token.expiresAt)}});
}

int ControlHandler::ListVoices(TSession* ses)
{
	json voices = json::array();
	for (const SpeechConfig::Voice& voice : config.Get()->voices)
		voices.push_back(json{{"alias", voice.alias}, {"lang", voice.lang}, {"voiceId", voice.voiceId}});
	return SendJSON(ses, 200, json{{"voices", voices}});
}

int ControlHandler::Status(TRequestInfo* req, TSession* ses)
{
	const bool wantText = req->query && strstr(req->query, "format=text");

	SpeechStatus::Info info;
	status.Collect(info, time(NULL));
	const std::string body = wantText ? SpeechStatus::RenderText(info) : SpeechStatus::RenderJSON(info);

	ResponseContentType(ses, wantText
		? (char*)"text/plain; charset=utf-8"
		: (char*)"application/json; charset=utf-8");
	return XmlRpcServer::SendResponse(ses, 200, body.c_str(), body.length());
}
