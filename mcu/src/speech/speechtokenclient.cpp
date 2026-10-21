#include "speech/speechtokenclient.h"

#include <nlohmann/json.hpp>
#include "speech/mistralhttp.h"

using json = nlohmann::json;

SpeechTokenClient::SpeechTokenClient(TaskPool& tasks, const std::string& controlUrl, const std::string& account) :
	tasks(tasks),
	controlUrl(controlUrl),
	account(account)
{
}

void SpeechTokenClient::Request(SpeechFunction function, std::function<void(const Result&)> done)
{
	const std::string url = controlUrl;
	const std::string account = this->account;
	tasks.Post([url, account, function, done] { done(Fetch(url, account, function)); });
}

SpeechTokenClient::Result SpeechTokenClient::Fetch(const std::string& controlUrl, const std::string& account, SpeechFunction function)
{
	json request = {{"function", SpeechFunctionName(function)}};
	if (!account.empty())
		request["account"] = account;

	std::string body;
	const MistralHttp::Result http = MistralHttp::Post(controlUrl, "/control/token", "", request.dump(), "application/json",
		[&body](const char* data, size_t size) {
			// A token answer is a few hundred bytes.
			if (body.size() + size > 4096)
				return false;
			body.append(data, size);
			return true;
		});

	Result result;
	if (http.cancelled)
	{
		result.error = "token: oversized answer";
		return result;
	}
	if (!http.error.empty())
	{
		result.error = "token: " + http.error;
		return result;
	}

	const json answer = json::parse(body, nullptr, false);
	if (answer.is_object() && answer.contains("token") && answer["token"].is_string())
		result.token = answer["token"].get<std::string>();
	if (result.token.empty())
		result.error = "token: unreadable answer";
	return result;
}
