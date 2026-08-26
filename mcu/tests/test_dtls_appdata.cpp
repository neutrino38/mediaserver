/**
 * test_dtls_appdata.cpp — données applicatives DTLS, la couture d'un data channel.
 *
 * Le DTLS de ce serveur ne servait qu'à dériver les clés SRTP : `SSL_read` était
 * appelé une fois par datagramme entrant et son résultat jeté. Un data channel
 * WebRTC (RFC 8831) est du SCTP dans ces mêmes records applicatifs, d'où la
 * sortie ajoutée à `DTLSConnection` — voir docs/conception/T140-DC/SPEC.md §5.1.
 *
 * Ce que ces tests couvrent, et qui n'était couvert nulle part :
 *
 *  - le handshake DTLS aboutit toujours et pose bien les clés SRTP. C'est le
 *    GARDE-FOU de la modification : l'export des clés est désormais conditionné
 *    à la négociation de l'extension use_srtp (RFC 5764), et toute jambe RTP
 *    doit continuer d'y passer ;
 *  - l'authentification du pair est séparée de l'export SRTP, et une empreinte
 *    qui ne correspond pas ne pose AUCUNE clé ;
 *  - un bloc applicatif traverse, entier, dans les deux sens ;
 *  - PLUSIEURS blocs livrés dans un même datagramme sortent tous — c'est
 *    exactement ce que l'unique `SSL_read` d'avant perdait ;
 *  - sans consommateur posé, rien ne change et rien ne casse ;
 *  - avant la fin du handshake, il n'y a pas de canal applicatif.
 *
 * Deux `DTLSConnection` dialoguent en mémoire, sans socket : `Read` vide le
 * write_bio de l'une, `Write` remplit le read_bio de l'autre. Le certificat est
 * généré à l'exécution (tests/dtlsfixture.h).
 */
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "dtls.h"
#include "dtlsfixture.h"

namespace {

// --- Un pair DTLS : la connexion, ses clés SRTP reçues, ses blocs applicatifs.
class Peer :
	public DTLSConnection::Listener,
	public DTLSConnection::ApplicationListener
{
public:
	explicit Peer(bool client) : dtls(*this), isClient(client) {}

	// Le rôle local est déduit de celui du pair : remote passive -> nous active.
	bool Start(const std::string& peerFingerprint)
	{
		dtls.SetRemoteSetup(isClient ? DTLSConnection::SETUP_PASSIVE
					     : DTLSConnection::SETUP_ACTIVE);
		dtls.SetRemoteFingerprint(DTLSConnection::SHA256, peerFingerprint.c_str());
		return dtls.Init() == 1;
	}

	// DTLSConnection::Listener
	void onDTLSSetup(DTLSConnection::Suite suite,
			 BYTE* localMasterKey, DWORD localMasterKeySize,
			 BYTE* remoteMasterKey, DWORD remoteMasterKeySize) override
	{
		srtpSetups++;
		lastLocalKeySize = localMasterKeySize;
	}

	// DTLSConnection::ApplicationListener
	void onDTLSApplicationData(const BYTE* data, DWORD size) override
	{
		received.push_back(std::string((const char*)data, size));
	}

	DTLSConnection dtls;
	bool           isClient;
	int            srtpSetups       = 0;
	DWORD          lastLocalKeySize = 0;
	std::vector<std::string> received;
};

// Vide tout ce que `from` a à émettre et le remet à `to`. Rend le nombre de
// datagrammes transportés.
int Pump(Peer& from, Peer& to)
{
	// Assez large pour un flight entier : plusieurs records dans un même transfert
	// est le cas NORMAL, et c'est ce que la boucle SSL_read doit encaisser.
	BYTE buffer[8192];
	int  moved = 0;
	int  len;

	while ((len = from.dtls.Read(buffer, sizeof(buffer))) > 0)
	{
		to.dtls.Write(buffer, len);
		moved++;
	}

	return moved;
}

// Mène le handshake à son terme. Rend false s'il n'aboutit pas.
bool Handshake(Peer& client, Peer& server)
{
	for (int i = 0; i < 32; i++)
	{
		const int a = Pump(client, server);
		const int b = Pump(server, client);

		if (client.dtls.IsHandshakeCompleted() && server.dtls.IsHandshakeCompleted())
			return true;

		// Plus rien ne circule et personne n'a fini : inutile de tourner.
		if (!a && !b)
			return false;
	}

	return false;
}

// Paire prête : handshake fait, consommateurs applicatifs posés.
class DTLSPair
{
public:
	DTLSPair() : client(true), server(false)
	{
		if (!DTLSTestCertificate::Ensure())
			return;

		const std::string fp = DTLSTestCertificate::FingerPrint();

		// Les deux pairs partagent le certificat du binaire : chacun attend donc
		// l'empreinte de l'autre, qui est la même.
		if (!client.Start(fp) || !server.Start(fp))
			return;

		ready = Handshake(client, server);
	}

	void WireApplicationListeners()
	{
		client.dtls.SetApplicationListener(&client);
		server.dtls.SetApplicationListener(&server);
	}

	// Chiffre `text` chez `from` et le remet à `to`.
	void SendApplication(Peer& from, Peer& to, const std::string& text)
	{
		ASSERT_GT(from.dtls.WriteApplicationData((const BYTE*)text.data(), text.size()), 0);
		Pump(from, to);
	}

