/**
 * test_websocket_echo.cpp — test d'intégration en-processus du serveur WebSocket.
 *
 * REMPLACE le harnais historique mcu/src/wstest.cpp (+ son client Python
 * test/websocket/ws_client.py) : plutôt que de lancer un serveur d'écho puis un
 * client externe, on démarre ici un WebSocketServer avec TextEchoWebsocketHandler
 * et on joue le rôle du client depuis un thread du même processus, via un socket
 * TCP loopback. On vérifie de bout en bout : poignée de main HTTP Upgrade →
 * 101 Switching Protocols, puis échange d'une trame texte masquée (client →
 * serveur) et réception de son écho (serveur → client). C'est le test de
 * non-régression de la brique WebSocketServer/Connection/Transport (Phases 0-2
 * du refactor websocket-refactor.md).
 *
 * Le test est tolérant à l'environnement : s'il ne parvient ni à écouter ni à se
 * connecter en loopback (sandbox réseau restreinte), il est SKIPPÉ plutôt
 * qu'échoué.
 */
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <cstring>
#include <string>

#include "websocketserver.h"
#include "websocketconnection.h"

namespace {

// Connexion TCP loopback avec quelques tentatives (laisse le thread d'accept démarrer).
int ConnectLoopback(int port)
{
	for (int attempt = 0; attempt < 50; ++attempt)
	{
		int fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd < 0)
			return -1;

		sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		addr.sin_addr.s_addr = inet_addr("127.0.0.1");

		if (connect(fd, (sockaddr*)&addr, sizeof(addr)) == 0)
		{
			//Timeout de lecture : évite de bloquer indéfiniment si le serveur ne répond pas.
			timeval tv{2, 0};
			setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
			return fd;
		}
		close(fd);
		usleep(20 * 1000);
	}
	return -1;
}

bool WriteAll(int fd, const BYTE* data, size_t size)
{
	size_t sent = 0;
	while (sent < size)
	{
		ssize_t n = write(fd, data + sent, size - sent);
		if (n <= 0)
			return false;
		sent += n;
	}
	return true;
}

// Lit jusqu'à trouver "\r\n\r\n" (fin des en-têtes HTTP) ou timeout.
std::string ReadHttpResponse(int fd)
{
	std::string acc;
	BYTE buf[512];
	for (int i = 0; i < 100; ++i)
	{
		ssize_t n = read(fd, buf, sizeof(buf));
		if (n <= 0)
			break;
		acc.append((char*)buf, n);
		if (acc.find("\r\n\r\n") != std::string::npos)
			break;
	}
	return acc;
}

// Lit exactement `size` octets (ou moins si timeout/fermeture).
size_t ReadN(int fd, BYTE* out, size_t size)
{
	size_t got = 0;
	while (got < size)
	{
		ssize_t n = read(fd, out + got, size - got);
		if (n <= 0)
			break;
		got += n;
	}
	return got;
}

// Construit une trame texte MASQUÉE (obligatoire côté client, RFC 6455 §5.3).
// Le masque est stocké MSB en tête (set4), et le serveur démasque avec
// mask[(pos+i)&3] — on applique donc exactement la même convention.
std::string BuildMaskedTextFrame(const std::string& payload, DWORD mask)
{
	WebSocketFrameHeader header(true, WebSocketFrameHeader::TextFrame, payload.size(), mask);
	std::string frame((char*)header.GetData(), header.GetSize());

	BYTE maskBytes[4] = {
		(BYTE)(mask >> 24), (BYTE)(mask >> 16), (BYTE)(mask >> 8), (BYTE)mask
	};
	for (size_t i = 0; i < payload.size(); ++i)
		frame.push_back((char)((BYTE)payload[i] ^ maskBytes[i & 3]));
	return frame;
}

} // namespace

TEST(WebSocketEcho, HandshakeAndTextEcho)
{
	WebSocketServer server;
	auto echo = std::make_shared<TextEchoWebsocketHandler>();
	server.AddHandler("/echo", echo.get());

	//Cherche un port libre dans une plage haute peu susceptible d'être occupée.
	int port = 0;
	for (int p = 39050; p < 39100; ++p)
	{
		if (server.Init(p))
		{
			port = p;
			break;
		}
	}
	if (port == 0)
		GTEST_SKIP() << "Aucun port loopback disponible pour le serveur WebSocket";

	int fd = ConnectLoopback(port);
	if (fd < 0)
	{
		server.End();
		GTEST_SKIP() << "Connexion loopback impossible (sandbox réseau ?)";
	}

	//--- Poignée de main HTTP Upgrade -----------------------------------------
	std::string handshake =
		"GET /echo HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
		"Sec-WebSocket-Version: 13\r\n"
		"\r\n";
	ASSERT_TRUE(WriteAll(fd, (const BYTE*)handshake.data(), handshake.size()));

	std::string response = ReadHttpResponse(fd);
	ASSERT_NE(response.find("101"), std::string::npos)
		<< "Réponse serveur inattendue : " << response;
	EXPECT_NE(response.find("Sec-WebSocket-Accept"), std::string::npos);

	//--- Échange d'une trame texte -------------------------------------------
	const std::string payload = "hello websocket";
	std::string frame = BuildMaskedTextFrame(payload, 0x37fa213d);
	ASSERT_TRUE(WriteAll(fd, (const BYTE*)frame.data(), frame.size()));

	//L'écho serveur → client est une trame texte NON masquée :
	//  octet 0 : 0x81 (FIN + TextFrame) ; octet 1 : longueur (< 126 ici).
	BYTE hdr[2];
	ASSERT_EQ(ReadN(fd, hdr, 2), 2u);
	EXPECT_EQ(hdr[0] & 0x0F, WebSocketFrameHeader::TextFrame);
	EXPECT_TRUE(hdr[0] & 0x80); // FIN
	EXPECT_FALSE(hdr[1] & 0x80); // serveur → client : non masqué
	DWORD len = hdr[1] & 0x7F;
	ASSERT_EQ(len, payload.size());

	BYTE echoed[128];
	ASSERT_EQ(ReadN(fd, echoed, len), (size_t)len);
	EXPECT_EQ(std::string((char*)echoed, len), payload);

	close(fd);
	server.End();
}
