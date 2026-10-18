#ifndef SPEECH_MISTRALSPEECH_H
#define SPEECH_MISTRALSPEECH_H

#include <functional>
#include <string>

/**
 * Server-Sent Events, fed as the bytes arrive (SPEC §4.2: the TTS stream).
 */
class SseParser
{
public:
	typedef std::function<void(const std::string& event, const std::string& data)> Handler;

	explicit SseParser(Handler handler) : handler(handler) {}

	void Feed(const char* data, size_t size);

private:
	void Line(const std::string& line);

	Handler		handler;
	std::string	buffer;
	std::string	event;
	std::string	data;
};

/**
 * One Voxtral TTS request in streaming mode: POST /v1/audio/speech, SSE back,
 * PCM float32 24 kHz in each speech.audio.delta.
 *
 * Blocking: it runs in a TaskPool thread, never on the reactor.
 */
class MistralSpeech
{
public:
	static constexpr int Rate = 24000;	// measured at lot 0, undocumented

	struct Request
	{
		std::string	baseUrl;	// mistral.url
		std::string	apiKey;
		std::string	model;
		std::string	voice;		// Mistral voice id or slug
		std::string	text;
	};

	struct Result
	{
		long		httpStatus = 0;	// 0: no answer
		bool		cancelled = false;
		std::string	error;		// empty on success
	};

	// `onAudio` receives float32 bytes; returning false cancels the request.
	static Result Synthesize(const Request& request, std::function<bool(const std::string&)> onAudio);
};

#endif
