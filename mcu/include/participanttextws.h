#ifndef PARTICIPANTTEXTWS_H
#define PARTICIPANTTEXTWS_H

#include "worker.h"
#include <pthread.h>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include "config.h"
#include "websockets.h"
#include "text.h"
#include "task.h"

// Real-time text over WebSocket for a CONFERENCE participant — the
// conference-API mirror of the JSR-309 WSEndpoint (S5; jsr309_text_over_wss.md
// §11, implementation plan mcu_text_over_wss_impl_plan.md in the elixip repo).
//
// Unlike WSEndpoint this is NOT an RTP port. RTPParticipant has no swappable
// transport — its TextStream is a by-value member whose mixer pipes are wired
// once at Init — so the swap happens at the MIXER seam instead: the
// participant's text-RTP half is stopped and this bridge runs against the very
// same pipes (TextMixer::GetSharedInput/GetSharedOutput of the participant).
// Consequence: no RTP, no redundancy, no payload types here — both directions
// speak TextFrame, and RED remains a per-RTP-leg affair the other participants
// negotiate for themselves.
//
//   browser --WS--> onMessage*                --> mixerOutput->SendFrame()
//   browser <--WS-- pull thread GetFrame(...) <-- mixerInput (this leg's mix)
//
// Lifecycle: created and owned (shared_ptr) by MultiConf, which resolves the
// URL token and calls ws->Accept(weak_ptr to this). Init() starts the pull
// thread; End() stops it and closes the socket; both idempotent. A second
// browser connection on the same token replaces the first (onOpen closes it).
class ParticipantTextWS : public WebSocket::Listener, public Worker
{
public:
	ParticipantTextWS(std::shared_ptr<TextInput> mixerInput,
			  std::shared_ptr<TextOutput> mixerOutput);
	virtual ~ParticipantTextWS();

	int Init();
	int End();

	//WebSocket::Listener — runs on the WebSocketServer thread
	virtual void onOpen(WebSocket *ws);
	virtual void onMessageStart(WebSocket *ws,const WebSocket::MessageType type,const DWORD length);
	virtual void onMessageData(WebSocket *ws,const BYTE* data, const DWORD size);
	virtual void onMessageEnd(WebSocket *ws);
	virtual void onError(WebSocket *ws);
	virtual void onClose(WebSocket *ws);

protected:
	int PullText();

private:
	//Corps du Worker
	virtual int Run() { return PullText(); }

	void DeliverToWs(const BYTE *data, DWORD size);

	std::shared_ptr<TextInput>	mixerInput;
	std::shared_ptr<TextOutput>	mixerOutput;

	//Incoming WS message being assembled — touched only by the server thread
	std::string	incoming;

	//Timestamp origin for the frames we inject into the mixer
	timeval		clock;

	//Guards _ws and pending: the pull thread delivers while the server
	//thread connects/disconnects. WSEndpoint lives without this lock only
	//because both its sides run on server threads; here the pull thread is
	//ours, so the lock is not optional.
	std::mutex	mtx;
	//weak_ptr: the WebSocket belongs to the WebSocketServer; lock before
	//every use, never dereference a connection destroyed concurrently.
	std::weak_ptr<WebSocket> _ws;

	//Text mixed for this leg before the browser connected: between the
	//200 OK and the WS handshake there is a full SDP round-trip, and the
	//first sentence is the one where the caller introduces themselves.
	//Bounded in BOTH dimensions (32 frames / 5 s — the WSEndpoint §4.5
	//policy): an unbounded queue on a stream nobody may ever read is a leak.
	static const size_t maxPendingFrames = 32;
	static const QWORD  maxPendingAgeMs  = 5000;
	std::list<std::pair<QWORD,std::string>> pending;

	TaskState	pulling;
};

#endif /* PARTICIPANTTEXTWS_H */
