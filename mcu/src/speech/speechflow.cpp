#include "speech/speechflow.h"

#include <chrono>
#include <nlohmann/json.hpp>
#include "log.h"
#include "websocketserver.h"
#include "speech/speechservice.h"

using json = nlohmann::json;

namespace
{

std::string Escape(const std::string& value)
{
	static const char hex[] = "0123456789ABCDEF";
	std::string out;
	for (unsigned char c : value)
		if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
			out.push_back(c);
		else
		{
			out.push_back('%');
			out.push_back(hex[c >> 4]);
			out.push_back(hex[c & 15]);
		}
	return out;
}

}

/**
 * One WebSocket to speech-server. The flow owns it; the connection only holds
 * it weakly, so a dropped link hears nothing more.
 */
class SpeechFlow::Link : public WebSocket::Listener
{
public:
	static constexpr size_t MaxMessageSize = 1024*1024;

	explicit Link(std::weak_ptr<SpeechFlow> flow) : flow(flow) {}

	void Send(const std::string& text)
	{
		if (std::shared_ptr<WebSocket> socket = Socket())
			socket->SendMessage(text);
	}

	void Send(const BYTE* data, DWORD size)
	{
		if (std::shared_ptr<WebSocket> socket = Socket())
			socket->SendMessage(data, size);
	}

	void Close()
	{
		std::shared_ptr<WebSocket> socket;
		{
			std::lock_guard<std::mutex> lock(mutex);
			closeRequested = true;
			socket = ws.lock();
		}
		if (socket)
			socket->Close(1000, "");
	}

	//WebSocket::Listener
	virtual void onOpen(WebSocket* socket) override
	{
		bool closeNow;
		{
			std::lock_guard<std::mutex> lock(mutex);
			ws = socket->GetWeakPtr();
			closeNow = closeRequested;
		}
		if (closeNow)
			socket->Close(1000, "");
	}

	virtual void onMessageStart(WebSocket* ws, const WebSocket::MessageType type, const DWORD length) override
	{
		messageType = type;
		message.clear();
		messageTooBig = false;
	}

	virtual void onMessageData(WebSocket* ws, const BYTE* data, const DWORD size) override
	{
		if (messageTooBig || message.size() + size > MaxMessageSize)
		{
			messageTooBig = true;
			message.clear();
			return;
		}
		message.append((const char*)data, size);
	}

	virtual void onMessageEnd(WebSocket* ws) override
	{
		std::shared_ptr<SpeechFlow> target = flow.lock();
		if (!target)
			return;
		if (messageTooBig)
		{
			Error("-SpeechFlow: oversized message ignored\n");
			return;
		}
		if (messageType == WebSocket::Binary)
			return target->OnLinkBinary(this, (const BYTE*)message.data(), message.size());

		const json parsed = json::parse(message, nullptr, false);
		if (!parsed.is_object() || !parsed.contains("type") || !parsed["type"].is_string())
		{
			Error("-SpeechFlow: unreadable message ignored\n");
			return;
		}
		target->OnLinkMessage(this, parsed);
	}

	virtual void onError(WebSocket* ws) override
	{
	}

	virtual void onClose(WebSocket* ws) override
	{
		if (std::shared_ptr<SpeechFlow> target = flow.lock())
			target->OnLinkClosed(this);
	}

	// Under the flow lock.
	std::string	fatalReason;
	std::string	tokenPrefix;

private:
	std::shared_ptr<WebSocket> Socket()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return ws.lock();
	}

	const std::weak_ptr<SpeechFlow>	flow;
	std::mutex			mutex;
	std::weak_ptr<WebSocket>	ws;
	bool				closeRequested = false;

	// Reactor thread only.
	WebSocket::MessageType		messageType = WebSocket::Text;
	std::string			message;
	bool				messageTooBig = false;
};

SpeechFlow::SpeechFlow(SpeechService& service, SpeechFunction function, std::weak_ptr<Listener> listener, const Params& params) :
	service(service),
	function(function),
	listener(listener),
	params(params)
{
}

SpeechFlow::~SpeechFlow()
{
	Stop();
}

