#include "bfcp/BFCPTcpTransport.h"
#include "bfcp/BFCPFloorControlServer.h"
#include "rtpsessionset.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>


/* BFCPTcpConnection */

BFCPTcpConnection::BFCPTcpConnection(int fd, BFCPFloorControlServer* server, const IPEndpoint& peer) :
	fd(fd),
	server(server),
	peer(peer),
	finished(false)
{
}


BFCPTcpConnection::~BFCPTcpConnection()
{
	if (fd != FD_INVALID)
		close(fd);
}


bool BFCPTcpConnection::IsFinished() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return finished;
}


void BFCPTcpConnection::Finish()
{
	// The socket is NOT closed here: the reactor may hold it in the pollfd
	// array of the turn under way. The listener closes it when it reaps, on
	// the reactor thread, once nothing runs inside this object any more.
	finished = true;

	// That reap happens in the listener's OnPeriodic, so it needs a turn. The
	// listener may already have run this one, and nothing else is going to
	// wake a reactor whose handlers all ask for an infinite timeout: without
	// this, a closed connection is never reaped and its socket never closed.
	RtpSessionSet::Default().Wake();
}


bool BFCPTcpConnection::Send(const BFCPMessage& msg)
{
	BYTE buffer[BFCPMessage::HeaderLen + 4096];
	const size_t size = msg.Serialize(buffer, sizeof(buffer));

	if (size == BFCPMessage::Failed)
		return ::Error("BFCPTcpConnection::Send() | cannot serialize a %s\n", BFCPMessage::PrimitiveName(msg.GetPrimitive()));

	std::lock_guard<std::mutex> lock(mutex);
	if (finished)
		return false;

	SendLocked(buffer, size);
	return true;
}


void BFCPTcpConnection::SendLocked(const BYTE* data, size_t size)
{
	size_t sent = 0;

	// Nothing queued: try the socket first, it is almost always ready and that
	// spares a reactor turn.
	if (out.empty()) {
		while (sent < size) {
			const ssize_t n = send(fd, data + sent, size - sent, MSG_NOSIGNAL);
			if (n > 0) {
				sent += n;
				continue;
			}
			if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
				break;
			if (n < 0 && errno == EINTR)
				continue;
			::Error("BFCPTcpConnection::Send() | send failed, errno %d\n", errno);
			Finish();
			return;
		}
	}

	if (sent == size)
		return;

	// The rest waits for POLLOUT. Waking the reactor matters: it may be asleep
	// in a poll that does not yet watch this socket for writing.
	out.insert(out.end(), data + sent, data + size);
	RtpSessionSet::Default().Wake();
}


void BFCPTcpConnection::Close()
{
	std::lock_guard<std::mutex> lock(mutex);
	if (finished)
		return;
	Finish();
}


int BFCPTcpConnection::GetPollFds(pollfd* fds, int max)
{
	std::lock_guard<std::mutex> lock(mutex);

	// A finished connection declares no descriptor: the reactor stops polling
	// it, which is what makes closing the socket safe when we are reaped.
	if (max < 1 || finished || fd == FD_INVALID)
		return 0;

	fds[0].fd	= fd;
	fds[0].events	= POLLIN | (out.empty() ? 0 : POLLOUT);
	fds[0].revents	= 0;
	return 1;
}


int BFCPTcpConnection::GetNextTimeoutMs(QWORD nowUs)
{
	// TCP is reliable: nothing to retransmit, nothing to time out here.
	return -1;
}


void BFCPTcpConnection::OnPollEvents(const pollfd* fds, int count, QWORD nowUs)
{
	if (count < 1)
		return;

	if (fds[0].revents & POLLOUT)
		OnWritable();

	if (fds[0].revents & POLLIN)
		OnReadable();
}


