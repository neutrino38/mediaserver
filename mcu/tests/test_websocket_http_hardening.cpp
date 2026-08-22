/**
 * test_websocket_http_hardening.cpp — la poignée de main WebSocket face à un
 * client qui fragmente (chantier 5, cf. network_parsers_hardening_plan.md).
 *
 * Le parseur HTTP embarqué (celui de Node.js/nginx) le dit dans son propre
 * en-tête : « http_data_cb ne rend pas des morceaux complets ; il peut être
 * appelé plusieurs fois pour la même chaîne ». C'est une propriété de TCP, pas
 * une pathologie : rien ne garantit qu'une URL, un nom d'en-tête ou sa valeur
 * arrivent d'un seul tenant — et un client hostile peut la provoquer à volonté
 * en écrivant octet par octet.
 *
 * Or les callbacks REMPLAÇAIENT à chaque appel au lieu d'accumuler : la valeur
 * retenue pour `Sec-WebSocket-Key` était son DERNIER fragment, donc la clé de
 * réponse était fausse ; et `on_url` créait une requête neuve à chaque
 * fragment, abandonnant la précédente.
 *
 * Ce test envoie la MÊME poignée de main que test_websocket_echo.cpp, mais un
 * octet à la fois, et exige la même réponse — dont la valeur de
 * Sec-WebSocket-Accept, qui est celle de l'exemple de la RFC 6455 §1.3 pour la
 * clé « dGhlIHNhbXBsZSBub25jZQ== ». Si le moindre fragment est perdu, cette
 * valeur ne tombe pas juste.
 *
 *     ./tests/runtests --gtest_filter='WebSocketHandshake*'
 */
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <cstring>
#include <string>

#include "websocketconnection.h"
#include "websocketserver.h"

namespace {

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
			timeval tv{2, 0};
			setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
			return fd;
		}
		close(fd);
		usleep(20 * 1000);
	}
	return -1;
}

// Écrit la requête UN OCTET À LA FOIS, avec une pause : chaque octet arrive
// dans son propre read() côté serveur, donc dans son propre callback.
bool WriteByteByByte(int fd, const std::string& data)
{
	for (size_t i = 0; i < data.size(); ++i)
	{
		if (write(fd, data.data() + i, 1) != 1)
			return false;
		usleep(300);
	}
	return true;
}

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

const char* kHandshake =
	"GET /echo HTTP/1.1\r\n"
	"Host: localhost\r\n"
	"Upgrade: websocket\r\n"
	"Connection: Upgrade\r\n"
	"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
	"Sec-WebSocket-Version: 13\r\n"
	"\r\n";

//Valeur de reference de la RFC 6455 §1.3 pour la cle ci-dessus.
const char* kExpectedAccept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";

} // namespace

TEST(WebSocketHandshake, UneRequeteFragmenteeOctetParOctetResteComprise)
{
	WebSocketServer server;
	auto echo = std::make_shared<TextEchoWebsocketHandler>();
	server.AddHandler("/echo", echo.get());

	int port = 0;
	for (int p = 39100; p < 39150; ++p)
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

	ASSERT_TRUE(WriteByteByByte(fd, kHandshake));

	std::string response = ReadHttpResponse(fd);
	EXPECT_NE(response.find("101"), std::string::npos)
		<< "Réponse serveur inattendue : " << response;
	//C'est ici que se voit la troncature : l'accept est calculé sur la clé, donc
	//sur la totalité de ses fragments.
	EXPECT_NE(response.find(kExpectedAccept), std::string::npos)
		<< "Sec-WebSocket-Accept faux (clé reconstituée incomplète ?) : " << response;

	close(fd);
	server.End();
}

// Même requête d'un seul tenant : garde-fou de non-régression du chemin normal,
// et point de comparaison du test précédent.
TEST(WebSocketHandshake, UneRequeteDUnSeulTenantEstComprise)
{
	WebSocketServer server;
	auto echo = std::make_shared<TextEchoWebsocketHandler>();
	server.AddHandler("/echo", echo.get());

	int port = 0;
	for (int p = 39150; p < 39200; ++p)
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

	std::string handshake(kHandshake);
	ASSERT_EQ((ssize_t)handshake.size(),
	          write(fd, handshake.data(), handshake.size()));

	std::string response = ReadHttpResponse(fd);
	EXPECT_NE(response.find("101"), std::string::npos)
		<< "Réponse serveur inattendue : " << response;
	EXPECT_NE(response.find(kExpectedAccept), std::string::npos) << response;

	close(fd);
	server.End();
}
