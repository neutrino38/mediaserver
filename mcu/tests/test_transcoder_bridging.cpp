/**
 * test_transcoder_bridging.cpp — mode pont dynamique des transcodeurs JSR309.
 *
 * Un transcodeur vidéo qui ré-encode alors que les deux pattes portent le même
 * codec coûte un décodage, un scale et un encodage par appel — pour un flux
 * identique. `AudioTranscoder` sait déjà l'éviter : il regarde le codec du
 * paquet qui ARRIVE, demande au puits s'il sait le porter tel quel
 * (`RTPMultiplexer::TryCodec`, qui interroge tous les listeners attachés), et
 * relaie sans toucher au flux quand la réponse est oui. `VideoTranscoder`
 * décodait toujours. Ces tests fixent la parité.
 *
 * CE QUI EST OBSERVÉ, ce sont les paquets qui ATTEIGNENT le puits, jamais l'état
 * interne (`state`, `recCodec` sont privés, et le mode est une conséquence, pas
 * une valeur à consulter) :
 *
 *   - pont     → le puits reçoit LE MÊME paquet, octet pour octet ;
 *   - décodage → le puits ne reçoit rien directement, le paquet part au décodeur
 *                (qui, sans codec configuré ni image clé, ne produira rien).
 *
 * Le puits est un `Joinable::Listener` dont on pilote `TryCheckCodec` : c'est
 * exactement le contrat qu'un `RTPEndpoint` remplit en production, où il répond
 * en basculant son propre codec d'émission.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "rtp.h"
#include "../src/jsr309/AudioTranscoder.h"
#include "../src/jsr309/VideoTranscoder.h"

namespace {

// Charge utile reconnaissable : distingue un paquet relayé d'un paquet reconstruit.
const BYTE kMagic[] = { 0xC0, 0xFF, 0xEE, 0x42, 0x13, 0x37 };

// Puits instrumenté. `accepted` est la liste des codecs pour lesquels il répond
// « je sais le porter » — un endpoint réel répond ainsi d'après son rtpMap de
// sortie négocié.
class RecordingSink : public Joinable::Listener
{
public:
	explicit RecordingSink(std::vector<int> accepted) : accepted(std::move(accepted)) {}

	void onRTPPacket(RTPPacket &packet) override
	{
		Received got;
		got.codec = packet.GetCodec();
		got.length = packet.GetMediaLength();
		if (got.length > 0 && packet.GetMediaData())
			got.payload.assign(packet.GetMediaData(), packet.GetMediaData() + got.length);
		received.push_back(got);
	}

	void onResetStream() override {}
	void onEndStream() override {}

	int TryCheckCodec(int codec) override
	{
		for (int c : accepted)
			if (c == codec)
				return codec;
		return -1;
	}

	struct Received
	{
		int codec = -1;
		DWORD length = 0;
		std::vector<BYTE> payload;
	};

	std::vector<Received> received;

private:
	std::vector<int> accepted;
};

// Paquet vidéo minimal mais complet : le codec porte la décision, la charge utile
// permet de vérifier qu'un relais ne la touche pas.
RTPPacket MakeVideoPacket(VideoCodec::Type codec, WORD seq, DWORD timestamp)
{
	RTPPacket packet(MediaFrame::Video, codec);
	packet.SetSeqNum(seq);
	packet.SetTimestamp(timestamp);
	memcpy(packet.GetMediaData(), kMagic, sizeof(kMagic));
	packet.SetMediaLength(sizeof(kMagic));
	return packet;
}

// Init(adaptative=false, allowBridging=...) : le premier paramètre pilote
// UseInputSize et n'a rien à voir avec le pont — d'où un drapeau à lui.
class VideoBridgingTest : public ::testing::Test
{
protected:
	void SetUp() override { name = L"tr-test"; }

	std::wstring name;
};

TEST_F(VideoBridgingTest, RelaysUntouchedWhenTheSinkCarriesTheIncomingCodec)
{
	VideoTranscoder transcoder(name);
	ASSERT_EQ(1, transcoder.Init(false, /*allowBridging=*/true));

	RecordingSink sink({ VideoCodec::VP8 });
	transcoder.AddListener(&sink);

	RTPPacket packet = MakeVideoPacket(VideoCodec::VP8, 42, 90000);
	transcoder.onRTPPacket(packet);

	ASSERT_EQ(1u, sink.received.size())
		<< "le puits porte VP8 : le paquet doit lui parvenir relaye, pas decode";
	EXPECT_EQ((int)VideoCodec::VP8, sink.received[0].codec);
	ASSERT_EQ(sizeof(kMagic), sink.received[0].length);
	EXPECT_EQ(0, memcmp(kMagic, sink.received[0].payload.data(), sizeof(kMagic)))
		<< "un relais ne reecrit pas la charge utile";

	transcoder.RemoveListener(&sink);
	transcoder.End();
}

