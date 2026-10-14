#include "speech/mistralchat.h"

#include <nlohmann/json.hpp>
#include "speech/mistralhttp.h"

using json = nlohmann::json;

namespace
{

// A correction answer is a few dozen tokens.
constexpr int MaxTokens = 300;
constexpr size_t MaxBody = 64*1024;

}

MistralChat::Result MistralChat::Complete(const Request& request)
{
	const std::string body = json{
		{"model", request.model},
		{"temperature", 0},
		{"max_tokens", MaxTokens},
		{"response_format", {{"type", "json_object"}}},
		{"messages", json::array({
			{{"role", "system"}, {"content", request.system}},
			{{"role", "user"}, {"content", request.user}}})}}.dump();

	std::string answer;
	const MistralHttp::Result http = MistralHttp::Post(request.baseUrl, "/v1/chat/completions", request.apiKey,
		body, "application/json", [&](const char* data, size_t size) {
			answer.append(data, size);
			return answer.size() <= MaxBody;
		});

	Result result;
	result.httpStatus = http.httpStatus;
	result.error = http.cancelled ? "answer too large" : http.error;
	if (!result.error.empty())
		return result;

	const json parsed = json::parse(answer, nullptr, false);
	if (parsed.is_discarded() || !parsed.is_object())
	{
		result.error = "unreadable answer";
		return result;
	}
	if (parsed.contains("usage") && parsed["usage"].is_object())
		result.tokens = parsed["usage"].value("total_tokens", 0);
	if (parsed.contains("choices") && parsed["choices"].is_array() && !parsed["choices"].empty()
	    && parsed["choices"][0].contains("message") && parsed["choices"][0]["message"].contains("content")
	    && parsed["choices"][0]["message"]["content"].is_string())
		result.content = parsed["choices"][0]["message"]["content"];
	else
		result.error = "answer without content";
	return result;
}
