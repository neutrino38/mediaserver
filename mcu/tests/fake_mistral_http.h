/**
 * fake_mistral_http.h — a local Voxtral TTS for the tts tests, on Abyss.
 *
 * POST /v1/audio/speech answers in SSE, like Mistral (SPEC §4.2). The text
 * chooses the behaviour: "REJECT" gives 403, "FAIL" gives 500, "SLOW" sends
 * ten deltas 300 ms apart, "FIXTURE" replays the lot 0 recording byte for
 * byte; anything else is three deltas of a 440 Hz sine, 0.2 s each.
 */
#ifndef FAKE_MISTRAL_HTTP_H
#define FAKE_MISTRAL_HTTP_H

#include <cmath>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>
#include "xmlrpcserver.h"
extern "C" {
#include <libavutil/base64.h>
}

class FakeMistralHttp : public Handler
{
public:
	static constexpr int Rate = 24000;
	static constexpr int DeltaSamples = 4800;	// 0.2 s

	struct Received
	{
		std::string	authorization;
		nlohmann::json	body;
	};

	explicit FakeMistralHttp(const std::string& fixture)
	{
		std::ifstream file(fixture, std::ios::binary);
		std::stringstream raw;
		raw << file.rdbuf();
		recording = raw.str();
	}

	static std::string Sine(int samples)
	{
		std::string out(samples * sizeof(float), '\0');
		float* f = (float*)&out[0];
		for (int i = 0; i < samples; ++i)
			f[i] = 0.5f * std::sin(2 * M_PI * 440.0 * i / Rate);
		return out;
	}

	static std::string Event(const std::string& type, const std::string& audio)
	{
		nlohmann::json data = {{"type", type}};
		if (!audio.empty())
		{
			std::string encoded(AV_BASE64_SIZE(audio.size()), '\0');
			av_base64_encode(&encoded[0], encoded.size(), (const uint8_t*)audio.data(), audio.size());
			encoded.resize(strlen(encoded.c_str()));
			data["audio_data"] = encoded;
		}
		return "event: " + type + "\ndata: " + data.dump() + "\n\n";
	}

	int ProcessRequest(TRequestInfo* req, TSession* const ses) override
	{
		const char* length = RequestHeaderValue(ses, (char*)"content-length");
		const char* auth = RequestHeaderValue(ses, (char*)"authorization");
		std::string body(length ? atoi(length) : 0, '\0');
		if (!body.empty())
			XmlRpcServer::GetBody(ses, &body[0], body.size());

		Received received{auth ? auth : "", nlohmann::json::parse(body, nullptr, false)};
		{
			std::lock_guard<std::mutex> lock(mutex);
			requests.push_back(received);
		}
		const std::string input = received.body.value("input", "");

		if (input.find("REJECT") != std::string::npos)
			return XmlRpcServer::SendError(ses, 403, "moderation: the text quotes REJECT");
		if (input.find("FAIL") != std::string::npos)
			return XmlRpcServer::SendError(ses, 500);

		std::vector<std::string> pieces;
		int pauseMs = 0;
		if (input.find("FIXTURE") != std::string::npos)
		{
			// Odd sizes: events and base64 cut anywhere.
			for (size_t pos = 0; pos < recording.size(); pos += 7777)
				pieces.push_back(recording.substr(pos, 7777));
		}
		else
		{
			const bool slow = input.find("SLOW") != std::string::npos;
			pauseMs = slow ? 300 : 0;
			for (int i = 0; i < (slow ? 10 : 3); ++i)
				pieces.push_back(Event("speech.audio.delta", Sine(DeltaSamples)));
			pieces.push_back(Event("speech.audio.done", ""));
		}

		size_t total = 0;
		for (const std::string& piece : pieces)
			total += piece.size();

		ResponseStatus(ses, 200);
		ResponseContentType(ses, (char*)"text/event-stream");
		ResponseContentLength(ses, total);
		ResponseWriteStart(ses);
		for (const std::string& piece : pieces)
		{
			if (!ResponseWriteBody(ses, (char*)piece.data(), piece.size()))
				break;
			if (pauseMs)
				std::this_thread::sleep_for(std::chrono::milliseconds(pauseMs));
		}
		ResponseWriteEnd(ses);
		return 1;
	}

	std::vector<Received> Requests()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return requests;
	}

private:
	std::mutex		mutex;
	std::vector<Received>	requests;
	std::string		recording;
};

#endif
