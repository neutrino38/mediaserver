#ifndef SPEECH_ASRSESSION_H
#define SPEECH_ASRSESSION_H

#include <memory>
#include <mutex>
#include <string>
#include "speech/mistralrealtime.h"
#include "speech/speechbackend.h"
#include "speech/speechsession.h"

/**
 * asr (SPEC §7.4): client audio relayed to Voxtral Realtime, its text relayed
 * back. A relay, without a thread of its own.
 *
 * `ready` goes out once Mistral has accepted the delay: before that, nothing
 * says the backend answers. Audio received while Mistral (re)opens is held,
 * up to PendingAudioMs; beyond, it is dropped and the client told.
 */
class AsrSession :
	public SpeechSession,
	public MistralRealtimeClient::Listener
{
public:
	static constexpr size_t PendingAudioMs = 5000;
	static constexpr uint64_t FinishTimeoutMs = 5000;
	// From the request to session.updated: a backend that accepted the
	// connection and then stays mute must not hold the client forever.
	static constexpr uint64_t OpenTimeoutMs = 10000;

	AsrSession(const Params& params, const SpeechBackend& backend);

	//MistralRealtimeClient::Listener
	virtual void OnRealtimeReady(MistralRealtimeClient* client) override;
	virtual void OnRealtimeText(MistralRealtimeClient* client, const std::string& text) override;
	virtual void OnRealtimeDone(MistralRealtimeClient* client, const std::string& text) override;
	virtual void OnRealtimeError(MistralRealtimeClient* client, int code, const std::string& message) override;
	virtual void OnRealtimeClosed(MistralRealtimeClient* client) override;

protected:
	virtual void OnOpen() override;
	virtual void FillReady(nlohmann::json& ready) override;
	virtual void OnText(const nlohmann::json& message) override;
	virtual void OnBinary(const BYTE* data, DWORD size) override;
	virtual void OnTick(uint64_t nowMs) override;
	virtual void OnClosed() override;

private:
	// Caller holds `mutex`.
	void OpenMistral();
	void Relay(const BYTE* data, size_t size);

	const SpeechBackend&	backend;

	std::mutex		mutex;
	std::shared_ptr<MistralRealtimeClient> mistral;	// null when closed or closing
	bool			mistralReady = false;
	uint64_t		openingAtMs = 0;
	bool			readySent = false;
	bool			paused = false;
	// Mistral refuses a flush with no audio since the last one (error 3051),
	// and closes its session.
	bool			audioSinceFlush = false;
	uint64_t		pausedAtMs = 0;
	bool			finishing = false;
	uint64_t		finishingAtMs = 0;
	std::string		pending;	// audio held while Mistral opens
	bool			droppedTold = false;
	std::string		segment;	// text relayed since the last transcription.done
};

#endif
