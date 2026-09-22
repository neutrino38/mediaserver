#include "participanttextws.h"
#include "log.h"
#include "tools.h"

static BYTE BOMUTF8[]		= {0xEF,0xBB,0xBF};
//U+FFFD REPLACEMENT CHARACTER: what T.140 §5.3 asks to insert in the stream
//when a session loss is detected — the only trace a user has that text is
//missing. Sent toward the side that SURVIVES: here the mixer, when the
//browser's WebSocket drops.
static BYTE REPLACEMENT_UTF8[]	= {0xEF,0xBF,0xBD};

ParticipantTextWS::ParticipantTextWS(std::shared_ptr<TextInput> mixerInput,
				     std::shared_ptr<TextOutput> mixerOutput) :
	mixerInput(mixerInput),
	mixerOutput(mixerOutput)
{
	pulling = TaskIdle;
	gettimeofday(&clock,NULL);
}

ParticipantTextWS::~ParticipantTextWS()
{
	End();
}

int ParticipantTextWS::Init()
{
	std::lock_guard<std::mutex> lock(lifecycle);

	if (pulling != TaskIdle)
		return Error("ParticipantTextWS::Init: already started.\n");

	pulling = TaskStarting;
	if (!StartThread())
	{
		pulling = TaskIdle;
		return Error("ParticipantTextWS::Init: pull thread still joinable.\n");
	}

	Log("ParticipantTextWS: mixer<->websocket text bridge started.\n");
	return 1;
}

int ParticipantTextWS::End()
{
	std::lock_guard<std::mutex> stop(lifecycle);

	//Stop the pull thread first: it must not deliver into a socket we are
	//about to close.
	if (pulling == TaskRunning || pulling == TaskStarting)
	{
		pulling = TaskStopping;
		//Unblock GetFrame
		mixerInput->Cancel();
	}
	//Joined unconditionally, outside the test above: StopThread() is
	//idempotent, and a stop that depends on a state read is a stop that can
	//be skipped while the thread is still alive.
	StopThread();
	pulling = TaskIdle;

	std::lock_guard<std::mutex> lock(mtx);
	if (std::shared_ptr<WebSocket> ws = _ws.lock())
		ws->Close();
	_ws.reset();
	pending.clear();

	return 1;
}

int ParticipantTextWS::PullText()
{
	//Do NOT overwrite a TaskStopping that End() posted while this thread was
	//starting: the flag would go back to TaskRunning, the loop would never
	//exit and StopThread() would join forever. A test then a store is not
	//enough — End() fits between the two, and the store wins. Observed in
	//production: pull thread at 100% of a core, End() joining forever.
	TaskState starting = TaskStarting;
	pulling.compare_exchange_strong(starting,TaskRunning);

	//Both conditions, because both can stop this loop on their own: Worker
	//lowers `running` in StopThread(), whoever calls it.
	while (IsThreadRunning() && pulling == TaskRunning)
	{
		//The per-leg mix destined to this participant. Blocks up to the
		//timeout; Cancel() (from End) unblocks it with NULL.
		TextFrame *frame = mixerInput->GetFrame(PullIdleMs);

		if (!frame)
		{
			//Timeout or cancel. No keepalive on the WebSocket side: WS
			//has no RTP idle semantics, and the historical clients never
			//expected one (jsr309_text_over_wss.md §5.5).
			//
			//`TextInput::GetFrame` only honours its timeout while the pipe
			//is inited: a pipe the text mixer has ended returns NULL at
			//once, and this loop would then spin on a core. StopThread()
			//cancels this wait, so the stop stays prompt.
			//Flag re-read BEFORE waiting: End() posts TaskStopping and
			//then cancels the wait; without this the thread can slip
			//between the two and sleep the whole interval.
			if (pulling == TaskRunning)
				wait.WaitSignal(PullIdleMs);
			continue;
		}

		//A lone BOM is T.140 keepalive plumbing, not conversation — do
		//not wake the browser up for it.
		if (!(frame->GetLength() == sizeof(BOMUTF8) &&
		      memcmp(frame->GetData(),BOMUTF8,sizeof(BOMUTF8)) == 0))
			DeliverToWs(frame->GetData(),frame->GetLength());

		delete frame;
	}

	pulling = TaskIdle;
	return 1;
}

