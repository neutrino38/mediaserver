/**
 * speech_test_util.h — raw HTTP and WebSocket clients for the speech-server
 * tests. Raw sockets on purpose: the tests must see the exact status line and
 * the exact close code.
 */
#ifndef SPEECH_TEST_UTIL_H
#define SPEECH_TEST_UTIL_H

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include "config.h"
#include "ipaddress.h"
#include "websocketconnection.h"

namespace speechtest
{

// A port the kernel just handed out, then released.
inline int FreePort(int family)
{
	int fd = socket(family, SOCK_STREAM, 0);
	const IPEndpoint any = IPAddress::Any(family).To(0);
	if (fd < 0 || bind(fd, any, any.Len()) != 0)
	{
		if (fd >= 0)
			close(fd);
		return 0;
	}
	IPEndpoint bound;
	getsockname(fd, bound.Data(), bound.LenPtr());
	close(fd);
	return bound.Port();
}

inline int Connect(const std::string& address, int port)
{
	const IPEndpoint to = IPAddress::Parse(address).To(port);
	for (int attempt = 0; attempt < 100; ++attempt)
	{
		int fd = socket(to.Sockaddr()->sa_family, SOCK_STREAM, 0);
		if (fd < 0)
			return -1;
		if (connect(fd, to, to.Len()) == 0)
		{
			timeval tv{3, 0};
			setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
			return fd;
		}
		close(fd);
		usleep(20*1000);
	}
	return -1;
}

inline bool WriteAll(int fd, const std::string& data)
{
	size_t sent = 0;
	while (sent < data.size())
	{
		ssize_t n = write(fd, data.data() + sent, data.size() - sent);
		if (n <= 0)
			return false;
		sent += n;
	}
	return true;
}

struct HttpResponse
{
	int		status = 0;
	std::string	body;
};

// One request, Connection: close, the whole response read.
inline HttpResponse Http(const std::string& address, int port, const std::string& method,
			 const std::string& path, const std::string& contentType = "", const std::string& body = "")
{
	HttpResponse response;
	int fd = Connect(address, port);
	if (fd < 0)
		return response;

	std::string request = method + " " + path + " HTTP/1.1\r\nHost: test\r\nConnection: close\r\n";
	if (!contentType.empty())
		request += "Content-Type: " + contentType + "\r\n";
	if (!body.empty() || method == "POST")
		request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
	request += "\r\n" + body;
	WriteAll(fd, request);

	std::string raw;
	char buf[4096];
	ssize_t n;
	while ((n = read(fd, buf, sizeof(buf))) > 0)
		raw.append(buf, n);
	close(fd);

	if (raw.compare(0, 9, "HTTP/1.1 ") == 0 || raw.compare(0, 9, "HTTP/1.0 ") == 0)
		response.status = atoi(raw.c_str() + 9);
	const size_t end = raw.find("\r\n\r\n");
	if (end != std::string::npos)
		response.body = raw.substr(end + 4);
	return response;
}

// A WebSocket client frame by frame.
class WsClient
{
public:
	struct Frame
	{
		int		opcode = -1;	// -1: nothing read
		std::string	payload;

		int CloseCode() const
		{
			return payload.size() >= 2 ? ((BYTE)payload[0] << 8) | (BYTE)payload[1] : -1;
		}
	};

	~WsClient()	{ if (fd >= 0) close(fd); }

	// Status of the upgrade response: 101 when the stream opened.
	int Open(const std::string& address, int port, const std::string& path)
	{
		fd = Connect(address, port);
		if (fd < 0)
			return 0;
		WriteAll(fd, "GET " + path + " HTTP/1.1\r\nHost: test\r\nUpgrade: websocket\r\n"
			     "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
			     "Sec-WebSocket-Version: 13\r\n\r\n");
		std::string head;
		char c;
		while (head.find("\r\n\r\n") == std::string::npos && read(fd, &c, 1) == 1)
			head.push_back(c);
		return head.size() > 12 ? atoi(head.c_str() + 9) : 0;
	}

	void SetReadTimeout(int secs)
	{
		timeval tv{secs, 0};
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	}

	void SendText(const std::string& text)	{ Send(WebSocketFrameHeader::TextFrame, text); }
	void SendBinary(const std::string& data)	{ Send(WebSocketFrameHeader::BinaryFrame, data); }

	Frame Read()
	{
		Frame frame;
		BYTE head[2];
		if (!ReadN(head, 2))
			return frame;
		uint64_t length = head[1] & 0x7F;
		if (length == 126)
		{
			BYTE ext[2];
			if (!ReadN(ext, 2))
				return frame;
			length = (ext[0] << 8) | ext[1];
		}
		else if (length == 127)
		{
			BYTE ext[8];
			if (!ReadN(ext, 8))
				return frame;
			length = 0;
			for (int i = 0; i < 8; ++i)
				length = (length << 8) | ext[i];
		}
		frame.payload.resize(length);
		if (length && !ReadN((BYTE*)&frame.payload[0], length))
			return frame;
		frame.opcode = head[0] & 0x0F;
		return frame;
	}

	// Next text frame, skipping nothing: returns an empty frame on anything else.
	std::string ReadText()
	{
		const Frame frame = Read();
		return frame.opcode == WebSocketFrameHeader::TextFrame ? frame.payload : "";
	}

private:
	void Send(WebSocketFrameHeader::OpCode opcode, const std::string& payload)
	{
		const DWORD mask = 0x11223344;
		WebSocketFrameHeader header(true, opcode, payload.size(), mask);
		std::string frame((const char*)header.GetData(), header.GetSize());
		const BYTE key[4] = {0x11, 0x22, 0x33, 0x44};
		for (size_t i = 0; i < payload.size(); ++i)
			frame.push_back((char)((BYTE)payload[i] ^ key[i & 3]));
		WriteAll(fd, frame);
	}

	bool ReadN(BYTE* out, size_t size)
	{
		size_t got = 0;
		while (got < size)
		{
			ssize_t n = read(fd, out + got, size - got);
			if (n <= 0)
				return false;
			got += n;
		}
		return true;
	}

	int fd = -1;
};

}

#endif
