/**
 * test_datachannel.cpp — DCEP, le protocole qui ouvre un data channel (RFC 8832).
 *
 * Deux messages, sur le PPID 50, et le canal est là. Le premier — DATA_CHANNEL_OPEN
 * — porte DEUX longueurs déclarées par le pair, celle du label et celle du
 * sous-protocole, suivies des chaînes elles-mêmes. C'est la forme exacte du
 * défaut que le chantier de durcissement des parseurs a traqué partout ailleurs :
 * un pair qui annonce 60 000 octets de label dans un message de 20 fait lire le
 * parseur bien au-delà de ce qui est arrivé.
 *
 * Le dernier test le prouve autrement qu'en croyant le code : le message est
 * placé en fin de page, la page suivante interdite. Une lecture d'un seul octet
 * de trop tue le processus fils, et le test échoue.
 *
 * Conception : docs/conception/T140-DC/SPEC.md §5.4.
 */
#include <gtest/gtest.h>

#include <string.h>
#include <string>

#include "datachannel.h"
#include "guardedbuffer.h"

namespace {

// Un DATA_CHANNEL_OPEN construit à la main, sans passer par le sérialiseur : on
// veut tester le parseur contre des octets, pas contre son inverse.
std::string BuildOpen(BYTE channelType,WORD priority,DWORD reliability,
		      const std::string& label,const std::string& protocol,
		      WORD declaredLabelLength,WORD declaredProtocolLength)
{
	std::string msg;
	msg.push_back((char) DCEP::MessageOpen);
	msg.push_back((char) channelType);
	msg.push_back((char) (priority >> 8));
	msg.push_back((char) (priority & 0xff));
	msg.push_back((char) (reliability >> 24));
	msg.push_back((char) ((reliability >> 16) & 0xff));
	msg.push_back((char) ((reliability >> 8) & 0xff));
	msg.push_back((char) (reliability & 0xff));
	msg.push_back((char) (declaredLabelLength >> 8));
	msg.push_back((char) (declaredLabelLength & 0xff));
	msg.push_back((char) (declaredProtocolLength >> 8));
	msg.push_back((char) (declaredProtocolLength & 0xff));
	msg += label;
	msg += protocol;
	return msg;
}

TEST(DCEPOpen, UnOpenBienFormeSeRelitEntierement)
{
	const std::string msg = BuildOpen(DCEP::Reliable,42,0,"chat","t140",4,4);

	DCEP::Open open;
	ASSERT_TRUE(DCEP::ParseOpen((const BYTE*)msg.data(),msg.size(),open));

	EXPECT_EQ((BYTE)DCEP::Reliable,open.channelType);
	EXPECT_EQ((WORD)42,open.priority);
	EXPECT_EQ((DWORD)0,open.reliability);
	EXPECT_EQ("chat",open.label);
	EXPECT_EQ("t140",open.protocol);
	EXPECT_TRUE(open.IsReliableOrdered());
}

TEST(DCEPOpen, LAllerRetourDeSerialisationConserveTout)
{
	DCEP::Open sent;
	sent.channelType = DCEP::Reliable;
	sent.priority	 = 256;
	sent.reliability = 0;
	sent.label	 = "t140";
	sent.protocol	 = "t140";

	BYTE buffer[64];
	const DWORD length = DCEP::SerializeOpen(sent,buffer,sizeof(buffer));
	ASSERT_EQ(DCEP::OpenHeaderLength + 8,length);

	DCEP::Open received;
	ASSERT_TRUE(DCEP::ParseOpen(buffer,length,received));

	EXPECT_EQ(sent.channelType,received.channelType);
	EXPECT_EQ(sent.priority,received.priority);
	EXPECT_EQ(sent.label,received.label);
	EXPECT_EQ(sent.protocol,received.protocol);
}

// LE défaut de cette famille de parseurs : la longueur annoncée dépasse ce qui
// est arrivé. Refuser est la seule réponse — tronquer serait accepter un message
// que le pair n'a pas envoyé.
TEST(DCEPOpen, UneLongueurDeLabelMensongereEstRefusee)
{
	const std::string msg = BuildOpen(DCEP::Reliable,0,0,"chat","t140",60000,4);

	DCEP::Open open;
	EXPECT_FALSE(DCEP::ParseOpen((const BYTE*)msg.data(),msg.size(),open));
}

TEST(DCEPOpen, UneLongueurDeProtocolMensongereEstRefusee)
{
	const std::string msg = BuildOpen(DCEP::Reliable,0,0,"chat","t140",4,60000);

	DCEP::Open open;
	EXPECT_FALSE(DCEP::ParseOpen((const BYTE*)msg.data(),msg.size(),open));
}

// Les deux longueurs sont des WORD : additionnées dans un type trop court, elles
// débordent et le contrôle passe. C'est pourquoi la somme se fait en DWORD.
TEST(DCEPOpen, LaSommeDesDeuxLongueursNeDebordePas)
{
	const std::string msg = BuildOpen(DCEP::Reliable,0,0,"","",0xFFFF,0xFFFF);

	DCEP::Open open;
	EXPECT_FALSE(DCEP::ParseOpen((const BYTE*)msg.data(),msg.size(),open));
}

TEST(DCEPOpen, UnMessageTronqueEstRefuse)
{
	const std::string full = BuildOpen(DCEP::Reliable,0,0,"chat","t140",4,4);

	DCEP::Open open;

	// Toutes les troncatures, du message vide à l'en-tête incomplet.
	for (size_t size = 0; size < DCEP::OpenHeaderLength; size++)
		EXPECT_FALSE(DCEP::ParseOpen((const BYTE*)full.data(),size,open))
			<< "accepte a " << size << " octets";
}

TEST(DCEPOpen, UnAutreTypeDeMessageNEstPasUnOpen)
{
	std::string msg = BuildOpen(DCEP::Reliable,0,0,"chat","t140",4,4);
	msg[0] = (char) 0x7f;

	DCEP::Open open;
	EXPECT_FALSE(DCEP::ParseOpen((const BYTE*)msg.data(),msg.size(),open));
}

// Un canal non fiable ou non ordonné se lit quand même : c'est la couche T.140
// qui décide quoi en faire, et RFC 8865 en demande un fiable et ordonné.
TEST(DCEPOpen, UnCanalNonFiableSeLitMaisSeSignale)
{
	const std::string msg = BuildOpen(DCEP::PartialReliableTimedUnordered,0,3000,"chat","t140",4,4);

	DCEP::Open open;
	ASSERT_TRUE(DCEP::ParseOpen((const BYTE*)msg.data(),msg.size(),open));

	EXPECT_FALSE(open.IsReliableOrdered());
	EXPECT_EQ((DWORD)3000,open.reliability);
}

TEST(DCEPOpen, LaSerialisationRefuseUnTamponTropPetit)
{
	DCEP::Open open;
	open.label    = "t140";
	open.protocol = "t140";

	BYTE buffer[DCEP::OpenHeaderLength + 8];

	// Un octet de moins que le strict nécessaire.
	EXPECT_EQ((DWORD)0,DCEP::SerializeOpen(open,buffer,sizeof(buffer) - 1));
	EXPECT_EQ(sizeof(buffer),DCEP::SerializeOpen(open,buffer,sizeof(buffer)));
}

TEST(DCEPAck, UnAckEstUnOctetEtRienDAutre)
{
	BYTE buffer[4];
	ASSERT_EQ((DWORD)1,DCEP::SerializeAck(buffer,sizeof(buffer)));
	EXPECT_EQ((BYTE)DCEP::MessageAck,buffer[0]);
	EXPECT_TRUE(DCEP::IsAck(buffer,1));

	// Rien à écrire dans un tampon vide.
	EXPECT_EQ((DWORD)0,DCEP::SerializeAck(buffer,0));

	// Et ce qui n'est pas un ACK ne doit pas être pris pour tel.
	const BYTE open = DCEP::MessageOpen;
	EXPECT_FALSE(DCEP::IsAck(&open,1));
	EXPECT_FALSE(DCEP::IsAck(buffer,0));
	EXPECT_FALSE(DCEP::IsAck(NULL,1));
}

// La preuve, plutôt que la relecture : le message finit au bord d'une page
// interdite. Si le parseur lit un octet de trop sur une longueur mensongère, le
// fils meurt d'un SIGSEGV au lieu de sortir avec 0.
TEST(DCEPOpen, UneLongueurMensongereNeFaitPasLireHorsDuMessage)
{
	const std::string msg = BuildOpen(DCEP::Reliable,0,0,"chat","t140",0xFFFF,0xFFFF);
	GuardedBuffer guarded(msg.data(),msg.size());
	ASSERT_TRUE(guarded.IsValid());

	EXPECT_EXIT({
		DCEP::Open open;
		DCEP::ParseOpen(guarded.data(),guarded.Size(),open);
		_exit(0);
	},::testing::ExitedWithCode(0),"");
}

} // namespace