TEST_F(VideoBridgingTest, DecodesWhenTheSinkCannotCarryTheIncomingCodec)
{
	VideoTranscoder transcoder(name);
	ASSERT_EQ(1, transcoder.Init(false, /*allowBridging=*/true));

	// Le puits ne sait porter que VP8 ; il arrive du H.264.
	RecordingSink sink({ VideoCodec::VP8 });
	transcoder.AddListener(&sink);

	RTPPacket packet = MakeVideoPacket(VideoCodec::H264, 7, 90000);
	transcoder.onRTPPacket(packet);

	EXPECT_TRUE(sink.received.empty())
		<< "codec non portable par le puits : le paquet doit passer par le decodeur, "
		   "donc ne PAS atteindre le puits tel quel";

	transcoder.RemoveListener(&sink);
	transcoder.End();
}

// Le drapeau est un opt-in : sans lui, le comportement historique — toujours
// décoder — doit être intact, sinon toute autre utilisation du transcodeur
// (mixage, enregistrement) changerait de sémantique sans le demander.
TEST_F(VideoBridgingTest, WithoutTheFlagEverythingIsDecodedAsBefore)
{
	VideoTranscoder transcoder(name);
	ASSERT_EQ(1, transcoder.Init(false, /*allowBridging=*/false));

	RecordingSink sink({ VideoCodec::VP8 });
	transcoder.AddListener(&sink);

	RTPPacket packet = MakeVideoPacket(VideoCodec::VP8, 1, 90000);
	transcoder.onRTPPacket(packet);

	EXPECT_TRUE(sink.received.empty())
		<< "pont non autorise : meme un codec que le puits porte doit etre decode";

	transcoder.RemoveListener(&sink);
	transcoder.End();
}

// Le codec entrant peut changer en cours de flux — c'est tout l'intérêt de
// décider par paquet. Le mode doit suivre, dans les deux sens.
TEST_F(VideoBridgingTest, TheModeFollowsTheIncomingCodecMidStream)
{
	VideoTranscoder transcoder(name);
	ASSERT_EQ(1, transcoder.Init(false, /*allowBridging=*/true));

	RecordingSink sink({ VideoCodec::VP8 });
	transcoder.AddListener(&sink);

	RTPPacket vp8 = MakeVideoPacket(VideoCodec::VP8, 1, 90000);
	transcoder.onRTPPacket(vp8);
	ASSERT_EQ(1u, sink.received.size());

	// bascule vers un codec que le puits ne porte pas : plus rien ne doit être relayé
	RTPPacket h264 = MakeVideoPacket(VideoCodec::H264, 2, 93000);
	transcoder.onRTPPacket(h264);
	EXPECT_EQ(1u, sink.received.size())
		<< "le pont ne doit pas survivre a un codec que le puits ne porte pas";

	// et retour : le pont se rouvre sans qu'on ait rien renegocie
	RTPPacket vp8_again = MakeVideoPacket(VideoCodec::VP8, 3, 96000);
	transcoder.onRTPPacket(vp8_again);
	EXPECT_EQ(2u, sink.received.size())
		<< "le mode est rejuge a chaque changement de codec, dans les deux sens";

	transcoder.RemoveListener(&sink);
	transcoder.End();
}

// ── Le câblage, et pas seulement l'arbitrage ────────────────────────────────
//
// Tous les tests ci-dessus appellent `onRTPPacket` DIRECTEMENT. Ils prouvent que
// l'arbitrage est juste ; ils ne prouvent pas qu'il est ATTEIGNABLE. C'est
// précisément l'angle mort dans lequel le défaut a vécu : `VideoTranscoder::Attach`
// branchait la source sur le DÉCODEUR quoi qu'il arrive, donc `onRTPPacket`
// n'était jamais appelé en production — `TryCodec` jamais interrogé, tout le
// chemin pont mort — et cette suite passait au vert dessus. Le 2026-08-12, un
// appel AV1 ↔ AV1 a donc décodé un flux que rien ne préparait, écran noir des
// deux côtés, tandis que l'audio opus ↔ opus relayait très bien : `AudioTranscoder`
// est le seul des deux dont l'`Attach` honorait le drapeau.
//
// Les tests suivants entrent par où la production entre : une source, un
// `Attach`, un paquet publié PAR LA SOURCE.

class FakeSource : public Joinable
{
public:
	void AddListener(Joinable::Listener *listener) override
	{
		listeners.push_back(listener);
	}

	void RemoveListener(Joinable::Listener *listener) override
	{
		listeners.erase(std::remove(listeners.begin(), listeners.end(), listener),
		                listeners.end());
	}

