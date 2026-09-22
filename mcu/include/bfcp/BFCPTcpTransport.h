#ifndef BFCPTCPTRANSPORT_H
#define BFCPTCPTRANSPORT_H


#include "bfcp/BFCPTransport.h"
#include "bfcp/BFCPMessage.h"
#include "pollhandler.h"
#include "ipaddress.h"
#include <memory>
#include <mutex>
#include <vector>


class BFCPFloorControlServer;
class RtpSessionSet;
class BFCPTcpListener;


/**
 * One accepted TCP connection, RFC 4582 §7: a stream of messages, each one a
 * 12-octet header whose Payload Length says how much follows.
 *
 * Beaten by the BFCP reactor. Everything below runs on its thread, EXCEPT
 * Send() and Close(), which the chair calls from the XML-RPC thread.
 */
class BFCPTcpConnection : public BFCPTransport, public PollHandler
{
public:
	// A message longer than this is refused and the connection dropped. The
	// protocol allows 262152 octets; a conference with one floor never comes
	// close, and a peer that asks for more is not one of ours.
	static constexpr size_t MaxMessageLen = 65536;

public:
	BFCPTcpConnection(int fd, BFCPFloorControlServer* server, const IPEndpoint& peer);
	~BFCPTcpConnection();

	// BFCPTransport — called by the floor control server, from any thread.
	bool Send(const BFCPMessage& msg) override;
	void Close() override;
	bool IsReliable() const override	{ return true; }

	// PollHandler — called by the reactor, on its thread.
	int GetPollFds(pollfd* fds, int max) override;
	int GetNextTimeoutMs(QWORD nowUs) override;
	void OnPollEvents(const pollfd* fds, int count, QWORD nowUs) override;
	void OnPeriodic(QWORD nowUs) override;
	void OnPollError(short revents) override;

	// True once the connection is done: the listener reaps it, on the reactor
	// thread, where nothing is running inside it any more.
	bool IsFinished() const;
	const IPEndpoint& GetPeer() const	{ return peer; }

private:
	// Drains the socket into `in`, then hands up every whole message it holds.
	void OnReadable();
	void OnWritable();
	// Writes what it can, queues the rest. The mutex must be held.
	void SendLocked(const BYTE* data, size_t size);
	void Finish();

private:
	int			fd;
	BFCPFloorControlServer*	server;
	IPEndpoint		peer;

	mutable std::mutex	mutex;
	std::vector<BYTE>	in;
	std::vector<BYTE>	out;
	bool			finished;
};


/**
 * The listening socket of one conference. Dual-stack, like every other listen
 * in the mcu: one AF_INET6 socket with IPV6_V6ONLY cleared, so an IPv4 client
 * arrives as ::ffff:a.b.c.d.
 */
class BFCPTcpListener : public PollHandler
{
public:
	BFCPTcpListener(BFCPFloorControlServer* server, RtpSessionSet* reactor = NULL);
	~BFCPTcpListener();

	// Binds, listens and joins the reactor. Rend the bound port, 0 on failure.
	// Port 0 lets the kernel choose, and the chosen one is what we announce.
	int Open(WORD port = 0);
	void Close();
	WORD GetPort() const			{ return port; }
	int GetConnectionCount();

	// PollHandler
	int GetPollFds(pollfd* fds, int max) override;
	int GetNextTimeoutMs(QWORD nowUs) override;
	void OnPollEvents(const pollfd* fds, int count, QWORD nowUs) override;
	void OnPeriodic(QWORD nowUs) override;
	void OnPollError(short revents) override;

private:
	void Accept();
	void Reap();

private:
	int			fd;
	WORD			port;
	BFCPFloorControlServer*	server;
	RtpSessionSet*		reactor;

	mutable std::mutex	mutex;
	std::vector<std::unique_ptr<BFCPTcpConnection> > connections;
};


#endif /* BFCPTCPTRANSPORT_H */