	Peer client;
	Peer server;
	bool ready = false;
};

// --- Le garde-fou : une jambe RTP doit continuer d'obtenir ses clés SRTP.
TEST(DTLSApplicationData, LeHandshakeAboutitEtLesClesSRTPSortent)
{
	DTLSPair pair;

	ASSERT_TRUE(pair.ready) << "le handshake DTLS n'a pas abouti";

	EXPECT_EQ(1, pair.client.srtpSetups);
	EXPECT_EQ(1, pair.server.srtpSetups);
	// 16 octets de clé + 14 de sel : la matière que SRTP attend.
	EXPECT_EQ((DWORD)30, pair.client.lastLocalKeySize);
	EXPECT_EQ((DWORD)30, pair.server.lastLocalKeySize);
}

// L'authentification du pair ne dépend pas de ce que le transport portera : une
// empreinte qui ne colle pas ne pose aucune clé, donc rien ne se déchiffrera.
TEST(DTLSApplicationData, UneEmpreinteFausseNePoseAucuneCle)
{
	ASSERT_TRUE(DTLSTestCertificate::Ensure());

	Peer client(true);
	Peer server(false);

	const std::string good = DTLSTestCertificate::FingerPrint();
	// Même forme, premier octet changé : le certificat sera bien lu, et refusé.
	std::string bad = good;
	bad[0] = (bad[0] == 'A') ? 'B' : 'A';

	ASSERT_TRUE(client.Start(bad));
	ASSERT_TRUE(server.Start(good));

	// Le handshake TLS lui-même aboutit (le certificat est accepté par le
	// callback de vérification) : c'est la comparaison d'empreinte qui tranche.
	Handshake(client, server);

	EXPECT_EQ(0, client.srtpSetups) << "des clés SRTP ont été posées sur un pair non authentifié";
	EXPECT_EQ(1, server.srtpSetups);
}

TEST(DTLSApplicationData, UnBlocApplicatifTraverseDansLesDeuxSens)
{
	DTLSPair pair;
	ASSERT_TRUE(pair.ready);
	pair.WireApplicationListeners();

	pair.SendApplication(pair.client, pair.server, "bonjour");
	ASSERT_EQ((size_t)1, pair.server.received.size());
	EXPECT_EQ("bonjour", pair.server.received[0]);

	pair.SendApplication(pair.server, pair.client, "bonsoir");
	ASSERT_EQ((size_t)1, pair.client.received.size());
	EXPECT_EQ("bonsoir", pair.client.received[0]);
}

// Les octets ne sont pas interprétés : un T140block porte de l'UTF-8, et un
// paquet SCTP du binaire quelconque, zéros compris.
TEST(DTLSApplicationData, LesOctetsBinairesPassentTelsQuels)
{
	DTLSPair pair;
	ASSERT_TRUE(pair.ready);
	pair.WireApplicationListeners();

	std::string payload;
	for (int i = 0; i < 256; i++)
		payload.push_back((char)i);

	pair.SendApplication(pair.client, pair.server, payload);

	ASSERT_EQ((size_t)1, pair.server.received.size());
	EXPECT_EQ(payload, pair.server.received[0]);
}

// LE défaut que la boucle corrige : plusieurs records applicatifs remis en un
// seul transfert. L'unique SSL_read d'avant en rendait un et perdait le reste.
TEST(DTLSApplicationData, PlusieursBlocsDansUnMemeTransfertSortentTous)
{
	DTLSPair pair;
	ASSERT_TRUE(pair.ready);
	pair.WireApplicationListeners();

	const char* blocks[] = { "un", "deux", "trois" };

	// Trois chiffrements AVANT le moindre transfert : les trois records
	// s'empilent dans le write_bio et repartent ensemble.
	for (int i = 0; i < 3; i++)
		ASSERT_GT(pair.client.dtls.WriteApplicationData(
				  (const BYTE*)blocks[i], strlen(blocks[i])), 0);

	Pump(pair.client, pair.server);

	ASSERT_EQ((size_t)3, pair.server.received.size());
	EXPECT_EQ("un",    pair.server.received[0]);
	EXPECT_EQ("deux",  pair.server.received[1]);
	EXPECT_EQ("trois", pair.server.received[2]);
}

// Sans consommateur posé : le comportement d'avant, et pas un crash.
TEST(DTLSApplicationData, SansConsommateurLesDonneesSontJetees)
{
	DTLSPair pair;
	ASSERT_TRUE(pair.ready);
	// Volontairement : pas de WireApplicationListeners().

	pair.SendApplication(pair.client, pair.server, "personne n'ecoute");

	EXPECT_TRUE(pair.server.received.empty());
	// La jambe reste utilisable : les clés SRTP sont posées et rien n'a cassé.
	EXPECT_EQ(1, pair.server.srtpSetups);
}

// Avant la fin du handshake, il n'y a pas de canal applicatif : le pair n'est
// pas authentifié, et SSL_write pousserait dans le flight en cours.
TEST(DTLSApplicationData, RienNePartAvantLaFinDuHandshake)
{
	ASSERT_TRUE(DTLSTestCertificate::Ensure());

	Peer client(true);
	const std::string fp = DTLSTestCertificate::FingerPrint();

	ASSERT_TRUE(client.Start(fp));
	ASSERT_FALSE(client.dtls.IsHandshakeCompleted());

	EXPECT_EQ(0, client.dtls.WriteApplicationData((const BYTE*)"trop tot", 8));
}

} // namespace