void ParticipantTextWS::DeliverToWs(const BYTE *data, DWORD size)
{
	std::string msg((const char*)data,size);

	std::lock_guard<std::mutex> lock(mtx);

	if (std::shared_ptr<WebSocket> ws = _ws.lock())
	{
		ws->SendMessage(msg);
		return;
	}

	//No browser yet: keep the frame, bounded in number AND age.
	pending.push_back(std::make_pair((QWORD) getDifTime(&clock)/1000, msg));
	if (pending.size() > maxPendingFrames)
	{
		pending.pop_front();
		Log("ParticipantTextWS: pending text queue full, oldest frame dropped.\n");
	}
}

void ParticipantTextWS::onOpen(WebSocket *ws)
{
	std::lock_guard<std::mutex> lock(mtx);

	//A connection already existed (a reconnect on the same token): the new
	//one replaces it, as WSEndpoint::onOpen does.
	if (std::shared_ptr<WebSocket> old = _ws.lock())
		old->Close();

	_ws = ws->GetWeakPtr();

	//Replay the text mixed before the browser was there, in order, minus
	//the frames too old to still be dialogue.
	if (!pending.empty())
	{
		const QWORD now = getDifTime(&clock)/1000;
		size_t sent = 0, stale = 0;

		for (std::list<std::pair<QWORD,std::string>>::const_iterator it = pending.begin();
		     it != pending.end(); ++it)
		{
			if (now - it->first > maxPendingAgeMs) { stale++; continue; }
			ws->SendMessage(it->second);
			sent++;
		}

		Log("ParticipantTextWS: replayed %u pending text frame(s) on connect (%u dropped as stale).\n",
		    (unsigned) sent, (unsigned) stale);
		pending.clear();
	}
}

void ParticipantTextWS::onMessageStart(WebSocket *ws, const WebSocket::MessageType type, const DWORD length)
{
	incoming.clear();
	if (length)
		incoming.reserve(length);
}

void ParticipantTextWS::onMessageData(WebSocket *ws, const BYTE* data, const DWORD size)
{
	incoming.append((const char*)data,size);
}

void ParticipantTextWS::onMessageEnd(WebSocket *ws)
{
	//SetFrame parses the UTF-8 into the frame's wide string — which is what
	//PipeTextOutput::SendFrame consumes (GetWChar/GetWLength); raw bytes
	//appended without parsing would feed the mixer an empty string.
	TextFrame frame(getDifTime(&clock)/1000,
			(const BYTE*) incoming.data(), incoming.size());
	incoming.clear();

	//A lone BOM from the browser is keepalive, not text.
	if (frame.GetLength() == sizeof(BOMUTF8) &&
	    memcmp(frame.GetData(),BOMUTF8,sizeof(BOMUTF8)) == 0)
		return;

	if (frame.GetWLength())
		mixerOutput->SendFrame(frame);
}

void ParticipantTextWS::onError(WebSocket *ws)
{
	Error("ParticipantTextWS: websocket connection reported an error.\n");
}

void ParticipantTextWS::onClose(WebSocket *ws)
{
	std::lock_guard<std::mutex> lock(mtx);

	//Only reset if the closing connection is the current one (a reconnect
	//may have replaced it in the meantime, via onOpen).
	std::shared_ptr<WebSocket> cur = _ws.lock();
	if (cur.get() != ws)
		return;

	Log("ParticipantTextWS: connection associated with participant is closing.\n");
	_ws.reset();

	//T.140 §5.3: the surviving side — the conference — learns about the
	//loss with a U+FFFD in the stream.
	TextFrame lost(getDifTime(&clock)/1000,
		       REPLACEMENT_UTF8, sizeof(REPLACEMENT_UTF8));
	mixerOutput->SendFrame(lost);
}
