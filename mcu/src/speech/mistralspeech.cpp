#include "speech/mistralspeech.h"

#include <mutex>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
extern "C" {
#include <libavutil/base64.h>
}

using json = nlohmann::json;

void SseParser::Feed(const char* chunk, size_t size)
{
	buffer.append(chunk, size);
	size_t start = 0;
	for (size_t end; (end = buffer.find('\n', start)) != std::string::npos; start = end + 1)
	{
		std::string line = buffer.substr(start, end - start);
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		Line(line);
	}
	buffer.erase(0, start);
}

void SseParser::Line(const std::string& line)
{
	if (line.empty())
	{
		if (!data.empty() || !event.empty())
			handler(event, data);
		event.clear();
		data.clear();
		return;
	}
	if (line[0] == ':')
		return;

	const size_t colon = line.find(':');
	const std::string field = line.substr(0, colon);
	std::string value = colon == std::string::npos ? "" : line.substr(colon + 1);
	if (!value.empty() && value[0] == ' ')
		value.erase(0, 1);

	if (field == "event")
		event = value;
	else if (field == "data")
		data += data.empty() ? value : "\n" + value;
}

namespace
{

struct Transfer
{
	std::function<bool(const std::string&)>	onAudio;
	SseParser*				parser;
	bool					cancelled = false;
	CURL*					curl;
};

size_t OnBody(char* data, size_t size, size_t count, void* user)
{
	Transfer* transfer = (Transfer*)user;
	const size_t bytes = size * count;

	// An error body may quote the user's text: it is neither parsed nor kept.
	long status = 0;
	curl_easy_getinfo(transfer->curl, CURLINFO_RESPONSE_CODE, &status);
	if (status != 200)
		return bytes;

	transfer->parser->Feed(data, bytes);
	return transfer->cancelled ? 0 : bytes;
}

}

MistralSpeech::Result MistralSpeech::Synthesize(const Request& request, std::function<bool(const std::string&)> onAudio)
{
	static std::once_flag init;
	std::call_once(init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

	Result result;
	CURL* curl = curl_easy_init();
	if (!curl)
	{
		result.error = "curl_easy_init failed";
		return result;
	}

	Transfer transfer;
	transfer.onAudio = onAudio;
	transfer.curl = curl;
	bool streamError = false;
	SseParser parser([&](const std::string& event, const std::string& data) {
		if (transfer.cancelled)
			return;
		const json payload = json::parse(data, nullptr, false);
		if (payload.is_discarded() || !payload.is_object())
			return;
		const std::string type = payload.value("type", event);
		if (type == "speech.audio.delta" && payload.contains("audio_data") && payload["audio_data"].is_string())
		{
			const std::string encoded = payload["audio_data"];
			std::string audio(AV_BASE64_DECODE_SIZE(encoded.size()), '\0');
			const int decoded = av_base64_decode((uint8_t*)&audio[0], encoded.c_str(), audio.size());
			if (decoded < 0)
				return;
			audio.resize(decoded);
			if (!transfer.onAudio(audio))
				transfer.cancelled = true;
		}
		else if (type == "error")
			streamError = true;
	});
	transfer.parser = &parser;

	std::string base = request.baseUrl;
	while (!base.empty() && base.back() == '/')
		base.pop_back();
	const std::string url = base + "/v1/audio/speech";
	const std::string body = json{
		{"model", request.model},
		{"input", request.text},
		{"voice", request.voice},
		{"response_format", "pcm"},
		{"stream", true}}.dump();
	const std::string authorization = "Authorization: Bearer " + request.apiKey;

	curl_slist* headers = nullptr;
	headers = curl_slist_append(headers, authorization.c_str());
	headers = curl_slist_append(headers, "Content-Type: application/json");
	headers = curl_slist_append(headers, "Accept: text/event-stream");

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, OnBody);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
	// A stream that stalls for 15 s is dead.
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 15L);

	const CURLcode code = curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.httpStatus);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	result.cancelled = transfer.cancelled;
	if (result.cancelled)
		return result;
	if (code != CURLE_OK)
		result.error = curl_easy_strerror(code);
	else if (result.httpStatus != 200)
		result.error = "HTTP " + std::to_string(result.httpStatus);
	else if (streamError)
		result.error = "error event in the stream";
	return result;
}