	void Update() override {}
	void SetREMB(DWORD estimation) override {}

	// Ce que fait RTPMultiplexer::Multiplex côté production.
	void Publish(RTPPacket &packet)
	{
		for (Joinable::Listener *l : listeners)
			l->onRTPPacket(packet);
	}

	bool HasListener(Joinable::Listener *listener) const
	{
		return std::find(listeners.begin(), listeners.end(), listener) != listeners.end();
	}

	size_t ListenerCount() const { return listeners.size(); }

private:
	std::vector<Joinable::Listener *> listeners;
};

TEST_F(VideoBridgingTest, AttachPutsTheTranscoderOnThePathSoBridgingCanHappen)
{
	VideoTranscoder transcoder(name);
	ASSERT_EQ(1, transcoder.Init(false, /*allowBridging=*/true));

	RecordingSink sink({ VideoCodec::VP8 });
	transcoder.AddListener(&sink);

	auto source = std::make_shared<FakeSource>();
	ASSERT_EQ(1, transcoder.Attach(source));

	// LE test de régression : c'est le transcodeur qui doit écouter la source, pas
	// son décodeur. Sans cela l'arbitrage est inatteignable.
	EXPECT_TRUE(source->HasListener(&transcoder))
		<< "en mode pont, la source doit publier dans le transcodeur : c'est lui "
		   "qui arbitre relais ou transcodage sur le codec recu";

	RTPPacket packet = MakeVideoPacket(VideoCodec::VP8, 42, 90000);
	source->Publish(packet);

	ASSERT_EQ(1u, sink.received.size())
		<< "un paquet publie par la SOURCE doit arriver relaye au puits";
	ASSERT_EQ(sizeof(kMagic), sink.received[0].length);
	EXPECT_EQ(0, memcmp(kMagic, sink.received[0].payload.data(), sizeof(kMagic)));

	transcoder.RemoveListener(&sink);
	transcoder.End();
}

TEST_F(VideoBridgingTest, AttachWiresTheDecoderWhenBridgingIsNotAllowed)
{
	// Le comportement historique reste intact là où le pont n'est pas demandé
	// (mixage, enregistrement) : c'est le décodeur qui écoute la source.
	VideoTranscoder transcoder(name);
	ASSERT_EQ(1, transcoder.Init(false, /*allowBridging=*/false));

	RecordingSink sink({ VideoCodec::VP8 });
	transcoder.AddListener(&sink);

	auto source = std::make_shared<FakeSource>();
	ASSERT_EQ(1, transcoder.Attach(source));

	EXPECT_FALSE(source->HasListener(&transcoder));
	EXPECT_EQ(1u, source->ListenerCount()) << "le decodeur doit etre l'ecouteur";

	RTPPacket packet = MakeVideoPacket(VideoCodec::VP8, 1, 90000);
	source->Publish(packet);

	EXPECT_TRUE(sink.received.empty())
		<< "pont non autorise : meme un codec que le puits porte doit etre decode";

	transcoder.RemoveListener(&sink);
	transcoder.End();
}

TEST_F(VideoBridgingTest, DettachTakesTheTranscoderOffTheSource)
{
	VideoTranscoder transcoder(name);
	ASSERT_EQ(1, transcoder.Init(false, /*allowBridging=*/true));

	RecordingSink sink({ VideoCodec::VP8 });
	transcoder.AddListener(&sink);

	auto source = std::make_shared<FakeSource>();
	ASSERT_EQ(1, transcoder.Attach(source));
	ASSERT_TRUE(source->HasListener(&transcoder));

	ASSERT_EQ(1, transcoder.Dettach());

	EXPECT_FALSE(source->HasListener(&transcoder))
		<< "une source qui garde le pointeur publie dans un objet detache";
	EXPECT_EQ(0u, source->ListenerCount());

	RTPPacket packet = MakeVideoPacket(VideoCodec::VP8, 2, 90000);
	source->Publish(packet);
	EXPECT_TRUE(sink.received.empty());

	transcoder.RemoveListener(&sink);
	transcoder.End();
}

TEST_F(VideoBridgingTest, EndAlsoTakesTheTranscoderOffTheSource)
{
	// Sûreté mémoire, pas hygiène : `MediaSession::VideoTranscoderDelete` appelle
	// End() SANS passer par Dettach(), puis le dernier shared_ptr détruit l'objet
	// en sortie de portée. Une source qui garderait le Joinable::Listener*
	// publierait ensuite dans de la mémoire libérée. L'ordre des appels du
	// contrôleur ne doit pas décider de ça.
	VideoTranscoder transcoder(name);
	ASSERT_EQ(1, transcoder.Init(false, /*allowBridging=*/true));

	auto source = std::make_shared<FakeSource>();
	ASSERT_EQ(1, transcoder.Attach(source));
	ASSERT_TRUE(source->HasListener(&transcoder));

	transcoder.End();

	EXPECT_FALSE(source->HasListener(&transcoder));
}

