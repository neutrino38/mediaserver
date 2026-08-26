/**
 * test_dcendpoint.cpp — la jambe texte JSR-309 sur data channel, de bout en bout.
 *
 * Deux `DCEndpoint` sur la boucle locale, avec de VRAIES sockets UDP et un VRAI
 * handshake DTLS. La chaîne complète est exercée :
 *
 *   jambe pontée → onRTPPacket → RED → T.140 → SCTP → DTLS → UDP
 *                                                              ↓
 *   jambe pontée ← Multiplex ← RED ← T.140 ← SCTP ← DTLS ← UDP
 *
 * C'est le seul test du chantier qui prouve que le porteur tient : que les
 * données applicatives DTLS arrivent bien à la pile, que la boucle poll de la
 * session bat la cadence des timers SCTP, que la file de sortie est vidée par le
 * bon thread, et que le texte ressort dans le dialecte que la jambe pontée a
 * négocié — T140 ou T140RED.
 *
 * `OpenDataChannel` n'est pas appelé en production (nous répondons
 * `a=setup:passive`, donc c'est le navigateur qui crée le canal) : c'est ici
 * qu'il est joué, et c'est ce qui l'empêche d'être du code mort qui mente.
 *
 * Conception : docs/conception/T140-DC/SPEC.md §6, §12.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "../src/jsr309/DCEndpoint.h"
#include "dtlsfixture.h"
#include "medkit/codecs.h"

namespace {

// La jambe pontée : ce qu'un RTPEndpoint T.140, un WSEndpoint ou l'enregistreur
// verrait à la place.
class BridgedLeg : public Joinable::Listener
{
public:
	void onRTPPacket(RTPPacket& packet) override
	{
		codecs.push_back(packet.GetCodec());

		if (packet.GetCodec() == TextCodec::T140RED)
		{
			//Le bloc primaire d'un paquet redondant : c'est le texte de
			//maintenant, les autres blocs sont des répétitions.
			RTPRedundantPacket* red = (RTPRedundantPacket*) &packet;
			blocks.push_back(std::string((const char*)red->GetPrimaryPayloadData(),
						     red->GetPrimaryPayloadSize()));
			return;
		}

		blocks.push_back(std::string((const char*)packet.GetMediaData(),
					     packet.GetMediaLength()));
	}

	void onResetStream() override {}
	void onEndStream() override {}

	std::vector<std::string> blocks;
	std::vector<DWORD>	 codecs;
};

// Un endpoint data channel prêt à parler à son jumeau.
class Leg
{
public:
	explicit Leg(bool dtlsClient) : dc(MediaFrame::Text), isClient(dtlsClient) {}

	bool Init()
	{
		//Init ouvre les sockets et démarre la boucle poll de la session.
		if (dc.Init() != 0)
			return false;

		dc.AddListener(&bridged);
		return true;
	}

	// Le rôle local se déduit de celui du pair : `passive` chez lui -> `active`
	// chez nous. Les deux jambes partagent le certificat du binaire, donc chacune
	// attend l'empreinte de l'autre, qui est la même.
	bool Negotiate(int peerPort)
	{
		if (!dc.SetRemoteCryptoDTLS(isClient ? "passive" : "active",
					    "sha-256",
					    DTLSTestCertificate::FingerPrint().c_str()))
			return false;

		char ip[] = "127.0.0.1";
		if (!dc.SetRemotePort(ip,peerPort))
			return false;

		return dc.StartReceiving() == 1;
	}

	int Port() { return dc.GetLocalPort(); }

	DCEndpoint	dc;
	BridgedLeg	bridged;
	bool		isClient;
};

// Émet un paquet T140 vers la jambe, comme le ferait l'autre patte du pont.
void SpeakT140(DCEndpoint& dc,const std::string& text)
{
	RTPPacket packet(MediaFrame::Text,TextCodec::T140);
	packet.SetTimestamp(0);
	packet.SetPayload((BYTE*)text.data(),text.size());
	dc.onRTPPacket(packet);
}

template <typename Predicate>
bool WaitUntil(Predicate done,int timeoutMs = 8000)
{
	for (int elapsed = 0; elapsed < timeoutMs; elapsed += 5)
	{
		if (done())
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return done();
}

class DCEndpointLoopback : public ::testing::Test
{
protected:
	DCEndpointLoopback() : a(true), b(false) {}

	void SetUp() override
	{
		ASSERT_TRUE(DTLSTestCertificate::Ensure());

		ASSERT_TRUE(a.Init());
		ASSERT_TRUE(b.Init());

		ASSERT_TRUE(a.Negotiate(b.Port()));
		ASSERT_TRUE(b.Negotiate(a.Port()));

		//Le handshake DTLS puis l'association SCTP : la boucle poll de chaque
		//session les mène, personne n'a rien à pomper ici.
		ASSERT_TRUE(WaitUntil([&]{ return a.dc.IsDTLSHandshakeCompleted() &&
						  b.dc.IsDTLSHandshakeCompleted(); }))
			<< "le handshake DTLS n'a pas abouti";
	}

	void TearDown() override
	{
		a.dc.End();
		b.dc.End();
	}

	// Canal ouvert par A. Flux IMPAIR : la parité du serveur DTLS (RFC 8832 §6).
	void OpenChannel()
	{
		ASSERT_TRUE(WaitUntil([&]{ return a.dc.GetStreamId() >= 0 ||
						  a.dc.OpenDataChannel(1) == 1; }))
			<< "l'association SCTP n'est pas montee";

		ASSERT_TRUE(WaitUntil([&]{ return a.dc.GetStreamId() == 1 &&
						  b.dc.GetStreamId() == 1; }))
			<< "le canal t140 ne s'est pas ouvert des deux cotes";
	}

	Leg a;
	Leg b;
};

TEST_F(DCEndpointLoopback, LeCanalSOuvreSurUneJambeICEDTLSUDP)
{
	OpenChannel();

	EXPECT_EQ(1,a.dc.GetStreamId());
	EXPECT_EQ(1,b.dc.GetStreamId());

	//La jambe a bien un port UDP annoncé, comme n'importe quelle jambe RTP :
	//c'est celui que le `m=application` porte.
	EXPECT_GT(a.dc.GetLocalMediaPort(),0);
	EXPECT_EQ((WORD)5000,a.dc.GetLocalSCTPPort());
	EXPECT_EQ(SCTPTransport::MaxMessageSize,a.dc.GetMaxMessageSize());
}

// LE test du chantier : du texte entre par la jambe pontée d'un côté et ressort
// par celle de l'autre, en ayant traversé RED, T.140, SCTP, DTLS et UDP.
TEST_F(DCEndpointLoopback, LeTexteTraverseDeBoutEnBout)
{
	OpenChannel();

	SpeakT140(a.dc,"bonjour");
	ASSERT_TRUE(WaitUntil([&]{ return !b.bridged.blocks.empty(); }))
		<< "le texte n'est jamais arrive sur la jambe pontee";

	EXPECT_EQ("bonjour",b.bridged.blocks[0]);
	EXPECT_EQ((DWORD)TextCodec::T140,b.bridged.codecs[0]);

	SpeakT140(b.dc,"bonsoir");
	ASSERT_TRUE(WaitUntil([&]{ return !a.bridged.blocks.empty(); }));

	EXPECT_EQ("bonsoir",a.bridged.blocks[0]);
}

// La redondance de RFC 4103 n'a aucun sens SUR le canal — SCTP est fiable — mais
// elle en a sur la patte RTP d'en face, et c'est nous qui la produisons.
TEST_F(DCEndpointLoopback, LaRedondanceEstProduitePourLaJambePonteeQuiLaDemande)
{
	//Ce que ferait Endpoint::StartReceiving en lisant une rtpMap portant T140RED.
	b.dc.SetUseRed(true);
	b.dc.SetPrimaryPayloadType(TextCodec::T140);

	OpenChannel();

	SpeakT140(a.dc,"redondant");
	ASSERT_TRUE(WaitUntil([&]{ return !b.bridged.blocks.empty(); }));

	EXPECT_EQ("redondant",b.bridged.blocks[0]);
	EXPECT_EQ((DWORD)TextCodec::T140RED,b.bridged.codecs[0]);
}

TEST_F(DCEndpointLoopback, LUTF8MultiOctetsArriveIntact)
{
	OpenChannel();

	const std::string text = "élève — 日本語";
	SpeakT140(a.dc,text);

	ASSERT_TRUE(WaitUntil([&]{ return !b.bridged.blocks.empty(); }));

	EXPECT_EQ(text,b.bridged.blocks[0]);
	EXPECT_EQ((size_t)1,b.bridged.blocks.size()) << "le bloc a ete decoupe";
}

// Le texte émis avant l'ouverture du canal attend et se rejoue : entre le 200 OK
// et l'ouverture il s'écoule un aller-retour SDP, un ICE et un handshake DTLS, et
// la première phrase est celle où l'appelant se présente.
TEST_F(DCEndpointLoopback, LeTexteEmisAvantLOuvertureEstRejoue)
{
	SpeakT140(a.dc,"bonjour ");
	SpeakT140(a.dc,"je suis Alice");

	OpenChannel();

	ASSERT_TRUE(WaitUntil([&]{ return b.bridged.blocks.size() >= 2; }));

	EXPECT_EQ("bonjour ",b.bridged.blocks[0]);
	EXPECT_EQ("je suis Alice",b.bridged.blocks[1]);
}

// T.140 §5.3 : la perte du canal s'annonce par un U+FFFD du côté qui survit —
// ici la jambe pontée. C'est la seule trace qu'un utilisateur ait qu'il manque
// du texte.
TEST_F(DCEndpointLoopback, LaPerteDuCanalMetUnUFFFDSurLaJambePontee)
{
	OpenChannel();

	SpeakT140(a.dc,"avant");
	ASSERT_TRUE(WaitUntil([&]{ return !b.bridged.blocks.empty(); }));

	const size_t before = b.bridged.blocks.size();

	//La jambe B s'en va : son propre canal se ferme et sa jambe pontée l'apprend.
	b.dc.End();

	ASSERT_GT(b.bridged.blocks.size(),before) << "aucun U+FFFD emis";

	const std::string& last = b.bridged.blocks.back();
	ASSERT_EQ((size_t)3,last.size());
	EXPECT_EQ("\xEF\xBF\xBD",last);
}

} // namespace