void BFCPTcpConnection::OnWritable()
{
	std::lock_guard<std::mutex> lock(mutex);

	while (! out.empty() && ! finished) {
		const ssize_t n = send(fd, &out[0], out.size(), MSG_NOSIGNAL);
		if (n > 0) {
			out.erase(out.begin(), out.begin() + n);
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return;
		if (n < 0 && errno == EINTR)
			continue;
		::Error("BFCPTcpConnection::OnWritable() | send failed, errno %d\n", errno);
		Finish();
		return;
	}
}


void BFCPTcpConnection::OnReadable()
{
	BYTE chunk[4096];

	for (;;) {
		const ssize_t n = recv(fd, chunk, sizeof(chunk), 0);

		if (n == 0) {
			// The peer closed. Its floor goes with it.
			::Log("BFCPTcpConnection::OnReadable() | peer closed the connection\n");
			{
				std::lock_guard<std::mutex> lock(mutex);
				Finish();
			}
			server->TransportClosed(this);
			return;
		}

		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			if (errno == EINTR)
				continue;
			::Error("BFCPTcpConnection::OnReadable() | recv failed, errno %d\n", errno);
			{
				std::lock_guard<std::mutex> lock(mutex);
				Finish();
			}
			server->TransportClosed(this);
			return;
		}

		{
			std::lock_guard<std::mutex> lock(mutex);
			in.insert(in.end(), chunk, chunk + n);
		}
	}

	// A single read can hold the tail of one message and the head of the next,
	// or several whole ones. Every message in the buffer is handed up, not just
	// the first: dropping the rest is exactly the defect libbfcp has.
	for (;;) {
		size_t total = 0;
		std::vector<BYTE> message;

		{
			std::lock_guard<std::mutex> lock(mutex);

			if (finished || in.size() < BFCPMessage::HeaderLen)
				return;

			// The header says how much follows, in 4-octet units.
			total = BFCPMessage::HeaderLen + get2(&in[0], 2) * 4;

			if (total > MaxMessageLen) {
				::Error("BFCPTcpConnection::OnReadable() | a message of %zu octets is over the %zu we accept\n", total, MaxMessageLen);
				Finish();
				server->TransportClosed(this);
				return;
			}

			// Not all here yet: wait for the rest, keeping what we hold.
			if (in.size() < total)
				return;

			message.assign(in.begin(), in.begin() + total);
			in.erase(in.begin(), in.begin() + total);
		}

		// Parsed and dispatched OUTSIDE our lock: the server takes its own, and
		// may call back into Send() on this very connection.
		BFCPMessage* msg = BFCPMessage::Parse(&message[0], total);
		if (! msg) {
			// RFC 4582 §8: a message we cannot parse over a reliable transport
			// leaves the stream unusable — we cannot know where the next one
			// starts, since we just proved we do not trust the length we read.
			::Error("BFCPTcpConnection::OnReadable() | unparsable message, closing the connection\n");
			{
				std::lock_guard<std::mutex> lock(mutex);
				Finish();
			}
			server->TransportClosed(this);
			return;
		}

		server->MessageReceived(msg, this);
		delete msg;
	}
}


void BFCPTcpConnection::OnPeriodic(QWORD nowUs)
{
}


void BFCPTcpConnection::OnPollError(short revents)
{
	::Log("BFCPTcpConnection::OnPollError() | socket event 0x%x\n", revents);
	{
		std::lock_guard<std::mutex> lock(mutex);
		Finish();
	}
	server->TransportClosed(this);
}


/* BFCPTcpListener */

BFCPTcpListener::BFCPTcpListener(BFCPFloorControlServer* server, RtpSessionSet* reactor) :
	fd(FD_INVALID),
	port(0),
	server(server),
	reactor(reactor ? reactor : &RtpSessionSet::Default())
{
}


BFCPTcpListener::~BFCPTcpListener()
{
	Close();
}


int BFCPTcpListener::Open(WORD port)
{
	if (fd != FD_INVALID)
		return ::Error("BFCPTcpListener::Open() | already open on port %d\n", this->port);

	// One AF_INET6 socket for both families, like every other listen in the
	// mcu: an IPv4 client arrives as ::ffff:a.b.c.d. libbfcp passed "0.0.0.0"
	// and was therefore reachable over IPv4 only.
	fd = socket(AF_INET6, SOCK_STREAM, 0);
	if (fd < 0) {
		fd = FD_INVALID;
		return ::Error("BFCPTcpListener::Open() | cannot create socket, errno %d\n", errno);
	}

	int optval = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

	int v6only = 0;
	if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)) < 0)
		::Error("BFCPTcpListener::Open() | cannot clear IPV6_V6ONLY (errno %d) — IPv4 clients will not be served\n", errno);

	const IPEndpoint listenOn = IPAddress::Any(AF_INET6).To(port);
	if (bind(fd, listenOn, listenOn.Len()) < 0) {
		::Error("BFCPTcpListener::Open() | cannot bind port %d, errno %d\n", port, errno);
		close(fd);
		fd = FD_INVALID;
		return 0;
	}

	if (listen(fd, 8) < 0) {
		::Error("BFCPTcpListener::Open() | cannot listen, errno %d\n", errno);
		close(fd);
		fd = FD_INVALID;
		return 0;
	}

	// Non blocking: accept() runs on the reactor thread, and a blocking one
	// would stop every other BFCP conference of the process.
	const int flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	// The port the kernel actually gave us is the one to announce in the SDP.
	// Asking a probe socket first, as the mcu did, is a race with whoever binds
	// in between.
	IPEndpoint bound;
	if (getsockname(fd, bound.Data(), bound.LenPtr()) == 0)
		this->port = IPAddress::PortOf(bound.Data());
	else
		this->port = port;

	reactor->Add(this);

	::Log("BFCPTcpListener::Open() | listening on port %d, both families\n", this->port);
	return this->port;
}


