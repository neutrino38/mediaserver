#include "bfcp/BFCPUdpTransport.h"
#include "bfcp/BFCPFloorControlServer.h"
#include "rtpsessionset.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>


BFCPUdpEndpoint::BFCPUdpEndpoint(BFCPFloorControlServer* server, int userId, RtpSessionSet* reactor) :
	fd(FD_INVALID),
	port(0),
	server(server),
	userId(userId),
	reactor(reactor ? reactor : &RtpSessionSet::Default()),
	hasRemote(false),
	latched(false),
	// Until the peer has spoken, RFC 8855 is what we assume. Its first
	// datagram tells us whether it is really a version 1 endpoint.
	peerVersion(BFCPMessage::VersionUnreliable),
	nextTransactionId(0),
	finished(false)
{
}


BFCPUdpEndpoint::~BFCPUdpEndpoint()
{
	if (fd != FD_INVALID) {
		reactor->Remove(this);
		close(fd);
	}
}


int BFCPUdpEndpoint::Open(WORD port)
{
	if (fd != FD_INVALID)
		return ::Error("BFCPUdpEndpoint::Open() | already open on port %d\n", this->port);

	// One AF_INET6 socket for both families, like every other socket in the
	// mcu: an IPv4 peer is reached through a v4-mapped address.
	fd = socket(AF_INET6, SOCK_DGRAM, 0);
	if (fd < 0) {
		fd = FD_INVALID;
		return ::Error("BFCPUdpEndpoint::Open() | cannot create socket, errno %d\n", errno);
	}

	int v6only = 0;
	if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)) < 0)
		::Error("BFCPUdpEndpoint::Open() | cannot clear IPV6_V6ONLY (errno %d) — IPv4 peers will not be served\n", errno);

	const IPEndpoint bindTo = IPAddress::Any(AF_INET6).To(port);
	if (bind(fd, bindTo, bindTo.Len()) < 0) {
		::Error("BFCPUdpEndpoint::Open() | cannot bind port %d, errno %d\n", port, errno);
		close(fd);
		fd = FD_INVALID;
		return 0;
	}

	const int flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	IPEndpoint bound;
	if (getsockname(fd, bound.Data(), bound.LenPtr()) == 0)
		this->port = IPAddress::PortOf(bound.Data());
	else
		this->port = port;

	reactor->Add(this);

	::Log("BFCPUdpEndpoint::Open() | user '%d' listening on port %d, both families\n", userId, this->port);
	return this->port;
}


void BFCPUdpEndpoint::SetRemote(const IPEndpoint& remote)
{
	std::lock_guard<std::mutex> lock(mutex);

	// Only until the peer speaks: what it says beats what the SDP claimed,
	// which is the whole point behind a NAT.
	if (latched)
		return;

	this->remote	= remote;
	this->hasRemote	= true;

	::Log("BFCPUdpEndpoint::SetRemote() | user '%d' will send to %s\n", userId, remote.ToString().c_str());
}


bool BFCPUdpEndpoint::HasRemote() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return hasRemote;
}


bool BFCPUdpEndpoint::IsFinished() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return finished;
}


void BFCPUdpEndpoint::Finish()
{
	finished = true;
	pending.clear();
}


void BFCPUdpEndpoint::Close()
{
	std::lock_guard<std::mutex> lock(mutex);
	Finish();
}


bool BFCPUdpEndpoint::SendServerHello()
{
	// libbfcp greets back after answering a HelloAck, and the endpoints in
	// service expect it. It is a transaction of ours, so it needs an id of its
	// own: without one the peer has nothing to name in its acknowledgement,
	// and we would retransmit until we gave up on a peer that answered.
	int transactionId;
	{
		std::lock_guard<std::mutex> lock(mutex);
		nextTransactionId = (nextTransactionId % 0xFFFF) + 1;
		transactionId = nextTransactionId;
	}

	BFCPMsgHello hello(transactionId, server->GetConferenceId(), userId);
	return Send(hello);
}


