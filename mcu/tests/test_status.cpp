/**
 * test_status.cpp — /status/general : les deux rendus du MÊME état.
 *
 * Ce que ces tests protègent, ce n'est pas le formatage : c'est l'invariant qui
 * a motivé la conception du handler. JSON et texte sont deux VUES d'un unique
 * StatusHandler::Info, collecté une seule fois. Deux collectes indépendantes
 * finiraient par se contredire — un statut qui se contredit ne sert plus à
 * décider, et c'est exactement le défaut qui a coûté un appel AV1 mort en 488
 * (cf. CLAUDE.md, « ce que le serveur sait de lui-même »).
 *
 * Deux garde-fous de fond :
 *   - les capacités viennent des fabriques de libmedikit, jamais d'une liste
 *     écrite à la main (c'est ce qu'était GetSupportedCodecs de xmlrpcmcu.cpp,
 *     8 codecs figés sans OPUS) ;
 *   - les sens ÉMISSION et RÉCEPTION sont publiés séparément, parce qu'ils ne
 *     coïncident pas.
 */
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "log.h"
#include "statushandler.h"
#include "addressprofiles.h"
#include "dtls.h"
#include "audio.h"
#include "video.h"
#include "medkit/codecs.h"

namespace {

// Un instant fixe : sans lui l'uptime rendrait tout test irreproductible.
const time_t kStartedAt = 1700000000;
const time_t kNow       = kStartedAt + 3*86400 + 4*3600 + 12*60 + 33;

// Pas de MCU ni de JSR309Manager : le handler ne les possède pas, il les
// observe. Les compteurs valent alors 0, ce qui est précisément le cas d'un
// serveur au repos — et cela garde le test libre de toute conférence.
StatusHandler::Info CollectFixture(int eventQueueExpiresSecs = 60)
{
	StatusHandler handler(NULL,NULL,kStartedAt,eventQueueExpiresSecs);
	StatusHandler::Info info;

	handler.Collect(info,kNow);

	return info;
}

bool Contains(const std::vector<std::string>& v,const std::string& name)
{
	for (size_t i=0; i<v.size(); i++)
		if (v[i] == name)
			return true;

	return false;
}

} // namespace

/* ------------------------ Collecte ------------------------ */

TEST(Status, LUptimeSeCompteDepuisLInstantDeDemarrage)
{
	const StatusHandler::Info info = CollectFixture();

	EXPECT_EQ(kStartedAt,info.startedAt);
	EXPECT_EQ(kNow - kStartedAt,info.uptimeSecs);
}

TEST(Status, LesCapacitesViennentDesFabriquesPasDUneListeEcriteAMain)
{
	const StatusHandler::Info info = CollectFixture();

	// Autant d'entrées que la fabrique en déclare, et dans le même ordre :
	// toute divergence signalerait une liste recopiée quelque part.
	const std::vector<AudioCodec::Type>& audio = AudioCodecFactory::GetSupportedCodecs();
	ASSERT_EQ(audio.size(),info.audioDecode.size());
	for (size_t i=0; i<audio.size(); i++)
		EXPECT_EQ(std::string(AudioCodec::GetNameFor(audio[i])),info.audioDecode[i]);

	const std::vector<VideoCodec::Type>& video = VideoCodecFactory::GetSupportedCodecs();
	ASSERT_EQ(video.size(),info.videoDecode.size());
	for (size_t i=0; i<video.size(); i++)
		EXPECT_EQ(std::string(VideoCodec::GetNameFor(video[i])),info.videoDecode[i]);
}

TEST(Status, OpusEstAnnonceDesQueLaLibLeSupporte)
{
	// Le défaut historique en personne : l'ancien GetSupportedCodecs XML-RPC
	// listait 8 codecs audio à la main, sans OPUS — celui de tous les appels
	// réels. Si la lib le supporte, le statut DOIT le dire.
	if (!AudioCodec::IsSupported(AudioCodec::OPUS))
		GTEST_SKIP() << "ffmpeg sans decodeur opus sur cette machine";

	EXPECT_TRUE(Contains(CollectFixture().audioDecode,"OPUS"));
}

TEST(Status, LesDeuxSensSontPubliesSeparement)
{
	const StatusHandler::Info info = CollectFixture();

	// VP6 est le cas d'école : ffmpeg le décode (flux RTMP entrants) et n'a
	// aucun encodeur pour lui. Le publier dans un seul champ « supporté »
	// laisserait un contrôleur demander au serveur d'EMETTRE du VP6.
	if (VideoCodec::IsSupported(VideoCodec::VP6))
	{
		EXPECT_TRUE(Contains(info.videoDecode,"VP6"));
		EXPECT_FALSE(Contains(info.videoEncode,"VP6"))
			<< "VP6 annonce en emission : aucun encodeur VP6 n'existe";
	}

	EXPECT_FALSE(VideoCodec::IsEncodingSupported(VideoCodec::VP6));
}

