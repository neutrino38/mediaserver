#ifndef SPEECH_COMPSESSION_H
#define SPEECH_COMPSESSION_H

#include <memory>
#include <mutex>
#include <string>
#include <nlohmann/json.hpp>
#include "speech/speechbackend.h"
#include "speech/speechsession.h"

/**
 * comp (SPEC §7.5): `check` in, `checked` out. The server corrects and keeps
 * nothing between two checks; the client splits, asks, validates, then sends
 * the text in T.140.
 *
 * One check runs at a time; one more may wait; a newer one replaces it, and
 * the replaced one is answered `superseded`.
 */
class CompSession : public SpeechSession
{
public:
	static constexpr size_t MaxChars = 500;

	CompSession(const Params& params, const SpeechBackend& backend);

	// The versioned prompt (src/speech/comp_prompt.txt).
	static const char* Prompt();

protected:
	virtual void FillReady(nlohmann::json& ready) override;
	virtual void OnText(const nlohmann::json& message) override;
	virtual void OnClosed() override;

private:
	struct Check
	{
		nlohmann::json	id;
		std::string	context;
		std::string	text;
	};

	// Caller holds `mutex`.
	void Start(const Check& check);
	// TaskPool thread.
	void Run(const Check& check);
	void SendFailure(const nlohmann::json& id, const std::string& reason);

	const SpeechBackend&	backend;

	std::mutex		mutex;
	bool			running = false;
	std::unique_ptr<Check>	waiting;
};

#endif