void BFCPTcpListener::Close()
{
	if (fd == FD_INVALID)
		return;

	// Synchronous when called from another thread: once it returns, the
	// reactor no longer holds our descriptor, so closing it is safe.
	reactor->Remove(this);

	std::vector<std::unique_ptr<BFCPTcpConnection> > doomed;
	{
		std::lock_guard<std::mutex> lock(mutex);
		doomed.swap(connections);
	}

	for (size_t i=0; i<doomed.size(); i++) {
		// The server still holds this transport on one of its users. Detaching
		// it before the object dies is what keeps that pointer from dangling.
		server->TransportClosed(doomed[i].get());
		reactor->Remove(doomed[i].get());
	}
	doomed.clear();

	close(fd);
	fd = FD_INVALID;
	port = 0;
}


int BFCPTcpListener::GetConnectionCount()
{
	std::lock_guard<std::mutex> lock(mutex);
	return connections.size();
}


int BFCPTcpListener::GetPollFds(pollfd* fds, int max)
{
	if (max < 1 || fd == FD_INVALID)
		return 0;

	fds[0].fd	= fd;
	fds[0].events	= POLLIN;
	fds[0].revents	= 0;
	return 1;
}


int BFCPTcpListener::GetNextTimeoutMs(QWORD nowUs)
{
	return -1;
}


void BFCPTcpListener::OnPollEvents(const pollfd* fds, int count, QWORD nowUs)
{
	if (count >= 1 && (fds[0].revents & POLLIN))
		Accept();
}


void BFCPTcpListener::Accept()
{
	// Drain the backlog: the socket is non blocking, so we stop on EAGAIN.
	for (;;) {
		IPEndpoint peer;
		const int client = accept(fd, peer.Data(), peer.LenPtr());

		if (client < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
				return;
			::Error("BFCPTcpListener::Accept() | accept failed, errno %d\n", errno);
			return;
		}

		const int flags = fcntl(client, F_GETFL, 0);
		fcntl(client, F_SETFL, flags | O_NONBLOCK);

		BFCPTcpConnection* connection = new BFCPTcpConnection(client, server, peer);
		{
			std::lock_guard<std::mutex> lock(mutex);
			connections.push_back(std::unique_ptr<BFCPTcpConnection>(connection));
		}

		// Adding from the reactor thread is allowed: the handler joins the next
		// turn. The connection is NOT tied to a user yet — the first message it
		// carries says who it is.
		reactor->Add(connection);

		::Log("BFCPTcpListener::Accept() | connection from %s\n", peer.ToString().c_str());
	}
}


void BFCPTcpListener::OnPeriodic(QWORD nowUs)
{
	Reap();
}


// Destroys the connections that are done. Runs on the reactor thread, which is
// the point: a connection that finished during this turn has already returned
// from its own callback, so nothing is running inside what we delete.
void BFCPTcpListener::Reap()
{
	std::vector<std::unique_ptr<BFCPTcpConnection> > doomed;

	{
		std::lock_guard<std::mutex> lock(mutex);

		for (size_t i=0; i<connections.size(); ) {
			if (! connections[i]->IsFinished()) {
				i++;
				continue;
			}
			doomed.push_back(std::move(connections[i]));
			connections.erase(connections.begin() + i);
		}
	}

	// Two things must happen before the destructor runs. The server has to let
	// go of the transport, or a user keeps a pointer to freed memory and the
	// next Goodbye writes into it. And the reactor has to let go of the
	// descriptor, or it polls one the destructor is about to close.
	for (size_t i=0; i<doomed.size(); i++) {
		server->TransportClosed(doomed[i].get());
		reactor->Remove(doomed[i].get());
	}
}


void BFCPTcpListener::OnPollError(short revents)
{
	::Error("BFCPTcpListener::OnPollError() | listening socket event 0x%x\n", revents);
}