TEST(Status, LeTexteRfc4103EstToujoursDisponible)
{
	const StatusHandler::Info info = CollectFixture();

	// T140/T140RED sont natifs (aucune dépendance externe) : les annoncer
	// indisponibles serait un bug de câblage, pas un fait d'environnement.
	EXPECT_TRUE(info.textRtp);
	EXPECT_TRUE(info.textRtpRedundancy);
	EXPECT_TRUE(info.textWebSocket);
}

TEST(Status, LeDataChannelSuitLaDisponibiliteDuDtls)
{
	const StatusHandler::Info info = CollectFixture();

	// RFC 8865 = SCTP dans des records DTLS. La dépendance est structurelle :
	// annoncer le data channel sans DTLS promettrait un canal impossible.
	EXPECT_EQ(DTLSConnection::IsAvailable(),info.textDataChannel);

	// Même origine pour dtls-srtp dans la liste des modes.
	EXPECT_EQ(DTLSConnection::IsAvailable(),Contains(info.encryptionModes,"dtls-srtp"));
	// Le clair et le SDES ne dependent d'aucun certificat.
	EXPECT_TRUE(Contains(info.encryptionModes,"none"));
	EXPECT_TRUE(Contains(info.encryptionModes,"sdes-srtp"));
}

TEST(Status, LesQuatreProfilsDAdressageSontTousDecrits)
{
	const StatusHandler::Info info = CollectFixture();

	// Disponibles ou non : un profil absent de la réponse est indiscernable
	// d'un profil que le serveur ignore, et le contrôleur ne peut pas trancher.
	ASSERT_EQ((size_t) AddressProfiles::Count,info.profiles.size());

	int defaults = 0;
	for (size_t i=0; i<info.profiles.size(); i++)
	{
		const StatusHandler::Info::Profile& p = info.profiles[i];

		EXPECT_EQ(std::string(AddressProfiles::NameOf((AddressProfiles::Id) i)),p.name);
		if (p.isDefault)
			defaults++;
		// Un profil indisponible ne doit porter AUCUNE adresse : une adresse
		// sur un profil mort serait lue comme annonçable.
		if (!p.available)
		{
			EXPECT_TRUE(p.bindAddress.empty());
			EXPECT_TRUE(p.announcedAddress.empty());
		}
	}
	EXPECT_EQ(1,defaults) << "exactement un profil par defaut";
}

TEST(Status, UnProfilNatteMontreSesDEUXAdresses)
{
	// Le cas qui justifie la table entière : derrière NAT, « lie » et
	// « annonce » DIFFERENT. Un statut qui n'en publierait qu'une laisserait le
	// contrôleur croire annonçable une adresse RFC 1918 — la devinette par
	// appelant que les profils ont précisément supprimée.
	AddressProfiles::Reset();

	std::string error;
	ASSERT_TRUE(AddressProfiles::AddPublic(IPAddress::Parse("192.168.42.7"),error)) << error;
	ASSERT_TRUE(AddressProfiles::SetNat(IPAddress::Parse("203.0.113.9"),error)) << error;
	ASSERT_TRUE(AddressProfiles::Freeze(error)) << error;

	const StatusHandler::Info info = CollectFixture();
	const std::string json = StatusHandler::RenderJSON(info);
	const std::string text = StatusHandler::RenderText(info);

	bool seen = false;
	for (size_t i=0; i<info.profiles.size(); i++)
	{
		const StatusHandler::Info::Profile& p = info.profiles[i];

		if (p.name != "publicv4")
			continue;
		seen = true;
		ASSERT_TRUE(p.available);
		// L'adresse de --public-ip n'est pas attachee a cette machine, donc le
		// bind reste VIDE : c'est l'ecoute toutes interfaces, le cas nominal du
		// NAT (cf. AddressProfiles::AddPublic). Le fait publie doit etre celui-la.
		EXPECT_TRUE(p.bindAddress.empty()) << "bind=" << p.bindAddress;
		EXPECT_EQ("203.0.113.9",p.announcedAddress);
	}
	EXPECT_TRUE(seen) << "profil publicv4 absent de la reponse";

	// L'adresse annoncee ET le nom du profil, dans les DEUX rendus : c'est ce
	// qu'un remplacement du nom dans un seul rendu doit faire echouer.
	EXPECT_NE(std::string::npos,json.find("publicv4"))	<< json;
	EXPECT_NE(std::string::npos,json.find("203.0.113.9"))	<< json;
	EXPECT_NE(std::string::npos,text.find("publicv4"))	<< text;
	EXPECT_NE(std::string::npos,text.find("203.0.113.9"))	<< text;
	// Un bind vide se dit "*" dans le rendu humain : une colonne blanche se
	// lirait comme une valeur manquante.
	EXPECT_NE(std::string::npos,text.find("publicv4    *"))	<< text;

	// La table est statique et partagee par tout le processus de test.
	AddressProfiles::Reset();
}