uint64_t SpeechFlow::NowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

const char* SpeechFlow::StateName(State state)
{
	switch (state)
	{
		case State::Idle:	return "idle";
		case State::Token:	return "token";
		case State::Connecting:	return "connecting";
		case State::Ready:	return "ready";
		case State::Down:	return "down";
	}
	return "?";
}

SpeechFlow::State SpeechFlow::GetState()
{
	std::lock_guard<std::mutex> lock(mutex);
	return state;
}

void SpeechFlow::Start()
{
	std::lock_guard<std::mutex> lock(mutex);
	if (stopped || state != State::Idle || opening)
		return;
	BeginOpen(NowMs());
}

void SpeechFlow::Stop()
{
	std::vector<std::shared_ptr<Link>> links;
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (stopped)
			return;
		stopped = true;
		++attempt;
		opening = false;
		for (std::shared_ptr<Link>* link : {&openingLink, &current, &retiring})
			if (*link)
				links.push_back(std::move(*link));
		for (auto& entry : abandoned)
			links.push_back(entry.first);
		abandoned.clear();
		state = State::Idle;
	}
	for (const std::shared_ptr<Link>& link : links)
		link->Close();
}

void SpeechFlow::BeginOpen(uint64_t nowMs)
{
	opening = true;
	openingDeadlineMs = nowMs + params.readyTimeoutMs;
	if (!current)
		state = State::Token;

	const uint64_t mine = ++attempt;
	std::weak_ptr<SpeechFlow> self = weak_from_this();
	service.GetTokens().Request(function, [self, mine](const SpeechTokenClient::Result& result) {
		if (std::shared_ptr<SpeechFlow> flow = self.lock())
			flow->OnToken(mine, result);
	});
}

void SpeechFlow::OnToken(uint64_t mine, const SpeechTokenClient::Result& result)
{
	Notice notice;
	std::shared_ptr<Link> link;
	std::string url;
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (stopped || mine != attempt)
			return;
		if (result.token.empty())
			OpeningFailed(result.error, NowMs(), notice);
		else
		{
			url = service.GetStreamingUrl();
			while (!url.empty() && url.back() == '/')
				url.pop_back();
			url += std::string("/streaming/") + SpeechFunctionName(function) + "/" + result.token;
			char separator = '?';
			for (const auto& entry : params.query)
			{
				url += separator + Escape(entry.first) + "=" + Escape(entry.second);
				separator = '&';
			}

			link = std::make_shared<Link>(weak_from_this());
			link->tokenPrefix = result.token.substr(0, 6);
			openingLink = link;
			if (!current)
				state = State::Connecting;
		}
	}
	if (!link)
		return Tell(notice);

	// Resolves the host and connects: this pool thread may wait, nobody else.
	if (service.GetServer().Connect(url, link))
		return;

	{
		std::lock_guard<std::mutex> lock(mutex);
		if (stopped || openingLink != link)
			return;
		openingLink.reset();
		OpeningFailed("cannot connect to " + service.GetStreamingUrl(), NowMs(), notice);
	}
	Tell(notice);
}

void SpeechFlow::OpeningFailed(const std::string& reason, uint64_t nowMs, Notice& notice)
{
	opening = false;
	service.RecordFailure(function, reason);

	const size_t index = std::min(retryIndex, params.retryMs.size() - 1);
	const uint64_t delay = params.retryMs.empty() ? 1000 : params.retryMs[index];

	if (current)
	{
		// A rotation that did not open: the old stream goes on serving.
		Error("-SpeechFlow: rotation failed, retry in %llu ms [function:%s,reason:%s]\n",
		      (unsigned long long)delay, SpeechFunctionName(function), reason.c_str());
		nextRotationMs = nowMs + delay;
		++retryIndex;
		return;
	}

	Error("-SpeechFlow: opening failed, retry in %llu ms [function:%s,reason:%s]\n",
	      (unsigned long long)delay, SpeechFunctionName(function), reason.c_str());
	state = State::Down;
	nextAttemptMs = nowMs + delay;
	++retryIndex;
	if (reported != 0)
	{
		reported = 0;
		notice.state = 0;
		notice.reason = reason;
	}
}

