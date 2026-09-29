/**
 * test_ws_client_seams.cpp — les coutures du mode client WebSocket (lot 0 de
 * docs/conception/WS-CLIENT/SPEC.md).
 *
 * Le mode client n'existe pas encore ; ces quatre briques, elles, sont posées.
 * Sans test, rien ne dirait qu'elles tiennent le contrat que le lot 2
 * exploitera :
 *
 *   - `HTTPParser::GetStatusCode` : un client doit savoir si on lui a répondu
 *     101 ou 404 ;
 *   - le parseur REND LA MAIN sur le reliquat : un serveur peut coller sa
 *     première trame T.140 au 101 dans le même segment TCP. Le compte rendu
 *     d'octets consommés est le seul moyen de ne pas la perdre (piège 1 du
 *     SPEC) ;
 *   - `HTTPRequest::Serialize` : la requête d'upgrade, qui n'avait pas de
 *     sérialisation (seule `HTTPResponse` en avait une) ;
 *   - `WebSocketTransport::IsReady` : `Send` JETTE les octets applicatifs tant
 *     qu'un handshake TLS est en cours (piège 2 du SPEC).
 *
 * La factorisation du calcul de `Sec-WebSocket-Accept` n'est pas reprise ici :
 * test_websocket_http_hardening.cpp l'exerce déjà de bout en bout, sur le
 * vecteur de la RFC 6455 §1.3.
 *
 *     ./tests/runtests --gtest_filter='WsClientSeams*'
 */
#include <gtest/gtest.h>

#include <sys/socket.h>
#include <unistd.h>
#include <string>

#include "http.h"
#include "httpparser.h"
#include "websockettransport.h"

namespace {

//Listener minimal : le parseur en exige huit, aucun ne nous intéresse.
class SilentListener : public HTTPParser::Listener
{
public:
	virtual int on_url (HTTPParser*, const char*, DWORD)		{ return 0; }
	virtual int on_header_field (HTTPParser*, const char*, DWORD)	{ return 0; }
	virtual int on_header_value (HTTPParser*, const char*, DWORD)	{ return 0; }
	virtual int on_body (HTTPParser*, const char*, DWORD)		{ return 0; }
	virtual int on_message_begin (HTTPParser*)			{ return 0; }
	virtual int on_status_complete (HTTPParser*)			{ return 0; }
	virtual int on_headers_complete (HTTPParser*)			{ return 0; }
	virtual int on_message_complete (HTTPParser*)			{ return 0; }
};

const char* const Response101 =
	"HTTP/1.1 101 Switching Protocols\r\n"
	"Upgrade: websocket\r\n"
	"Connection: Upgrade\r\n"
	"Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
	"\r\n";

} // namespace

TEST(WsClientSeams, LeCodeDeStatutEstLisibleApresLaReponse)
{
	SilentListener listener;
	HTTPParser parser;
	parser.Init(&listener, HTTPParser::HTTP_RESPONSE);

	parser.Execute(Response101, strlen(Response101));

	EXPECT_EQ(101, parser.GetStatusCode());
	EXPECT_EQ(1, parser.GetUpgrade());
}

TEST(WsClientSeams, UneReponseDErreurRendSonCodeEtNonUnUpgrade)
{
	const char* const response404 =
		"HTTP/1.1 404 Not Found\r\n"
		"Content-Length: 0\r\n"
		"\r\n";

	SilentListener listener;
	HTTPParser parser;
	parser.Init(&listener, HTTPParser::HTTP_RESPONSE);

	parser.Execute(response404, strlen(response404));

	EXPECT_EQ(404, parser.GetStatusCode());
	EXPECT_EQ(0, parser.GetUpgrade());
}

//Le piège 1 : la première trame WebSocket collée au 101 dans le même segment.
//Le parseur s'arrête à la fin des en-têtes et dit combien il a consommé ; le
//reste appartient au parseur de trames. Un client qui ignore ce compte perd la
//première phrase de son correspondant.
TEST(WsClientSeams, LeParseurSArreteAuBoutDesEnTetesEtLaisseLeReliquat)
{
	//Une trame texte non masquée de 3 octets, comme un serveur en émet
	const BYTE frame[] = { 0x81, 0x03, 'h', 'i', '!' };

	std::string stream(Response101);
	stream.append((const char*)frame, sizeof(frame));

	SilentListener listener;
	HTTPParser parser;
	parser.Init(&listener, HTTPParser::HTTP_RESPONSE);

	DWORD consumed = parser.Execute(stream.data(), stream.size());

	ASSERT_EQ(strlen(Response101), consumed);
	EXPECT_EQ(sizeof(frame), stream.size()-consumed);
}

TEST(WsClientSeams, LaRequeteDUpgradeSeSerialise)
{
	HTTPRequest request("GET", "/jsr309/1234/token", 1, 1);
	request.AddHeader("Host", "mediaserver.example:9090");
	request.AddHeader("Upgrade", "websocket");
	request.AddHeader("Connection", "Upgrade");
	request.AddHeader("Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==");
	request.AddHeader("Sec-WebSocket-Version", "13");

	const std::string serialized = request.Serialize();

	EXPECT_EQ(0u, serialized.find("GET /jsr309/1234/token HTTP/1.1\r\n"));
	EXPECT_NE(std::string::npos, serialized.find("\r\nHost: mediaserver.example:9090\r\n"));
	EXPECT_NE(std::string::npos, serialized.find("\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"));
	//Les en-têtes se terminent par une ligne vide, et rien ne suit
	EXPECT_EQ(serialized.size()-4, serialized.rfind("\r\n\r\n"));
}

TEST(WsClientSeams, LeTransportClairEstPretDesQuIlTientUnSocket)
{
	int fds[2];
	ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));

	WebSocketPlainTransport transport;

	//Sans socket, rien n'est envoyable
	EXPECT_FALSE(transport.IsReady());

	ASSERT_GT(transport.Init(fds[0]), 0);
	EXPECT_TRUE(transport.IsReady());

	transport.Shutdown();
	EXPECT_FALSE(transport.IsReady());

	close(fds[1]);
}