bool BFCPUdpEndpoint::Send(const BFCPMessage& msg)
{
	std::lock_guard<std::mutex> lock(mutex);

	if (finished || fd == FD_INVALID)
		return false;

	if (! hasRemote)
		return ::Error("BFCPUdpEndpoint::Send() | user '%d' has no remote yet, dropping a %s\n", userId, BFCPMessage::PrimitiveName(msg.GetPrimitive()));

	BYTE datagram[MaxDatagramLen];
	const size_t size = msg.Serialize(datagram, sizeof(datagram), peerVersion);
	if (size == BFCPMessage::Failed)
		return ::Error("BFCPUdpEndpoint::Send() | cannot serialize a %s\n", BFCPMessage::PrimitiveName(msg.GetPrimitive()));

	if (! SendDatagramLocked(datagram, size))
		return false;

	// The R flag says who started the transaction (RFC 8855 §5.1), and that is
	// exactly what decides the treatment below.
	if (msg.IsResponder()) {
		// An answer. Keep it: the request may well come again, and serving it
		// twice would, for instance, create a second FloorRequest.
		Cached entry;
		entry.transactionId	= msg.GetTransactionId();
		entry.datagram.assign(datagram, datagram + size);
		entry.expiresUs		= getTime() + T1GiveUpMs * 1000;

		for (size_t i=0; i<cache.size(); i++)
			if (cache[i].transactionId == entry.transactionId) {
				cache[i] = entry;
				return true;
			}
		cache.push_back(entry);
		return true;
	}

	// One of ours. The intermediate statuses are the exception: libbfcp does
	// not retransmit Pending or Accepted, and a late one would say less than
	// the Granted that follows it.
	if (msg.GetPrimitive() == BFCPMessage::FloorRequestStatus) {
		const BFCPAttrFloorRequestInformation* info = ((const BFCPMsgFloorRequestStatus&)msg).GetFloorRequestInformation();
		if (info && info->GetOverallRequestStatus() && info->GetOverallRequestStatus()->GetRequestStatus()) {
			const BFCPAttrRequestStatus::Status status = info->GetOverallRequestStatus()->GetRequestStatus()->GetStatus();
			if (status == BFCPAttrRequestStatus::Pending || status == BFCPAttrRequestStatus::Accepted)
				return true;
		}
	}

	Pending transaction;
	transaction.transactionId	= msg.GetTransactionId();
	transaction.datagram.assign(datagram, datagram + size);
	transaction.intervalMs		= T1InitialMs;
	transaction.firstSentUs		= getTime();
	transaction.dueUs		= transaction.firstSentUs + T1InitialMs * 1000;
	pending.push_back(transaction);

	// The reactor sleeps with no deadline until it knows about this one.
	reactor->Wake();
	return true;
}


bool BFCPUdpEndpoint::SendDatagramLocked(const BYTE* data, size_t size)
{
	const ssize_t n = sendto(fd, data, size, 0, remote, remote.Len());
	if (n == (ssize_t)size)
		return true;

	// A UDP send that fails says nothing about the peer: it is a local
	// condition, and the retransmission will try again.
	::Error("BFCPUdpEndpoint::Send() | sendto failed, errno %d\n", errno);
	return false;
}


void BFCPUdpEndpoint::AckLocked(int transactionId)
{
	for (size_t i=0; i<pending.size(); i++)
		if (pending[i].transactionId == transactionId) {
			pending.erase(pending.begin() + i);
			return;
		}
}


const BFCPUdpEndpoint::Cached* BFCPUdpEndpoint::FindCachedLocked(int transactionId) const
{
	for (size_t i=0; i<cache.size(); i++)
		if (cache[i].transactionId == transactionId)
			return &cache[i];
	return NULL;
}


int BFCPUdpEndpoint::GetPendingCount() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return pending.size();
}


int BFCPUdpEndpoint::GetPollFds(pollfd* fds, int max)
{
	std::lock_guard<std::mutex> lock(mutex);

	if (max < 1 || finished || fd == FD_INVALID)
		return 0;

	fds[0].fd	= fd;
	fds[0].events	= POLLIN;
	fds[0].revents	= 0;
	return 1;
}


int BFCPUdpEndpoint::GetNextTimeoutMs(QWORD nowUs)
{
	std::lock_guard<std::mutex> lock(mutex);

	if (finished || pending.empty())
		return -1;

	QWORD soonestUs = 0;
	for (size_t i=0; i<pending.size(); i++) {
		// Whichever comes first: the next retransmission, or the moment we
		// stop waiting for this peer.
		const QWORD giveUpUs = pending[i].firstSentUs + T1GiveUpMs * 1000;
		const QWORD dueUs = pending[i].dueUs < giveUpUs ? pending[i].dueUs : giveUpUs;
		if (soonestUs == 0 || dueUs < soonestUs)
			soonestUs = dueUs;
	}

	if (soonestUs <= nowUs)
		return 0;
	return (int)((soonestUs - nowUs) / 1000);
}


void BFCPUdpEndpoint::OnPollEvents(const pollfd* fds, int count, QWORD nowUs)
{
	if (count >= 1 && (fds[0].revents & POLLIN))
		OnReadable(nowUs);
}


void BFCPUdpEndpoint::OnReadable(QWORD nowUs)
{
	for (;;) {
		BYTE datagram[MaxDatagramLen];
		IPEndpoint from;

		const ssize_t n = recvfrom(fd, datagram, sizeof(datagram), 0, from.Data(), from.LenPtr());
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
				return;
			::Error("BFCPUdpEndpoint::OnReadable() | recvfrom failed, errno %d\n", errno);
			return;
		}

		{
			std::lock_guard<std::mutex> lock(mutex);

			if (finished)
				return;

			if (! latched) {
				// The first datagram latches the peer, address and port. What
				// it says beats what the SDP claimed: behind a NAT the two
				// differ, and only the one we heard from is reachable.
				remote		= from;
				hasRemote	= true;
				latched		= true;
				::Log("BFCPUdpEndpoint::OnReadable() | user '%d' latched on %s\n", userId, from.ToString().c_str());
			} else if (! (from == remote)) {
				// Somebody else writing to our port. There is no session here
				// to hijack, so it is simply not ours.
				::Error("BFCPUdpEndpoint::OnReadable() | datagram from %s, expected %s, dropped\n", from.ToString().c_str(), remote.ToString().c_str());
				continue;
			}
		}

		Dispatch(datagram, (size_t)n, nowUs);
	}
}