void SpeechFlow::Abandon(const std::shared_ptr<Link>& link, uint64_t nowMs)
{
	link->Close();
	abandoned.emplace_back(link, nowMs + params.retiringTimeoutMs);
}

void SpeechFlow::Tell(const Notice& notice)
{
	std::shared_ptr<Listener> target = listener.lock();
	if (!target)
		return;
	if (notice.ready)
		target->onReady(this);
	if (notice.state != -1)
		target->onState(this, notice.state == 1, notice.reason);
}

void SpeechFlow::Tick(uint64_t nowMs)
{
	Notice notice;
	std::shared_ptr<Link> keepAlive;
	bool askRotation = false;
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (stopped || state == State::Idle)
			return;

		if (state == State::Down && !opening && nowMs >= nextAttemptMs)
			BeginOpen(nowMs);

		if (opening && nowMs >= openingDeadlineMs)
		{
			++attempt;
			if (openingLink)
				Abandon(openingLink, nowMs);
			openingLink.reset();
			OpeningFailed("no ready in time", nowMs, notice);
		}

		if (current && nowMs - lastSentMs >= (uint64_t)params.keepAliveMs
		    && (function == SpeechFunction::Tts || paused))
		{
			keepAlive = current;
			lastSentMs = nowMs;
		}

		askRotation = current && !opening && nowMs - currentSinceMs >= (uint64_t)params.maxAgeMs
			&& nowMs >= nextRotationMs;

		if (retiring && nowMs >= retiringDeadlineMs)
		{
			Abandon(retiring, nowMs);
			retiring.reset();
		}

		for (auto it = abandoned.begin(); it != abandoned.end(); )
			it = nowMs >= it->second ? abandoned.erase(it) : it + 1;
	}

	if (keepAlive)
		keepAlive->Send(json{{"type", function == SpeechFunction::Tts ? "flush" : "pause"}}.dump());

	Tell(notice);

	if (!askRotation)
		return;
	std::shared_ptr<Listener> target = listener.lock();
	if (target && !target->CanRotate(this))
		return;

	std::lock_guard<std::mutex> lock(mutex);
	if (stopped || !current || opening)
		return;
	Log("-SpeechFlow: rotating the %s stream\n", SpeechFunctionName(function));
	BeginOpen(nowMs);
}

bool SpeechFlow::SendAudio(const BYTE* data, DWORD size)
{
	std::shared_ptr<Link> link;
	{
		std::lock_guard<std::mutex> lock(mutex);
		link = current;
		if (link)
			lastSentMs = NowMs();
	}
	if (link)
		link->Send(data, size);
	return link != nullptr;
}

bool SpeechFlow::SendJson(const json& message)
{
	std::shared_ptr<Link> link;
	{
		std::lock_guard<std::mutex> lock(mutex);
		link = current;
		if (link)
			lastSentMs = NowMs();
	}
	if (link)
		link->Send(message.dump());
	return link != nullptr;
}

bool SpeechFlow::SendText(const std::string& text)
{
	return SendJson(json{{"type", "text"}, {"text", text}});
}

bool SpeechFlow::Flush()
{
	return SendJson(json{{"type", "flush"}});
}

bool SpeechFlow::StopSpeaking()
{
	return SendJson(json{{"type", "stop"}});
}

bool SpeechFlow::Pause()
{
	{
		std::lock_guard<std::mutex> lock(mutex);
		paused = true;
	}
	return SendJson(json{{"type", "pause"}});
}

bool SpeechFlow::Resume()
{
	{
		std::lock_guard<std::mutex> lock(mutex);
		paused = false;
	}
	return SendJson(json{{"type", "resume"}});
}

