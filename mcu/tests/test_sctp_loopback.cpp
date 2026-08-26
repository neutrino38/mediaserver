/**
 * test_sctp_loopback.cpp — la pile T.140 sur data channel, de bout en bout.
 *
 * Deux `SCTPTransport` dos à dos dans le même processus : la file de sortie de
 * l'un est injectée dans le `OnPacket` de l'autre. Pas de socket, pas de DTLS,
 * pas de réseau — et pourtant l'association, DCEP et le T.140 sont exercés
 * ensemble, en quelques millisecondes. C'est le test qui vaut le plus du
 * chantier : il couvre les trois couches d'un coup, et le rôle qui ouvre
 * l'association (que le serveur ne joue jamais en production, faute d'être
 * jamais client DTLS) y est forcément représenté.
 *
 * Les datagrammes ne sont JAMAIS livrés depuis le callback de sortie : ils
 * passent par la file, exactement comme en production. Un réseau ne récurse pas,
 * et le contrat de thread de `SCTPTransport` interdit d'écrire sur le fil depuis
 * la pile.
 *
 * Conception : docs/conception/T140-DC/SPEC.md §5.3 à §5.5, §12.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "sctptransport.h"
#include "t140datachannel.h"

namespace {

// Un pair complet : la pile, le canal T.140, et ce qu'ils ont remonté.
class Peer :
	public SCTPTransport::Listener,
	public T140DataChannel::Listener
{
public:
	Peer() : sctp(*this), t140(sctp,*this) {}

	bool Start(WORD localPort,WORD remotePort)
	{
		return sctp.Init(localPort,remotePort) == 1;
	}

	// SCTPTransport::Listener
	void onSCTPOutboundReady() override {}
	void onSCTPMessage(WORD streamId,DWORD ppid,const BYTE* data,DWORD size) override
	{
		t140.OnMessage(streamId,ppid,data,size);
	}
	void onSCTPAssociationUp() override   { t140.OnAssociationUp(); }
	void onSCTPAssociationDown() override { t140.OnAssociationDown(); }

	// T140DataChannel::Listener
	void onT140Block(const BYTE* data,DWORD size) override
	{
		blocks.push_back(std::string((const char*)data,size));
	}
	void onT140ChannelOpen() override { opens++; }
	void onT140ChannelLost() override { losts++; }

	SCTPTransport		sctp;
	T140DataChannel		t140;
	std::vector<std::string> blocks;
	int			opens = 0;
	int			losts = 0;
};

// Transporte tout ce qui attend, dans les deux sens, et bat les timers de la
// pile. Rend le nombre de datagrammes transportés.
int Pump(Peer& a,Peer& b)
{
	int moved = 0;
	std::string datagram;

	// Vider d'abord les DEUX files, puis livrer : c'est ce qui interdit à un
	// datagramme d'en engendrer un autre dans le même tour, donc à la récursion
	// de s'installer.
	std::vector<std::string> toB, toA;

	while (a.sctp.GetOutbound(datagram))
		toB.push_back(datagram);
	while (b.sctp.GetOutbound(datagram))
		toA.push_back(datagram);

	for (size_t i = 0; i < toB.size(); i++, moved++)
		b.sctp.OnPacket((const BYTE*)toB[i].data(),(DWORD)toB[i].size());
	for (size_t i = 0; i < toA.size(); i++, moved++)
		a.sctp.OnPacket((const BYTE*)toA[i].data(),(DWORD)toA[i].size());

	SCTPTransport::HandleTimers();
	return moved;
}

// Fait tourner la boucle jusqu'à ce que `done` soit vrai, ou expiration.
template <typename Predicate>
bool PumpUntil(Peer& a,Peer& b,Predicate done,int timeoutMs = 2000)
{
	for (int elapsed = 0; elapsed < timeoutMs; elapsed += 2)
	{
		if (done())
			return true;

		Pump(a,b);
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}

	return done();
}

// Deux ports par test : la pile n'est jamais terminée (usrsctp_finish sur une
// socket vivante est un crash connu), donc on ne réutilise pas une paire de
// ports d'un test à l'autre.
class SCTPLoopback : public ::testing::Test
{
protected:
	void SetUp() override
	{
		static WORD nextPort = 5000;
		localPort  = nextPort++;
		remotePort = nextPort++;

		ASSERT_TRUE(a.Start(localPort,remotePort));
		ASSERT_TRUE(b.Start(remotePort,localPort));
	}

	void TearDown() override
	{
		a.sctp.End();
		b.sctp.End();
	}

	// Association montée des deux côtés, puis canal t140 ouvert par A. Le flux
	// impair est celui du serveur DTLS (RFC 8832 §6), le rôle que nous tenons.
	void EstablishChannel()
	{
		ASSERT_TRUE(PumpUntil(a,b,[&]{ return a.sctp.IsUp() && b.sctp.IsUp(); }));
		ASSERT_EQ(1,a.t140.OpenChannel(1));
		ASSERT_TRUE(PumpUntil(a,b,[&]{ return a.t140.IsOpen() && b.t140.IsOpen(); }));
	}

	// Émet depuis `from` et attend que `to` ait reçu `count` blocs.
	bool SendAndWait(Peer& from,Peer& to,const std::string& text,size_t count)
	{
		from.t140.SendText((const BYTE*)text.data(),(DWORD)text.size());
		return PumpUntil(a,b,[&]{ return to.blocks.size() >= count; });
	}

	WORD localPort  = 0;
	WORD remotePort = 0;
	Peer a;
	Peer b;
};

TEST_F(SCTPLoopback, LAssociationSEtablitDesDeuxCotes)
{
	ASSERT_TRUE(PumpUntil(a,b,[&]{ return a.sctp.IsUp() && b.sctp.IsUp(); }));

	EXPECT_TRUE(a.sctp.IsUp());
	EXPECT_TRUE(b.sctp.IsUp());
}

TEST_F(SCTPLoopback, LeCanalSOuvreDesDeuxCotesSurLeMemeFlux)
{
	EstablishChannel();

	EXPECT_EQ(1,a.opens);
	EXPECT_EQ(1,b.opens);
	EXPECT_EQ((WORD)1,a.t140.GetStreamId());
	EXPECT_EQ((WORD)1,b.t140.GetStreamId());
}

TEST_F(SCTPLoopback, UnT140blockTraverseDansLesDeuxSens)
{
	EstablishChannel();

	ASSERT_TRUE(SendAndWait(a,b,"bonjour",1));
	EXPECT_EQ("bonjour",b.blocks[0]);

	ASSERT_TRUE(SendAndWait(b,a,"bonsoir",1));
	EXPECT_EQ("bonsoir",a.blocks[0]);
}

// T.140 porte de l'UTF-8 : un caractère multi-octets ne doit pas être coupé, et
// un message = un T140block (RFC 8865 §5).
TEST_F(SCTPLoopback, LUTF8MultiOctetsArriveIntact)
{
	EstablishChannel();

	const std::string text = "élève — 日本語";
	ASSERT_TRUE(SendAndWait(a,b,text,1));

	EXPECT_EQ(text,b.blocks[0]);
	EXPECT_EQ((size_t)1,b.blocks.size()) << "le bloc a ete decoupe";
}

// Chaque frappe est un T140block, et l'ordre est celui de la frappe : le canal
// est fiable et ordonné.
TEST_F(SCTPLoopback, LesBlocsArriventDansLOrdre)
{
	EstablishChannel();

	const char* frappes[] = { "b", "o", "n", "j", "o", "u", "r" };
	const size_t count = sizeof(frappes)/sizeof(frappes[0]);

	for (size_t i = 0; i < count; i++)
		a.t140.SendText((const BYTE*)frappes[i],1);

	ASSERT_TRUE(PumpUntil(a,b,[&]{ return b.blocks.size() >= count; }));

	std::string reassemble;
	for (size_t i = 0; i < b.blocks.size(); i++)
		reassemble += b.blocks[i];

	EXPECT_EQ("bonjour",reassemble);
}

// Le tampon d'avant-ouverture : entre le 200 OK et l'ouverture du canal il
// s'écoule un aller-retour SDP, un ICE et un handshake DTLS. Sans lui, la
// première phrase — celle où l'appelant se présente — est perdue.
TEST_F(SCTPLoopback, LeTexteEmisAvantLOuvertureEstRejoue)
{
	ASSERT_TRUE(PumpUntil(a,b,[&]{ return a.sctp.IsUp() && b.sctp.IsUp(); }));

	// Aucun canal encore : ces deux blocs doivent attendre.
	ASSERT_FALSE(a.t140.IsOpen());
	a.t140.SendText((const BYTE*)"bonjour ",8);
	a.t140.SendText((const BYTE*)"je suis Alice",13);

	// Rien n'est parti.
	Pump(a,b);
	EXPECT_TRUE(b.blocks.empty());

	ASSERT_EQ(1,a.t140.OpenChannel(1));
	ASSERT_TRUE(PumpUntil(a,b,[&]{ return b.blocks.size() >= 2; }));

	ASSERT_EQ((size_t)2,b.blocks.size());
	EXPECT_EQ("bonjour ",b.blocks[0]);
	EXPECT_EQ("je suis Alice",b.blocks[1]);
}

// Un T140block vide se dit par son propre PPID : un message SCTP de longueur
// nulle n'existe pas.
TEST_F(SCTPLoopback, UnT140blockVideArriveVide)
{
	EstablishChannel();

	a.t140.SendText(NULL,0);
	ASSERT_TRUE(PumpUntil(a,b,[&]{ return !b.blocks.empty(); }));

	EXPECT_EQ("",b.blocks[0]);
}

// T.140 est du texte. Un message binaire sur le canal ne remonte pas : rien ne
// dit ce qu'il contient, et le remonter comme du texte casserait l'UTF-8.
TEST_F(SCTPLoopback, UnMessageBinaireNeRemontePas)
{
	EstablishChannel();

	const BYTE binary[] = { 0x00, 0xff, 0x7f };
	ASSERT_GT(a.sctp.Send(a.t140.GetStreamId(),T140DataChannel::PPIDBinary,binary,sizeof(binary)),0);

	// Et un vrai T140block juste après : il doit arriver, seul.
	ASSERT_TRUE(SendAndWait(a,b,"apres",1));

	ASSERT_EQ((size_t)1,b.blocks.size());
	EXPECT_EQ("apres",b.blocks[0]);
}

// Un canal ouvert sans le sous-protocole `t140` est accepté quand même, faute de
// mieux : le WebSocket a enseigné qu'un client déployé ne se corrige pas.
TEST_F(SCTPLoopback, UnCanalSansSousProtocoleT140EstAcceptePourLeTexte)
{
	ASSERT_TRUE(PumpUntil(a,b,[&]{ return a.sctp.IsUp() && b.sctp.IsUp(); }));

	DCEP::Open open;
	open.channelType = DCEP::Reliable;
	open.label	 = "chat";
	open.protocol	 = "";

	BYTE buffer[64];
	const DWORD length = DCEP::SerializeOpen(open,buffer,sizeof(buffer));
	ASSERT_GT(length,(DWORD)0);
	ASSERT_GT(a.sctp.Send(3,T140DataChannel::PPIDControl,buffer,length),0);

	ASSERT_TRUE(PumpUntil(a,b,[&]{ return b.t140.IsOpen(); }));

	EXPECT_EQ((WORD)3,b.t140.GetStreamId());
	EXPECT_EQ(1,b.opens);
}

// La fin de l'association ferme le canal et le DIT : c'est ce qui déclenche le
// U+FFFD de T.140 §5.3 chez l'adaptateur, vers la jambe qui survit.
TEST_F(SCTPLoopback, LaFinDeLAssociationFermeLeCanalEtLeDit)
{
	EstablishChannel();

	ASSERT_TRUE(a.t140.IsOpen());
	ASSERT_EQ(0,a.losts);

	a.sctp.End();

	EXPECT_FALSE(a.t140.IsOpen());
	EXPECT_EQ(1,a.losts);

	// End est idempotent, et ne redit pas une perte déjà annoncée.
	a.sctp.End();
	EXPECT_EQ(1,a.losts);
}

} // namespace