void BFCPUdpEndpoint::Dispatch(const BYTE* data, size_t size, QWORD nowUs)
{
	BFCPMessage* msg = BFCPMessage::Parse(data, size);
	if (! msg) {
		// Unlike TCP, one bad datagram says nothing about the next: there is
		// no stream to lose track of. Drop it and carry on.
		::Error("BFCPUdpEndpoint::Dispatch() | unparsable datagram of %zu octets dropped\n", size);
		return;
	}

	const int transactionId	= msg->GetTransactionId();
	bool replayed		= false;

	{
		std::lock_guard<std::mutex> lock(mutex);

		// Answer in the version we were addressed in. An endpoint served by
		// libbfcp speaks version 1, and would not understand a version 2.
		peerVersion = msg->GetVersion();

		if (msg->GetUserId() != userId) {
			::Error("BFCPUdpEndpoint::Dispatch() | user '%d' received a message for '%d', dropped\n", userId, msg->GetUserId());
			delete msg;
			return;
		}

		// An acknowledgement closes one of our transactions. Anything the peer
		// sends as a responder acknowledges the transaction it answers.
		if (msg->IsResponder())
			AckLocked(transactionId);

		switch (msg->GetPrimitive())
		{
			case BFCPMessage::HelloAck:
			case BFCPMessage::GoodbyeAck:
			case BFCPMessage::FloorRequestStatusAck:
			case BFCPMessage::FloorStatusAck:
			case BFCPMessage::Error:
				AckLocked(transactionId);
				break;
			default:
				break;
		}

		// A request we have already answered. Replaying the same octets is the
		// point: serving it again would, for a FloorRequest, create a second
		// one.
		if (! msg->IsResponder()) {
			const Cached* cached = FindCachedLocked(transactionId);
			if (cached) {
				::Log("BFCPUdpEndpoint::Dispatch() | transaction %d seen again, replaying the answer\n", transactionId);
				SendDatagramLocked(&cached->datagram[0], cached->datagram.size());
				replayed = true;
			}
		}
	}

	if (replayed) {
		delete msg;
		return;
	}

	switch (msg->GetPrimitive())
	{
		case BFCPMessage::HelloAck:
		case BFCPMessage::GoodbyeAck:
		case BFCPMessage::FloorRequestStatusAck:
		case BFCPMessage::FloorStatusAck:
			// Nothing above the transport cares: they close a transaction and
			// say nothing else.
			break;
		default:
			// Outside our lock: the server takes its own, and calls back into
			// Send() on this very endpoint.
			server->MessageReceived(msg, this);
			break;
	}

	delete msg;
}


void BFCPUdpEndpoint::OnPeriodic(QWORD nowUs)
{
	std::vector<std::vector<BYTE> > toResend;
	bool givenUp = false;

	{
		std::lock_guard<std::mutex> lock(mutex);

		if (finished)
			return;

		for (size_t i=0; i<pending.size(); ) {
			// RFC 8855 §6.2. Checked FIRST and on its own clock: tied to the
			// retransmission tick it would never fire, because the interval
			// doubles and the tick after 15.5 s lands at 47.5 s.
			if (nowUs >= pending[i].firstSentUs + T1GiveUpMs * 1000) {
				::Error("BFCPUdpEndpoint::OnPeriodic() | user '%d' never acknowledged transaction %d, giving it up\n", userId, pending[i].transactionId);
				pending.erase(pending.begin() + i);
				givenUp = true;
				continue;
			}

			if (pending[i].dueUs > nowUs) {
				i++;
				continue;
			}

			toResend.push_back(pending[i].datagram);
			pending[i].intervalMs *= 2;
			pending[i].dueUs = nowUs + pending[i].intervalMs * 1000;
			i++;
		}

		for (size_t i=0; i<toResend.size(); i++)
			SendDatagramLocked(&toResend[i][0], toResend[i].size());

		// The answers we keep are only useful for as long as the peer may
		// repeat itself.
		for (size_t i=0; i<cache.size(); ) {
			if (cache[i].expiresUs <= nowUs)
				cache.erase(cache.begin() + i);
			else
				i++;
		}
	}

	// A peer that stopped acknowledging is a peer that left: its floor has to
	// go back, or the sharing stays stuck on somebody who is not there.
	if (givenUp)
		server->TransportClosed(this);
}


void BFCPUdpEndpoint::OnPollError(short revents)
{
	::Error("BFCPUdpEndpoint::OnPollError() | user '%d' socket event 0x%x\n", userId, revents);
	{
		std::lock_guard<std::mutex> lock(mutex);
		Finish();
	}
	server->TransportClosed(this);
}
