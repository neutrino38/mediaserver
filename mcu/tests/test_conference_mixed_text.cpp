/**
 * test_conference_mixed_text.cpp — une conférence, DEUX transports texte.
 *
 * La question à laquelle ce test répond : un appelant qui propose son texte sur
 * WebSocket et un appelant qui le propose sur data channel peuvent-ils être
 * admis dans la MÊME conférence, et leur texte se croise-t-il ?
 *
 * Rien ne l'oblige a priori : les deux transports empruntent des chemins très
 * différents dans le serveur. Le WebSocket se branche à la COUTURE DU MIXEUR
 * (`ParticipantTextWS` co-possède les pipes du participant et son demi-plan RTP
 * est arrêté), le data channel reste DANS la jambe (`TextStream` en mode SCTP,
 * qui garde son port, son ICE et son DTLS). Ce qui les réunit est le mixeur
 * texte, où chaque participant est câblé dès `CreateParticipant`.
 *
 * Le test monte donc la vraie chose, sans rien simuler du serveur :
 *
 *   navigateur A ──WebSocket──► /mcu/<conf>/<token> ─┐
 *                                                    ├─► TextMixer ─┐
 *   navigateur B ──DTLS+SCTP──► la jambe texte de B ─┘              │
 *                                                                    │
 *   et le retour, par les deux mêmes chemins ◄────────────────────────┘
 *
 * Le navigateur A est un vrai client WebSocket (poignée de main HTTP Upgrade et
 * trames masquées, comme test_websocket_echo.cpp). Le navigateur B est un
 * `TextStream` en mode data channel, qui mène un vrai handshake DTLS et une
 * vraie association SCTP contre la jambe du participant.
 *
 * Le mixeur préfixe les étiquettes de participant ([nom]) et bat à 200 ms : les
 * assertions cherchent le texte EN SOUS-CHAÎNE, jamais en égalité, et attendent.
 *
 * Conception : docs/conception/T140-DC/SPEC.md §7, et §11 de
 * l'ancien plan texte-sur-WebSocket pour la moitié WS.
 */
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <memory>
#include <string>

#include "../src/jsr309/WSEndpoint.h"
#include "dtlsfixture.h"
#include "mcu.h"
#include "multiconf.h"
#include "pipetextinput.h"
#include "pipetextoutput.h"
#include "rtpsession.h"
#include "textstream.h"
#include "websocketconnection.h"
#include "websocketserver.h"
#include "xmlstreaminghandler.h"

namespace {

typedef std::chrono::steady_clock Clock;

long ElapsedMs(const Clock::time_point& t0)
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
}

// ── le navigateur A : un client WebSocket de bout en bout ────────────────────

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
			//Lecture non bloquante bornée : on interroge la socket en attendant
			//le tick du mixeur.
			timeval tv{0, 200 * 1000};
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

// Trame texte MASQUÉE : obligatoire côté client (RFC 6455 §5.3). Le masque est
// stocké MSB en tête, comme le démasquage du serveur l'attend.
std::string BuildMaskedTextFrame(const std::string& payload, DWORD mask)
{
	WebSocketFrameHeader header(true, WebSocketFrameHeader::TextFrame, payload.size(), mask);
	std::string frame((char*)header.GetData(), header.GetSize());

	BYTE maskBytes[4] = {(BYTE)(mask >> 24), (BYTE)(mask >> 16), (BYTE)(mask >> 8), (BYTE)mask};

	for (size_t i = 0; i < payload.size(); ++i)
		frame.push_back((char)((BYTE)payload[i] ^ maskBytes[i & 3]));

	return frame;
}

// Accumule ce que le serveur envoie sur le WebSocket jusqu'à y trouver
// `expected`, ou l'échéance. Les trames serveur → client ne sont pas masquées.
bool WsReceivedContains(int fd, const std::string& expected, long deadlineMs)
{
	std::string acc;
	Clock::time_point t0 = Clock::now();

	while (ElapsedMs(t0) < deadlineMs)
	{
		BYTE buf[512];
		ssize_t n = read(fd, buf, sizeof(buf));

		if (n <= 0)
			continue;

		//On ne dé-cadre pas : le texte cherché apparaît tel quel dans la charge
		//utile, et un en-tête de 2 octets ne peut pas le fabriquer.
		acc.append((char*)buf, n);

		if (acc.find(expected) != std::string::npos)
			return true;
	}

	return false;
}

// ── le navigateur B : un TextStream en mode data channel ─────────────────────

