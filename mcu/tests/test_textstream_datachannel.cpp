/**
 * test_textstream_datachannel.cpp — le texte d'une conférence sur data channel.
 *
 * Deux `TextStream` en mode data channel sur la boucle locale, avec de VRAIES
 * sockets UDP, un VRAI handshake DTLS et les VRAIS pipes du mixeur. La chaîne
 * exercée est celle d'un participant de conférence :
 *
 *   pipe du mixeur → SendTextOverDataChannel → T.140 → SCTP → DTLS → UDP
 *                                                                     ↓
 *   pipe du mixeur ←──── onT140Block ←──── T.140 ←── SCTP ←── DTLS ←── UDP
 *
 * Le contraste avec le WebSocket est le point de la phase : `ParticipantTextWS`
 * avait dû se brancher à la couture du mixeur parce qu'un WebSocket n'a PAS de
 * jambe ICE/DTLS/UDP. Un data channel en a une, et `TextStream` en possède déjà
 * exactement une. Il n'y a donc ni classe nouvelle, ni token, ni URL — et pas de
 * thread de réception non plus : les blocs entrants arrivent sur le thread de la
 * session et vont droit au pipe.
 *
 * Conception : docs/conception/T140-DC/SPEC.md §7, §12.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "dtlsfixture.h"
#include "pipetextinput.h"
#include "pipetextoutput.h"
#include "rtpsession.h"
#include "textstream.h"

namespace {

// La session exige un listener non nul.
class StubListener : public RTPSession::Listener
{
public:
	void onFPURequested(RTPSession*) override {}
	void onReceiverEstimatedMaxBitrate(RTPSession*, DWORD) override {}
	void onTempMaxMediaStreamBitrateRequest(RTPSession*, DWORD, DWORD) override {}
};

// Un participant : son flux texte et les deux pipes du mixeur.
class Leg
{
public:
	explicit Leg(bool dtlsClient) : stream(&listener), isClient(dtlsClient) {}

	bool Init()
	{
		toPeer	 = std::make_shared<PipeTextInput>();
		fromPeer = std::make_shared<PipeTextOutput>();

		toPeer->Init();
		fromPeer->Init();

		if (!stream.Init(toPeer,fromPeer))
			return false;

		//Ce que fait ConfigureParticipantMediaConnection avec proto=SCTP.
		return stream.SetTransport(MediaFrame::SCTP) == 1;
	}

	// Le rôle local se déduit de celui du pair : `passive` chez lui -> `active`
	// chez nous. Les deux jambes partagent le certificat du binaire, donc chacune
	// attend l'empreinte de l'autre, qui est la même.
	bool Negotiate(int peerPort)
	{
		if (!stream.SetRemoteCryptoDTLS(isClient ? "passive" : "active",
						"sha-256",
						DTLSTestCertificate::FingerPrint().c_str()))
			return false;

		//La rtpMap ne dit rien de ce transport : aucun payload type ne voyage
		//dans un data channel. Elle est passée vide, et c'est correct.
		RTPMap map;

		if (stream.StartReceiving(map) <= 0)
			return false;

		char ip[] = "127.0.0.1";
		return stream.StartSending(ip,peerPort,map) == 1;
	}

	// Ce que le mixeur a reçu pour ce participant.
	std::wstring Received()
	{
		wchar_t buffer[256];
		const int len = fromPeer->ReadText(buffer,sizeof(buffer)/sizeof(buffer[0]));
		return (len > 0) ? std::wstring(buffer,len) : std::wstring();
	}

	int Port() { return stream.GetLocalPort(); }

	StubListener	listener;
	TextStream	stream;
	bool		isClient;
	std::shared_ptr<PipeTextInput>	toPeer;
	std::shared_ptr<PipeTextOutput>	fromPeer;
};

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

class TextStreamDataChannel : public ::testing::Test
{
protected:
	TextStreamDataChannel() : a(true), b(false) {}

	void SetUp() override
	{
		ASSERT_TRUE(DTLSTestCertificate::Ensure());

		ASSERT_TRUE(a.Init());
		ASSERT_TRUE(b.Init());

		ASSERT_TRUE(a.Negotiate(b.Port()));
		ASSERT_TRUE(b.Negotiate(a.Port()));
	}

	void TearDown() override
	{
		a.stream.End();
		b.stream.End();
	}

	// Le canal est ouvert par A : en production c'est le navigateur qui le crée,
	// et une jambe de conférence n'expose donc pas d'ouverture. On passe ici par
	// les paramètres SCTP, qui disent quand le flux est prêt, puis par le
	// contrôle direct du pont via l'API publique du flux.
	void OpenChannel()
	{
		WORD  port = 0;
		DWORD max  = 0;
		int   streamId = -1;

		//Ce que fait SetupParticipantDataChannel : pose le port du pair et rend
		//les nôtres.
		ASSERT_EQ(1,a.stream.SetupDataChannel(5000,port,max,streamId));
		EXPECT_EQ((WORD)5000,port);
		EXPECT_GT(max,(DWORD)0);
		EXPECT_EQ(-1,streamId) << "le canal ne devrait pas encore etre ouvert";

		ASSERT_TRUE(WaitUntil([&]{ return a.stream.OpenDataChannel(1) == 1; }))
			<< "l'association SCTP n'est pas montee";

		ASSERT_TRUE(WaitUntil([&]{ return a.stream.IsDataChannelOpen() &&
						  b.stream.IsDataChannelOpen(); }))
			<< "le canal t140 ne s'est pas ouvert des deux cotes";
	}

	Leg a;
	Leg b;
};

TEST_F(TextStreamDataChannel, LeCanalSOuvreSansTokenNiURL)
{
	OpenChannel();

	EXPECT_TRUE(a.stream.IsDataChannelOpen());
	EXPECT_TRUE(b.stream.IsDataChannelOpen());
	EXPECT_EQ(MediaFrame::SCTP,a.stream.GetTransport());

	//La jambe garde son port UDP : c'est celui que le `m=application` porte, et
	//c'est StartReceiving qui l'a rendu.
	EXPECT_GT(a.stream.GetLocalPort(),0);
}

// LE test de la phase : du texte écrit dans le pipe du mixeur d'un côté ressort
// dans celui de l'autre, sans aucun détour par la couture du mixeur.
TEST_F(TextStreamDataChannel, LeTexteTraverseLesPipesDuMixeur)
{
	OpenChannel();

	a.toPeer->WriteText(L"bonjour");
	ASSERT_TRUE(WaitUntil([&]{ return b.fromPeer->Length() >= 7; }))
		<< "le texte n'est jamais arrive dans le pipe du mixeur";

	EXPECT_EQ(L"bonjour",b.Received());

	b.toPeer->WriteText(L"bonsoir");
	ASSERT_TRUE(WaitUntil([&]{ return a.fromPeer->Length() >= 7; }));

	EXPECT_EQ(L"bonsoir",a.Received());
}

TEST_F(TextStreamDataChannel, LUTF8MultiOctetsArriveIntact)
{
	OpenChannel();

	const std::wstring text = L"élève";
	a.toPeer->WriteText(text);

	ASSERT_TRUE(WaitUntil([&]{ return b.fromPeer->Length() >= (int) text.size(); }));

	EXPECT_EQ(text,b.Received());
}

// Le texte écrit avant l'ouverture du canal attend et se rejoue : entre le
// 200 OK et l'ouverture il s'écoule un aller-retour SDP, un ICE et un handshake
// DTLS, et la première phrase est celle où l'appelant se présente.
TEST_F(TextStreamDataChannel, LeTexteEmisAvantLOuvertureEstRejoue)
{
	a.toPeer->WriteText(L"bonjour");

	OpenChannel();

	ASSERT_TRUE(WaitUntil([&]{ return b.fromPeer->Length() >= 7; }));

	EXPECT_EQ(L"bonjour",b.Received());
}

// T.140 §5.3 : la perte du canal s'annonce par un U+FFFD du côté qui survit —
// ici la conférence, par le pipe du mixeur.
TEST_F(TextStreamDataChannel, LaPerteDuCanalMetUnUFFFDDansLeMixeur)
{
	OpenChannel();

	a.toPeer->WriteText(L"avant");
	ASSERT_TRUE(WaitUntil([&]{ return b.fromPeer->Length() >= 5; }));
	EXPECT_EQ(L"avant",b.Received());

	//La jambe B s'en va : son canal se ferme et le mixeur l'apprend.
	b.stream.End();

	ASSERT_TRUE(WaitUntil([&]{ return b.fromPeer->Length() >= 1; },1000));

	const std::wstring lost = b.Received();
	ASSERT_EQ((size_t)1,lost.size());
	EXPECT_EQ((wchar_t)0xFFFD,lost[0]);
}

} // namespace
