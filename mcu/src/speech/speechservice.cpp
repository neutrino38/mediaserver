#include "speech/speechservice.h"

#include "log.h"
#include "websocketserver.h"

namespace
{

bool StartsWith(const std::string& text, const char* prefix)
{
	return text.rfind(prefix, 0) == 0;
}

}

SpeechService::SpeechService(WebSocketServer& server, const std::string& controlUrl, const std::string& streamingUrl,
			     const std::string& account, int tickMs) :
	server(server),
	controlUrl(controlUrl),
	streamingUrl(streamingUrl),
	account(account),
	tickMs(tickMs)
{
	if (controlUrl.empty() || streamingUrl.empty())
		return;
	tasks = std::make_unique<TaskPool>(TaskThreads);
	tokens = std::make_unique<SpeechTokenClient>(*tasks, controlUrl, account);
}

SpeechService::~SpeechService()
{
	End();
}

bool SpeechService::CheckConfig(const std::string& controlUrl, const std::string& streamingUrl,
				const std::string& account, std::string& error)
{
	if (controlUrl.empty() && streamingUrl.empty())
	{
		if (!account.empty())
			error = "--speech-account without --speech-url";
		return account.empty();
	}
	if (controlUrl.empty() || streamingUrl.empty())
	{
		error = "--speech-url and --speech-streaming-url go together";
		return false;
	}
	if (!StartsWith(controlUrl, "http://") && !StartsWith(controlUrl, "https://"))
	{
		error = "--speech-url must be an http:// or https:// URL (the control port)";
		return false;
	}
	WebSocketServer::Target target;
	if (!WebSocketServer::ParseWsUrl(streamingUrl, target))
	{
		error = "--speech-streaming-url must be a ws:// or wss:// URL (the streaming port)";
		return false;
	}
	// The same rule as speech-server: 1 to 64 characters of [A-Za-z0-9._-].
	if (account.size() > 64)
	{
		error = "--speech-account is longer than 64 characters";
		return false;
	}
	for (char c : account)
		if (!isalnum((unsigned char)c) && c != '.' && c != '_' && c != '-')
		{
			error = "--speech-account allows [A-Za-z0-9._-] only";
			return false;
		}
	return true;
}

void SpeechService::Start()
{
	if (!IsConfigured())
		return;
	Log("-SpeechService: speech-server at %s, streams at %s [account:%s]\n",
	    controlUrl.c_str(), streamingUrl.c_str(), account.empty() ? "-" : account.c_str());
	StartThread();
}

void SpeechService::End()
{
	StopThread();

	std::vector<std::weak_ptr<SpeechFlow>> all;
	{
		std::lock_guard<std::mutex> lock(mutex);
		all.swap(flows);
	}
	for (const std::weak_ptr<SpeechFlow>& weak : all)
		if (std::shared_ptr<SpeechFlow> flow = weak.lock())
			flow->Stop();

	if (tasks)
		tasks->Stop();
}

std::shared_ptr<SpeechFlow> SpeechService::CreateFlow(SpeechFunction function, std::weak_ptr<SpeechFlow::Listener> listener,
						      const SpeechFlow::Params& params)
{
	if (!IsConfigured())
		return nullptr;
	auto flow = std::make_shared<SpeechFlow>(*this, function, listener, params);
	std::lock_guard<std::mutex> lock(mutex);
	flows.push_back(flow);
	return flow;
}

SpeechService::Counters SpeechService::GetCounters()
{
	std::vector<std::shared_ptr<SpeechFlow>> live;
	Counters result;
	{
		std::lock_guard<std::mutex> lock(mutex);
		result = counters;
		for (const std::weak_ptr<SpeechFlow>& weak : flows)
			if (std::shared_ptr<SpeechFlow> flow = weak.lock())
				live.push_back(flow);
	}
	// Counted from the flows themselves: a second tally would drift.
	for (const std::shared_ptr<SpeechFlow>& flow : live)
		if (flow->GetState() == SpeechFlow::State::Ready)
			++result.open[(int)flow->GetFunction()];
	return result;
}

void SpeechService::RecordFailure(SpeechFunction function, const std::string& reason)
{
	std::lock_guard<std::mutex> lock(mutex);
	++counters.failures;
	counters.lastFailureAt = time(NULL);
	counters.lastFailureFunction = SpeechFunctionName(function);
	counters.lastFailureReason = reason;
}

void SpeechService::RecordReopening()
{
	std::lock_guard<std::mutex> lock(mutex);
	++counters.reopenings;
}

void SpeechService::RecordRotation()
{
	std::lock_guard<std::mutex> lock(mutex);
	++counters.rotations;
}

int SpeechService::Run()
{
	while (IsThreadRunning())
	{
		std::vector<std::shared_ptr<SpeechFlow>> live;
		{
			std::lock_guard<std::mutex> lock(mutex);
			for (auto it = flows.begin(); it != flows.end(); )
			{
				if (std::shared_ptr<SpeechFlow> flow = it->lock())
				{
					live.push_back(flow);
					++it;
				}
				else
					it = flows.erase(it);
			}
		}

		const uint64_t nowMs = SpeechFlow::NowMs();
		for (const std::shared_ptr<SpeechFlow>& flow : live)
			flow->Tick(nowMs);
		live.clear();

		wait.WaitSignal(tickMs);
	}
	return 0;
}
