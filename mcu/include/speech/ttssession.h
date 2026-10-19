#ifndef SPEECH_TTSSESSION_H
#define SPEECH_TTSSESSION_H

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "speech/phrasesplitter.h"
#include "speech/speechbackend.h"
#include "speech/speechsession.h"

/**
 * tts (SPEC §7.3): T.140 text in, one Voxtral TTS request per phrase, PCM
 * S16LE 16 kHz out, framed by `say` and `done`.
 *
 * One request at a time, so that phrases are spoken in order. The request
 * runs in the TaskPool; it sends its audio under the session lock, which is
 * what guarantees that no audio follows `stopped`.
 */
class TtsSession : public SpeechSession
{
public:
	TtsSession(const Params& params, const SpeechBackend& backend);

protected:
	virtual void FillReady(nlohmann::json& ready) override;
	virtual void OnText(const nlohmann::json& message) override;
	virtual void OnTick(uint64_t nowMs) override;
	virtual void OnClosed() override;

private:
	struct Phrase
	{
		int		id;
		std::string	text;
	};

	// Caller holds `mutex`.
	void Enqueue(const std::vector<std::string>& phrases);
	void Pump();

	// TaskPool thread.
	void Speak(const Phrase& phrase, std::shared_ptr<std::atomic<bool>> cancel);

	const SpeechBackend&	backend;
	const std::string	voiceId;

	std::mutex		mutex;
	PhraseSplitter		splitter;
	std::deque<Phrase>	queue;
	int			nextId = 1;
	int			speakingId = 0;	// 0: none
	// Of the phrase being spoken; a stop sets it and forgets it.
	std::shared_ptr<std::atomic<bool>> cancel;
	bool			closed = false;
};

#endif