class StubListener : public RTPSession::Listener
{
public:
	void onFPURequested(RTPSession*) override {}
	void onReceiverEstimatedMaxBitrate(RTPSession*, DWORD) override {}
	void onTempMaxMediaStreamBitrateRequest(RTPSession*, DWORD, DWORD) override {}
};

class DataChannelBrowser
{
public:
	bool Init()
	{
		toServer   = std::make_shared<PipeTextInput>();
		fromServer = std::make_shared<PipeTextOutput>();
		toServer->Init();
		fromServer->Init();

		if (!stream.Init(toServer, fromServer))
			return false;

		return stream.SetTransport(MediaFrame::SCTP) == 1;
	}

	//Nous sommes le CLIENT DTLS : le pair (le serveur média) est passif.
	bool Negotiate(int peerPort)
	{
		if (!stream.SetRemoteCryptoDTLS("passive", "sha-256",
						DTLSTestCertificate::FingerPrint().c_str()))
			return false;

		RTPMap empty;

		if (stream.StartReceiving(empty) <= 0)
			return false;

		char ip[] = "127.0.0.1";
		return stream.StartSending(ip, peerPort, empty) == 1;
	}

	int Port() { return stream.GetLocalPort(); }

	//Accumule ce qui remonte du mixeur jusqu'à y trouver `expected`.
	bool ReceivedContains(const std::wstring& expected, long deadlineMs)
	{
		std::wstring acc;
		Clock::time_point t0 = Clock::now();

		while (ElapsedMs(t0) < deadlineMs)
		{
			wchar_t buffer[256];
			const int len = fromServer->ReadText(buffer, 256);

			if (len > 0)
				acc.append(buffer, len);

			if (acc.find(expected) != std::wstring::npos)
				return true;

			usleep(20 * 1000);
		}

		return false;
	}

	StubListener			listener;
	TextStream			stream{&listener};
	std::shared_ptr<PipeTextInput>	toServer;
	std::shared_ptr<PipeTextOutput>	fromServer;
};

// ── le test ──────────────────────────────────────────────────────────────────