TEST_F(VideoBridgingTest, ReattachingMovesTheTranscoderToTheNewSource)
{
	VideoTranscoder transcoder(name);
	ASSERT_EQ(1, transcoder.Init(false, /*allowBridging=*/true));

	RecordingSink sink({ VideoCodec::VP8 });
	transcoder.AddListener(&sink);

	auto first  = std::make_shared<FakeSource>();
	auto second = std::make_shared<FakeSource>();

	ASSERT_EQ(1, transcoder.Attach(first));
	ASSERT_EQ(1, transcoder.Attach(second));

	EXPECT_FALSE(first->HasListener(&transcoder))
		<< "l'ancienne source ne doit plus publier dans le transcodeur";
	EXPECT_TRUE(second->HasListener(&transcoder));

	// Une seule copie du paquet, pas deux : c'est ce que garantit le retrait.
	RTPPacket packet = MakeVideoPacket(VideoCodec::VP8, 3, 90000);
	second->Publish(packet);
	EXPECT_EQ(1u, sink.received.size());

	transcoder.RemoveListener(&sink);
	transcoder.End();
}

// ── Parité audio ────────────────────────────────────────────────────────────
//
// `AudioTranscoder` porte ce mode pont depuis longtemps et tournait sans test :
// c'est le modèle sur lequel la version vidéo est calquée, et le comportement
// qu'un appel opus↔opus dépend de. Le verrouiller ici évite que la parité se
// perde dans un sens ou dans l'autre.

RTPPacket MakeAudioPacket(AudioCodec::Type codec, WORD seq, DWORD timestamp)
{
	RTPPacket packet(MediaFrame::Audio, codec);
	packet.SetSeqNum(seq);
	packet.SetTimestamp(timestamp);
	memcpy(packet.GetMediaData(), kMagic, sizeof(kMagic));
	packet.SetMediaLength(sizeof(kMagic));
	return packet;
}

TEST(AudioBridgingTest, RelaysUntouchedWhenTheSinkCarriesTheIncomingCodec)
{
	std::wstring name = L"tr-audio";
	AudioTranscoder transcoder(name);
	ASSERT_EQ(1, transcoder.Init(/*allowBriding=*/true));

	RecordingSink sink({ AudioCodec::OPUS });
	transcoder.AddListener(&sink);

	RTPPacket packet = MakeAudioPacket(AudioCodec::OPUS, 11, 48000);
	transcoder.onRTPPacket(packet);

	ASSERT_EQ(1u, sink.received.size())
		<< "le puits porte OPUS : opus vers opus ne doit pas etre transcode";
	EXPECT_EQ((int)AudioCodec::OPUS, sink.received[0].codec);
	ASSERT_EQ(sizeof(kMagic), sink.received[0].length);
	EXPECT_EQ(0, memcmp(kMagic, sink.received[0].payload.data(), sizeof(kMagic)));

	transcoder.RemoveListener(&sink);
	transcoder.End();
}

TEST(AudioBridgingTest, DecodesWhenTheSinkCannotCarryTheIncomingCodec)
{
	std::wstring name = L"tr-audio";
	AudioTranscoder transcoder(name);
	ASSERT_EQ(1, transcoder.Init(/*allowBriding=*/true));

	// Le puits ne porte que PCMU ; il arrive de l'opus. C'est le seul cas où un
	// transcodeur audio a quelque chose à faire.
	RecordingSink sink({ AudioCodec::PCMU });
	transcoder.AddListener(&sink);

	RTPPacket packet = MakeAudioPacket(AudioCodec::OPUS, 12, 48000);
	transcoder.onRTPPacket(packet);

	EXPECT_TRUE(sink.received.empty())
		<< "codec non portable par le puits : le paquet doit passer par le decodeur";

	transcoder.RemoveListener(&sink);
	transcoder.End();
}

TEST(AudioBridgingTest, WithoutTheFlagEverythingIsDecodedAsBefore)
{
	std::wstring name = L"tr-audio";
	AudioTranscoder transcoder(name);
	ASSERT_EQ(1, transcoder.Init(/*allowBriding=*/false));

	RecordingSink sink({ AudioCodec::OPUS });
	transcoder.AddListener(&sink);

	RTPPacket packet = MakeAudioPacket(AudioCodec::OPUS, 13, 48000);
	transcoder.onRTPPacket(packet);

	EXPECT_TRUE(sink.received.empty())
		<< "pont non autorise : meme un codec que le puits porte doit etre decode";

	transcoder.RemoveListener(&sink);
	transcoder.End();
}

}  // namespace