void SpeechFlow::OnLinkMessage(Link* link, const json& message)
{
	const std::string type = message["type"];
	Notice notice;
	std::shared_ptr<Link> ended;
	bool pauseNew = false;
	std::shared_ptr<Link> promoted;
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (stopped)
			return;
		const bool isOpening = openingLink.get() == link;
		const bool isServing = current.get() == link || retiring.get() == link;

		if (type == "error")
		{
			const std::string reason = message.value("reason", "");
			if (message.value("fatal", false))
				link->fatalReason = reason;
			else
				Log("-SpeechFlow: %s error [reason:%s,token:%s]\n", SpeechFunctionName(function),
				    reason.c_str(), link->tokenPrefix.c_str());
			return;
		}

		if (type == "ready")
		{
			if (!isOpening)
				return;
			const uint64_t nowMs = NowMs();
			if (current)
			{
				// Rotation: the new stream takes over, the old one ends.
				ended = current;
				if (retiring)
					Abandon(retiring, nowMs);
				retiring = current;
				retiringDeadlineMs = nowMs + params.retiringTimeoutMs;
				service.RecordRotation();
			}
			else if (reported == 0)
				service.RecordReopening();

			current = openingLink;
			openingLink.reset();
			opening = false;
			currentSinceMs = nowMs;
			lastSentMs = nowMs;
			retryIndex = 0;
			nextRotationMs = 0;
			state = State::Ready;
			pauseNew = paused;
			promoted = current;

			Log("-SpeechFlow: %s stream ready [token:%s]\n", SpeechFunctionName(function), link->tokenPrefix.c_str());
			notice.ready = true;
			if (reported != 1)
			{
				reported = 1;
				notice.state = 1;
			}
		}
		else if (!isServing)
			return;
	}

	if (type == "ready")
	{
		if (pauseNew)
			promoted->Send(json{{"type", "pause"}}.dump());
		if (ended)
		{
			if (function == SpeechFunction::Asr)
				// Flushes the last text, then speech-server closes.
				ended->Send(json{{"type", "done"}}.dump());
			else
				ended->Close();
		}
		return Tell(notice);
	}

	std::shared_ptr<Listener> target = listener.lock();
	if (!target)
		return;

	if (type == "text")
		target->onText(this, message.value("text", ""));
	else if (type == "say")
		target->onSay(this, message.value("id", 0), message.value("text", ""));
	else if (type == "done")
		target->onDone(this, message.value("id", 0));
	else if (type == "stopped")
	{
		std::vector<int> ids;
		if (message.contains("ids") && message["ids"].is_array())
			for (const json& id : message["ids"])
				if (id.is_number_integer())
					ids.push_back(id.get<int>());
		target->onStopped(this, ids);
	}
	else
		Debug("-SpeechFlow: message %s ignored\n", type.c_str());
}

void SpeechFlow::OnLinkBinary(Link* link, const BYTE* data, DWORD size)
{
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (stopped || (current.get() != link && retiring.get() != link))
			return;
	}
	if (std::shared_ptr<Listener> target = listener.lock())
		target->onPcm(this, data, size);
}

void SpeechFlow::OnLinkClosed(Link* link)
{
	Notice notice;
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (stopped)
			return;
		const uint64_t nowMs = NowMs();

		for (auto it = abandoned.begin(); it != abandoned.end(); ++it)
			if (it->first.get() == link)
			{
				abandoned.erase(it);
				return;
			}

		if (retiring.get() == link)
		{
			retiring.reset();
			return;
		}

		const std::string reason = link->fatalReason.empty() ? "connection closed" : link->fatalReason;

		if (openingLink.get() == link)
		{
			openingLink.reset();
			++attempt;
			OpeningFailed(reason, nowMs, notice);
		}
		else if (current.get() == link)
		{
			Error("-SpeechFlow: %s stream lost [reason:%s,token:%s]\n", SpeechFunctionName(function),
			      reason.c_str(), link->tokenPrefix.c_str());
			current.reset();
			service.RecordFailure(function, reason);
			// A rotation in progress becomes the reopening.
			if (opening)
				state = openingLink ? State::Connecting : State::Token;
			else
			{
				state = State::Down;
				nextAttemptMs = nowMs + (params.retryMs.empty() ? 1000 : params.retryMs[0]);
				retryIndex = 1;
			}
			if (reported != 0)
			{
				reported = 0;
				notice.state = 0;
				notice.reason = reason;
			}
		}
		else
			return;
	}
	Tell(notice);
}