/* -------------------------- Rendus ------------------------- */

TEST(Status, LeJsonEstBienForme)
{
	const std::string json = StatusHandler::RenderJSON(CollectFixture());

	// Pas de parseur JSON dans l'arbre : on verifie l'equilibre des accolades et
	// des crochets hors chaines, ce qui attrape la faute reelle (un separateur
	// oublie en ajoutant un champ).
	int braces = 0, brackets = 0;
	bool inString = false, escaped = false;

	for (size_t i=0; i<json.size(); i++)
	{
		const char c = json[i];

		if (inString)
		{
			if (escaped)		escaped = false;
			else if (c == '\\')	escaped = true;
			else if (c == '"')	inString = false;
			continue;
		}

		switch (c)
		{
			case '"': inString = true;	break;
			case '{': braces++;		break;
			case '}': braces--;		break;
			case '[': brackets++;		break;
			case ']': brackets--;		break;
		}
		ASSERT_GE(braces,0)   << "accolade fermante en trop a l'offset " << i;
		ASSERT_GE(brackets,0) << "crochet fermant en trop a l'offset " << i;
	}

	EXPECT_FALSE(inString)	<< "chaine JSON non fermee";
	EXPECT_EQ(0,braces)	<< "accolades desequilibrees";
	EXPECT_EQ(0,brackets)	<< "crochets desequilibres";
	// Aucune virgule ne doit precéder immediatement une fermeture.
	EXPECT_EQ(std::string::npos,json.find(",}"));
	EXPECT_EQ(std::string::npos,json.find(",]"));
}

TEST(Status, LesChainesJsonSontEchappees)
{
	StatusHandler::Info info = CollectFixture();

	// Un nom d'hote ou un chemin de certificat exotique ne doit pas casser la
	// reponse : c'est le seul champ que le handler ne controle pas.
	info.hostname = "gui\"llemet\\antislash\nretour";

	const std::string json = StatusHandler::RenderJSON(info);

	EXPECT_NE(std::string::npos,json.find("gui\\\"llemet\\\\antislash\\nretour"));
	// Le seul saut de ligne brut autorise est celui qui termine la reponse.
	EXPECT_EQ(json.size()-1,json.find('\n'));
}

TEST(Status, LesDeuxRendusRacontentLeMemeEtat)
{
	const StatusHandler::Info info = CollectFixture();
	const std::string json = StatusHandler::RenderJSON(info);
	const std::string text = StatusHandler::RenderText(info);

	// L'invariant central : le texte n'est pas une seconde collecte, donc il ne
	// peut pas annoncer autre chose que le JSON.
	EXPECT_NE(std::string::npos,json.find(info.version));
	EXPECT_NE(std::string::npos,text.find(info.version));

	for (size_t i=0; i<info.audioDecode.size(); i++)
	{
		EXPECT_NE(std::string::npos,json.find(info.audioDecode[i]))
			<< info.audioDecode[i] << " absent du JSON";
		EXPECT_NE(std::string::npos,text.find(info.audioDecode[i]))
			<< info.audioDecode[i] << " absent du rendu texte";
	}

	for (size_t i=0; i<info.profiles.size(); i++)
	{
		EXPECT_NE(std::string::npos,json.find(info.profiles[i].name));
		EXPECT_NE(std::string::npos,text.find(info.profiles[i].name));
	}
}

TEST(Status, LeRenduTexteDitLUptimeEnClairEtEnSecondes)
{
	const std::string text = StatusHandler::RenderText(CollectFixture());

	// kNow - kStartedAt = 3 jours 04:12:33. Un exploitant lit la forme humaine,
	// un script la valeur brute : les deux doivent y etre.
	EXPECT_NE(std::string::npos,text.find("3 j 04:12:33")) << text;
	EXPECT_NE(std::string::npos,text.find("274353 s"))     << text;
}

TEST(Status, LExpirationDesarmeeEstDiteTelleQuelle)
{
	// 0 = desarme (XmlEventQueue). « 00:00:00 sans long-poll » se lirait comme
	// une destruction immediate : l'inverse exact du sens.
	const std::string armed   = StatusHandler::RenderText(CollectFixture(60));
	const std::string disarmed = StatusHandler::RenderText(CollectFixture(0));

	EXPECT_NE(std::string::npos,armed.find("long-poll"));
	EXPECT_NE(std::string::npos,disarmed.find("desarmee"));
	EXPECT_EQ(std::string::npos,disarmed.find("long-poll"));
}