TEST(ConferenceMixedTextTransports, WebSocketAndDataChannelParticipantsExchangeText)
{
	ASSERT_TRUE(DTLSTestCertificate::Ensure());

	//L'adresse que la conférence publie dans l'URL du WebSocket. Sans elle,
	//ConfigureParticipantMediaConnection refuse de basculer quoi que ce soit.
	RTPSession::SetAnnouncedIp("127.0.0.1");

	//--- le serveur WebSocket du MCU, sur son handler /mcu ---------------------
	XmlStreamingHandler handler;
	MCU mcu;
	//0 = balayage désarmé : cette conférence n'a pas de lecteur d'événements.
	mcu.Init(&handler, 0);

	WebSocketServer wsServer;
	wsServer.AddHandler("/mcu", &mcu);

	int wsPort = 0;
	for (int p = 39150; p < 39200; ++p)
	{
		if (wsServer.Init(p))
		{
			wsPort = p;
			break;
		}
	}

	if (wsPort == 0)
	{
		mcu.End();
		GTEST_SKIP() << "Aucun port loopback disponible pour le serveur WebSocket";
	}

	//C'est ce que main() pose au démarrage : le port que l'URL publiera.
	WSEndpoint::SetLocalPort(wsPort);

	//--- la conférence et ses deux participants -------------------------------
	const int confId = mcu.CreateConference(L"mixed-text", 0);
	ASSERT_GT(confId, 0);

	std::shared_ptr<MultiConf> conf;
	ASSERT_TRUE(mcu.GetConferenceRef(confId, conf));
	ASSERT_TRUE(conf->Init(0, 8000));

	const int partWs = conf->CreateParticipant(0, 0, L"alice", Participant::RTP);
	const int partDc = conf->CreateParticipant(0, 0, L"bob", Participant::RTP);
	ASSERT_GT(partWs, 0);
	ASSERT_GT(partDc, 0);

	//--- alice : son texte passe sur un WebSocket -----------------------------
	const std::string token = "11111111-2222-3333-4444-555555555555";
	const std::string base =
		conf->ConfigureParticipantMediaConnection(partWs, MediaFrame::Text, MediaFrame::WS,
							  token);

	ASSERT_FALSE(base.empty()) << "la bascule sur WebSocket a échoué";
	//Le SCHÉMA et le PORT sont ceux du serveur — l'adresse, elle, est celle qu'il
	//annonce, et ce n'est pas au test de la décider.
	EXPECT_EQ(0u, base.find("ws://")) << base;
	EXPECT_NE(base.find(":" + std::to_string(wsPort)), std::string::npos) << base;

	//--- bob : son texte passe sur un data channel ----------------------------
	const std::string dcBase =
		conf->ConfigureParticipantMediaConnection(partDc, MediaFrame::Text, MediaFrame::SCTP,
							  "");

	//Pas d'URL ici, et rien à signer : la réponse est le nom du transport.
	ASSERT_EQ("sctp", dcBase);

	WORD  sctpPort	     = 0;
	DWORD maxMessageSize = 0;
	int   streamId	     = -1;
	ASSERT_TRUE(conf->SetupParticipantDataChannel(partDc, MediaFrame::Text, 5000, sctpPort,
						      maxMessageSize, streamId));
	EXPECT_EQ((WORD)5000, sctpPort);
	EXPECT_GT(maxMessageSize, (DWORD)0);
	EXPECT_EQ(-1, streamId) << "le canal ne devrait pas encore être ouvert";

	//Le plan transport de bob, comme n'importe quelle jambe DTLS : le pair
	//(le navigateur) est ACTIF, donc nous sommes passifs.
	ASSERT_TRUE(conf->SetRemoteCryptoDTLS(partDc, MediaFrame::Text, "active", "sha-256",
					      DTLSTestCertificate::FingerPrint().c_str()));

	RTPMap emptyMap;
	const int dcPort = conf->StartReceiving(partDc, MediaFrame::Text, emptyMap);
	ASSERT_GT(dcPort, 0);

	//--- les deux navigateurs se branchent ------------------------------------
	DataChannelBrowser bob;
	ASSERT_TRUE(bob.Init());
	ASSERT_TRUE(bob.Negotiate(dcPort));

	char loopback[] = "127.0.0.1";
	ASSERT_TRUE(conf->StartSending(partDc, MediaFrame::Text, loopback, bob.Port(), emptyMap));

	//alice ouvre son WebSocket sur l'URL que la conférence a publiée
	int fd = ConnectLoopback(wsPort);
	if (fd < 0)
	{
		conf.reset();
		mcu.End();
		wsServer.End();
		GTEST_SKIP() << "Connexion loopback impossible (sandbox réseau ?)";
	}

	char path[256];
	snprintf(path, sizeof(path), "/mcu/%d/%s", confId, token.c_str());

	char upgrade[1024];
	snprintf(upgrade, sizeof(upgrade),
		 "GET %s HTTP/1.1\r\n"
		 "Host: localhost\r\n"
		 "Upgrade: websocket\r\n"
		 "Connection: Upgrade\r\n"
		 "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
		 "Sec-WebSocket-Version: 13\r\n"
		 "\r\n",
		 path);

	ASSERT_TRUE(WriteAll(fd, (const BYTE*)upgrade, strlen(upgrade)));

	//Le 101 vient d'abord : le token a résolu jusqu'au pont d'alice.
	ASSERT_TRUE(WsReceivedContains(fd, "101", 3000))
		<< "le WebSocket n'a pas été accepté sur /mcu/<conf>/<token>";

	//Le canal t140 de bob : c'est le navigateur qui le crée, sur un flux impair
	//(RFC 8832 §6 — la parité du serveur DTLS est l'autre).
	bool opened = false;
	for (Clock::time_point t0 = Clock::now(); ElapsedMs(t0) < 8000;)
	{
		if (bob.stream.IsDataChannelOpen())
		{
			opened = true;
			break;
		}

		bob.stream.OpenDataChannel(2);
		usleep(20 * 1000);
	}
	ASSERT_TRUE(opened) << "le canal t140 de bob ne s'est pas ouvert";

	//--- alice parle, bob entend ----------------------------------------------
	const std::string hello = "bonjour";
	const std::string frame = BuildMaskedTextFrame(hello, 0x37fa213d);
	ASSERT_TRUE(WriteAll(fd, (const BYTE*)frame.data(), frame.size()));

	EXPECT_TRUE(bob.ReceivedContains(L"bonjour", 5000))
		<< "le texte du WebSocket n'a pas atteint le data channel";

	//--- bob parle, alice entend ----------------------------------------------
	bob.toServer->WriteText(std::wstring(L"bonsoir"));

	EXPECT_TRUE(WsReceivedContains(fd, "bonsoir", 5000))
		<< "le texte du data channel n'a pas atteint le WebSocket";

	//--- démontage ------------------------------------------------------------
	close(fd);
	bob.stream.End();
	conf->StopReceiving(partDc, MediaFrame::Text);
	conf.reset();
	mcu.End();
	wsServer.End();
}

} // namespace
