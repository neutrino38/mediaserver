#include "speech/mistralspeech.h"

#include "speech/mistralhttp.h"

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

MistralSpeech::Result MistralSpeech::Synthesize(const Request& request, std::function<bool(const std::string&)> onAudio)
{
	bool cancelled = false;
	bool streamError = false;
	SseParser parser([&](const std::string& event, const std::string& data) {
		if (cancelled)
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
			if (!onAudio(audio))
				cancelled = true;
		}
		else if (type == "error")
			streamError = true;
	});

	const std::string body = json{
		{"model", request.model},
		{"input", request.text},
		{"voice", request.voice},
		{"response_format", "pcm"},
		{"stream", true}}.dump();

	const MistralHttp::Result http = MistralHttp::Post(request.baseUrl, "/v1/audio/speech", request.apiKey,
		body, "text/event-stream", [&](const char* data, size_t size) {
			parser.Feed(data, size);
			return !cancelled;
		});

	Result result;
	result.httpStatus = http.httpStatus;
	result.cancelled = http.cancelled;
	result.error = http.error;
	if (!result.cancelled && result.error.empty() && streamError)
		result.error = "error event in the stream";
	return result;
}
