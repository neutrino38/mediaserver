/**
 * test_rtmp_hardening.cpp — suite ADVERSE de la couche message RTMP
 * (chantier 5, cf. network_parsers_hardening_plan.md).
 *
 * RTMP arrive en TCP, mais avant toute autorisation applicative : un client qui
 * ouvre la connexion choisit librement le type de message, sa longueur (trois
 * octets, jusqu'à 16 Mo) et la découpe en chunks. Les cas ci-dessous sont ceux
 * qu'un client honnête ne produit jamais — message de longueur nulle, trame
 * média vide, flux de chunks dont l'état n'a pas été ouvert.
 *
 *     ./tests/runtests --gtest_filter='Rtmp*Hardening*:RtmpMessage*:RtmpChunkInput*'
 */
#include <gtest/gtest.h>

#include <vector>

#include "guardedbuffer.h"
#include "rtmpchunk.h"
#include "rtmpmessage.h"

// ---------------------------------------------------------------------------
// Message
// ---------------------------------------------------------------------------

// Les messages AMF3 sautent un premier octet nul — en le lisant AVANT de
// vérifier qu'il existe. Sur un message annoncé de longueur nulle, la longueur
// restante à parser vaut zéro : l'octet lu est hors du tampon, et la longueur
// transmise au parseur AMF (`len - 1`) vaut alors quatre milliards.
TEST(RtmpMessage, UnMessageAmf3DeLongueurNulleNeLitPasSonPremierOctet)
{
	BYTE payload[4] = { 0x00, 0x00, 0x00, 0x00 };
	GuardedBuffer guarded(payload, sizeof(payload));
	ASSERT_TRUE(guarded.IsValid());

	EXPECT_EXIT({
		RTMPMessage msg(1, 0, RTMPMessage::CommandAMF3, (DWORD)0);
		//Le chunk n'apporte rien : `left` vaut zéro.
		msg.Parse(guarded.data() + guarded.Size(), 0);
		_exit(0);
	}, ::testing::ExitedWithCode(0), "");
}

TEST(RtmpMessage, UnMessageDeDonneesAmf3DeLongueurNulleNeLitPasSonPremierOctet)
{
	BYTE payload[4] = { 0x00, 0x00, 0x00, 0x00 };
	GuardedBuffer guarded(payload, sizeof(payload));
	ASSERT_TRUE(guarded.IsValid());

	EXPECT_EXIT({
		RTMPMessage msg(1, 0, RTMPMessage::DataAMF3, (DWORD)0);
		msg.Parse(guarded.data() + guarded.Size(), 0);
		_exit(0);
	}, ::testing::ExitedWithCode(0), "");
}

// Non-régression : un message de commande AMF0 ordinaire reste parsé.
TEST(RtmpMessage, UneCommandeAmf0ResteParsee)
{
	//AMF0 : chaîne "close", nombre 0, null.
	std::vector<BYTE> body;
	body.push_back(0x02); body.push_back(0x00); body.push_back(0x05);
	body.push_back('c'); body.push_back('l'); body.push_back('o');
	body.push_back('s'); body.push_back('e');
	body.push_back(0x00);
	for (int i = 0; i < 8; ++i)
		body.push_back(0x00); //double 0
	body.push_back(0x05);     //null

	RTMPMessage msg(1, 0, RTMPMessage::Command, (DWORD)body.size());
	EXPECT_EQ(body.size(), msg.Parse(body.data(), body.size()));
	ASSERT_TRUE(msg.IsParsed());
	ASSERT_TRUE(msg.IsCommandMessage());
	RTMPCommandMessage* cmd = msg.GetCommandMessage();
	ASSERT_TRUE(cmd != NULL);
	EXPECT_EQ(std::wstring(L"close"), cmd->GetName());
}

// ---------------------------------------------------------------------------
// Trames média
// ---------------------------------------------------------------------------

// Une trame vidéo de taille nulle : le premier octet (codec et type d'image)
// était lu, puis la taille restante décrémentée sous zéro.
TEST(RtmpMediaFrame, UneTrameVideoVideNeLitRien)
{
	BYTE payload[4] = { 0x17, 0x01, 0x00, 0x00 };
	GuardedBuffer guarded(payload, sizeof(payload));
	ASSERT_TRUE(guarded.IsValid());

	EXPECT_EXIT({
		RTMPVideoFrame frame(0, (DWORD)1024);
		frame.Parse(guarded.data() + guarded.Size(), 0);
		_exit(0);
	}, ::testing::ExitedWithCode(0), "");
}

// Non-régression : une trame AVC ordinaire garde son en-tête et sa charge.
TEST(RtmpMediaFrame, UneTrameAvcResteParsee)
{
	BYTE payload[9] = { 0x17, 0x01, 0x00, 0x00, 0x00, 0xAA, 0xBB, 0xCC, 0xDD };

	RTMPVideoFrame frame(0, (DWORD)1024);
	EXPECT_EQ(sizeof(payload), frame.Parse(payload, sizeof(payload)));
	EXPECT_EQ(RTMPVideoFrame::AVC, frame.GetVideoCodec());
	EXPECT_EQ(RTMPVideoFrame::INTRA, frame.GetFrameType());
	EXPECT_EQ(4u, frame.GetMediaSize());
}

// ---------------------------------------------------------------------------
// Flux de chunks entrant
// ---------------------------------------------------------------------------

// Le flux n'a pas encore ouvert de message (StartChunkData) : ni le parsing ni
// la récupération du message ne doivent déréférencer ce qui n'existe pas.
TEST(RtmpChunkInput, UnFluxSansMessageOuvertNeDereferenceRien)
{
	EXPECT_EXIT({
		RTMPChunkInputStream stream;
		BYTE data[8];
		memset(data, 0, sizeof(data));
		stream.Parse(data, sizeof(data));
		stream.GetMessage();
		_exit(0);
	}, ::testing::ExitedWithCode(0), "");
}
