#include "speech/compsession.h"

#include <ctime>
#include "log.h"
#include "speech/compcheck.h"
#include "speech/mistralchat.h"

using json = nlohmann::json;

namespace
{

const char* const CorrectionPrompt =
#include "comp_prompt.inc"
;

size_t CountChars(const std::string& utf8)
{
	size_t count = 0;
	for (unsigned char c : utf8)
		if ((c & 0xC0) != 0x80)
			++count;
	return count;
}

}

CompSession::CompSession(const Params& params, const SpeechBackend& backend) :
	SpeechSession(params),
	backend(backend)
{
}

const char* CompSession::Prompt()
{
	return CorrectionPrompt;
}

void CompSession::FillReady(json& ready)
{
	ready["lang"] = params.lang;
}

void CompSession::SendFailure(const json& id, const std::string& reason)
{
	Send(json{{"type", "error"}, {"id", id}, {"reason", reason}, {"fatal", false}});
}

void CompSession::OnText(const json& message)
{
	if (message["type"] != "check")
		return SendError("unexpected message", false);

	if (!message.contains("id") || !(message["id"].is_number_integer() || message["id"].is_string()))
		return SendError("check needs an id, number or string", false);
	const json id = message["id"];

	if (!message.contains("text") || !message["text"].is_string()
	    || (message.contains("context") && !message["context"].is_string()))
		return SendFailure(id, "text and context must be strings");

	Check check{id, message.value("context", ""), message["text"]};
	if (CountChars(check.text) > MaxChars || CountChars(check.context) > MaxChars)
		return SendFailure(id, "too long");

	std::lock_guard<std::mutex> lock(mutex);
	if (!running)
		return Start(check);
	if (waiting)
		SendFailure(waiting->id, "superseded");
	waiting = std::make_unique<Check>(check);
}

void CompSession::Start(const Check& check)
{
	running = true;
	std::weak_ptr<SpeechSession> self = shared_from_this();
	backend.tasks->Post([self, check]() {
		if (std::shared_ptr<SpeechSession> session = self.lock())
			std::static_pointer_cast<CompSession>(session)->Run(check);
	});
}

void CompSession::Run(const Check& check)
{
	// Nothing to correct: no need to ask.
	if (check.text.find_first_not_of(" \t\r\n") == std::string::npos)
		Send(json{{"type", "checked"}, {"id", check.id}, {"text", check.text}, {"changes", json::array()}});
	else
	{
		const MistralChat::Request request = {
			params.config->mistralUrl, backend.apiKey, params.config->compModel, CorrectionPrompt,
			json{{"langue", params.lang}, {"contexte", check.context}, {"texte", check.text}}.dump()};
		const MistralChat::Result result = MistralChat::Complete(request);
		backend.usage->AddCompTokens(params.account, result.tokens);

		if (!result.error.empty())
		{
			Error("-CompSession: Mistral chat failed: %s [token:%s]\n", result.error.c_str(), params.tokenPrefix.c_str());
			backend.health->Failure(SpeechFunction::Comp, time(NULL), result.error);
			SendFailure(check.id, "backend unavailable");
		}
		else
		{
			const CompCheck::Result corrected = CompCheck::Apply(check.text, result.content);
			if (!corrected.valid)
			{
				backend.health->Discarded(SpeechFunction::Comp);
				SendFailure(check.id, "unreadable answer");
			}
			else
			{
				backend.health->Success(SpeechFunction::Comp, time(NULL));
				json changes = json::array();
				for (const CompCheck::Change& change : corrected.changes)
					changes.push_back(json{{"start", change.start}, {"end", change.end}, {"from", change.from}, {"to", change.to}});
				Send(json{{"type", "checked"}, {"id", check.id}, {"text", corrected.text}, {"changes", changes}});
			}
		}
	}

	std::lock_guard<std::mutex> lock(mutex);
	running = false;
	if (waiting)
	{
		const Check next = *waiting;
		waiting.reset();
		Start(next);
	}
}

void CompSession::OnClosed()
{
	std::lock_guard<std::mutex> lock(mutex);
	waiting.reset();
}
