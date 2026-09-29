#ifndef BFCPUDPTRANSPORT_H
#define BFCPUDPTRANSPORT_H


#include "bfcp/BFCPTransport.h"
#include "bfcp/BFCPMessage.h"
#include "pollhandler.h"
#include "ipaddress.h"
#include <mutex>
#include <vector>


class BFCPFloorControlServer;
class RtpSessionSet;


/**
 * The UDP leg of one participant, RFC 8855: one socket per user, and the
 * reliability the transport does not give.
 *
 * Three things follow from an unreliable transport, and all three are here:
 *
 *   - what the server STARTS is retransmitted until the peer acknowledges it,
 *     T1 doubling from 500 ms, given up past 16 s;
 *   - what the server ANSWERS is cached, so a request that arrives twice gets
 *     the same answer twice without being served twice;
 *   - the version is the peer's, not ours. libbfcp speaks version 1 over UDP,
 *     so the endpoints in service do too; a conforming peer speaks version 2.
 *     We answer in whichever we were addressed in.
 */
class BFCPUdpEndpoint : public BFCPTransport, public PollHandler
{
public:
	// RFC 8855 §6.2: T1 starts at 500 ms and doubles; a transaction that has
	// not been acknowledged by 16 s is given up, and its user with it.
	static constexpr QWORD T1InitialMs	= 500;
	static constexpr QWORD T1GiveUpMs	= 16000;
	// A datagram we accept. Ours are under 200 octets.
	static constexpr size_t MaxDatagramLen	= 8192;

public:
	BFCPUdpEndpoint(BFCPFloorControlServer* server, int userId, RtpSessionSet* reactor = NULL);
	~BFCPUdpEndpoint();

	// Binds and joins the reactor. Rend the bound port, 0 on failure.
	int Open(WORD port = 0);
	WORD GetPort() const			{ return port; }
	int GetUserId() const			{ return userId; }

	// Where the SDP says the peer is. The first datagram we receive overrides
	// it: that is what makes a NATted peer reachable.
	void SetRemote(const IPEndpoint& remote);
	bool HasRemote() const;
	// The Hello the server sends on its own over UDP, as libbfcp does.
	bool SendServerHello();

	// Transactions still waiting for an acknowledgement.
	int GetPendingCount() const;

	// BFCPTransport
	bool Send(const BFCPMessage& msg) override;
	void Close() override;
	bool IsReliable() const override	{ return false; }

	// PollHandler
	int GetPollFds(pollfd* fds, int max) override;
	int GetNextTimeoutMs(QWORD nowUs) override;
	void OnPollEvents(const pollfd* fds, int count, QWORD nowUs) override;
	void OnPeriodic(QWORD nowUs) override;
	void OnPollError(short revents) override;

	bool IsFinished() const;

private:
	// One message the server started, still unacknowledged.
	struct Pending
	{
		int			transactionId;
		std::vector<BYTE>	datagram;
		QWORD			dueUs;
		QWORD			intervalMs;
		QWORD			firstSentUs;
	};

	// One answer we gave, kept so a repeated request gets it again.
	struct Cached
	{
		int			transactionId;
		std::vector<BYTE>	datagram;
		QWORD			expiresUs;
	};

	void OnReadable(QWORD nowUs);
	void Dispatch(const BYTE* data, size_t size, QWORD nowUs);
	bool SendDatagramLocked(const BYTE* data, size_t size);
	void AckLocked(int transactionId);
	const Cached* FindCachedLocked(int transactionId) const;
	void Finish();

private:
	int			fd;
	WORD			port;
	BFCPFloorControlServer*	server;
	int			userId;
	RtpSessionSet*		reactor;

	mutable std::mutex	mutex;
	IPEndpoint		remote;
	bool			hasRemote;
	bool			latched;
	int			peerVersion;
	// For the messages the endpoint originates itself, outside any server
	// transaction: a Hello sent before the peer has said anything.
	int			nextTransactionId;
	std::vector<Pending>	pending;
	std::vector<Cached>	cache;
	bool			finished;
};


#endif /* BFCPUDPTRANSPORT_H */
