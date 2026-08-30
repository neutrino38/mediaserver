/**
 * test_transcoder_characterization.cpp — ce qu'un transcodeur JSR309 SORT,
 * aujourd'hui, pour une séquence de paquets donnée.
 *
 * Lot 0 de `jsr309_transcode_sans_thread.md` : ces tests sont écrits AVANT que
 * les décodeurs et l'encodeur audio perdent leur thread. Ils ne décrivent
 * aucune cible, ils photographient l'existant — nombre de paquets, code codec,
 * SSRC unique par run d'encodage, pas d'horodatage, demande de FPU sur perte.
 * Les lots 1 à 4 doivent rendre EXACTEMENT le même résultat ; c'est leur seule
 * raison d'être.
 *
 * Une réserve, assumée : la duplication d'images d'un encodeur vidéo cadencé
 * disparaît au lot 4 (§3.3, arbitré). Aucun test ici ne compte les images de
 * sortie de la vidéo : ce serait figer ce qu'on a décidé de changer.
 *
 * Ce qui est observé, ce sont les paquets qui atteignent le puits. Comme les
 * workers portent encore un thread, chaque attente est BORNÉE et jamais fixe :
 * après les lots 1 à 4 la sortie sera là au retour de `onRTPPacket`, et ces
 * mêmes attentes rendront la main aussitôt.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "rtp.h"
#include "medkit/video.h"
#include "../src/jsr309/AudioTranscoder.h"
#include "../src/jsr309/VideoTranscoder.h"
#include "../src/jsr309/RTPMultiplexer.h"

namespace {

typedef std::chrono::milliseconds Ms;

// Puits instrumenté. Il n'accepte aucun codec en relais : le transcodeur est de
// toute façon créé sans le mode pont, on veut le chemin décodage/encodage.
class CharacterizingSink : public Joinable::Listener
{
public:
	struct Received
	{
		int	codec;
		DWORD	ssrc;
		DWORD	timestamp;
		DWORD	length;
		bool	mark;
	};

	void onRTPPacket(RTPPacket &packet) override
	{
		Received got;
		got.codec	= packet.GetCodec();
		got.ssrc	= packet.GetSSRC();
		got.timestamp	= packet.GetTimestamp();
		got.length	= packet.GetMediaLength();
		got.mark	= packet.GetMark();

		std::lock_guard<std::mutex> lock(mutex);
		received.push_back(got);
	}

	void onResetStream() override {}
	void onEndStream() override {}
	int TryCheckCodec(int codec) override { return -1; }

	size_t Count()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return received.size();
	}

	std::vector<Received> Snapshot()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return received;
	}

	// Attend que la sortie se stabilise : plus rien de neuf pendant `quietMs`,
	// ou `capMs` écoulées. Rend le compte final.
	size_t WaitSettled(int quietMs = 300, int capMs = 5000)
	{
		size_t last = Count();
		int quiet = 0;
		for (int waited = 0; waited < capMs; waited += 25)
		{
			std::this_thread::sleep_for(Ms(25));
			size_t now = Count();
			if (now != last)
			{
				last = now;
				quiet = 0;
				continue;
			}
			quiet += 25;
			if (now > 0 && quiet >= quietMs)
				break;
		}
		return last;
	}

	void Reset()
	{
		std::lock_guard<std::mutex> lock(mutex);
		received.clear();
	}

	std::set<DWORD> DistinctSsrc()
	{
		std::set<DWORD> ssrcs;
		for (const Received& r : Snapshot())
			ssrcs.insert(r.ssrc);
		return ssrcs;
	}

private:
	std::mutex		mutex;
	std::vector<Received>	received;
};

// Source réelle : c'est un RTPMultiplexer, donc `Multiplex` tient son verrou
// pendant tout l'appel aux listeners — la barrière sur laquelle le chantier
// s'appuie. `Update()` compte les demandes d'intra remontées par le décodeur.
class CountingSource : public RTPMultiplexer
{
public:
	void Update() override { updates++; }

	std::atomic<int> updates { 0 };
};

// ── Audio : PCMU (8 kHz, 20 ms) vers Opus ───────────────────────────────────

// Silence PCMU : 0xFF est le zéro de la loi µ.
RTPPacket MakePcmuPacket(WORD seq, DWORD timestamp)
{
	RTPPacket packet(MediaFrame::Audio, AudioCodec::PCMU);
	packet.SetSeqNum(seq);
	packet.SetTimestamp(timestamp);
	memset(packet.GetMediaData(), 0xFF, 160);
	packet.SetMediaLength(160);
	return packet;
}

// L'encodeur ouvre le pipe en enregistrement sur SON thread ; tant qu'il ne
// l'a pas fait, PipeAudioInput::PutFrame jette ce que le décodeur lui donne.
// C'est l'établissement réel d'un appel, pas un artefact de test : on amorce
// jusqu'au premier paquet encodé, puis on remet le compteur à zéro et c'est
// SEULEMENT ensuite qu'on caractérise.
static bool WarmUpAudio(CountingSource& source, CharacterizingSink& sink, WORD& seq)
{
	for (int n = 0; n < 50 && sink.Count() == 0; ++n)
	{
		RTPPacket packet = MakePcmuPacket(seq, (DWORD)seq * 160);
		seq++;
		source.Multiplex(packet);
		std::this_thread::sleep_for(Ms(20));
	}
	return sink.Count() > 0;
}

TEST(TranscoderCharacterization, PcmuToOpusOutput)
{
	// 20 paquets = 400 ms, sous les 500 ms de PipeAudioInput::MaxQueuedMs : au
	// delà, la file se vide plutôt que de bloquer et le compte n'aurait plus
	// de sens.
	const WORD kPackets = 20;

	std::wstring name = L"carac-audio";
	AudioTranscoder transcoder(name);
	ASSERT_EQ(1, transcoder.Init(/*allowBriding=*/false));
	ASSERT_EQ(1, transcoder.SetCodec(AudioCodec::OPUS));

	CharacterizingSink sink;
	transcoder.AddListener(&sink);

	//Entrer par où la production entre : c'est `Attach` qui démarre le
	//décodeur. Sans lui, les paquets s'empilent dans une file que personne ne
	//dépile.
	auto source = std::make_shared<CountingSource>();
	ASSERT_EQ(1, transcoder.Attach(source));

	WORD seq = 1;
	ASSERT_TRUE(WarmUpAudio(*source, sink, seq)) << "l'encodeur n'a jamais demarre";
	sink.Reset();

	//Flux cadencé : 20 ms par paquet, comme sur le fil. Envoyer plus vite
	//remplirait les 500 ms de PipeAudioInput::MaxQueuedMs, qui se vide plutôt
	//que de bloquer — et le compte n'aurait plus de sens.
	for (WORD n = 0; n < kPackets; ++n, ++seq)
	{
		RTPPacket packet = MakePcmuPacket(seq, (DWORD)seq * 160);
		source->Multiplex(packet);
		std::this_thread::sleep_for(Ms(20));
	}

	size_t produced = sink.WaitSettled();
	std::vector<CharacterizingSink::Received> got = sink.Snapshot();

	// 20 ms entrent, 20 ms sortent : la fifo d'accumulation de FfAudioEncoder
	// ne retient rien de plus qu'une trame partielle.
	EXPECT_GE(produced, (size_t)(kPackets - 1))
		<< "une trame de 20 ms en PCMU vaut une trame de 20 ms en Opus";
	EXPECT_LE(produced, (size_t)kPackets);

	ASSERT_FALSE(got.empty());
	for (const CharacterizingSink::Received& r : got)
	{
		EXPECT_EQ((int)AudioCodec::OPUS, r.codec);
		EXPECT_GT(r.length, (DWORD)0);
	}

	// Un run d'encodage = un SSRC (RFC 3550 : nouvelle base de temps, nouvelle
	// identité de source). C'est ce que le lot 3 doit préserver.
	EXPECT_EQ((size_t)1, sink.DistinctSsrc().size())
		<< "un seul run d'encodage : un seul SSRC";

	// L'horloge n'avance que sur ce qui est réellement émis, d'un pas constant.
	if (got.size() >= 3)
	{
		DWORD step = got[1].timestamp - got[0].timestamp;
		EXPECT_GT(step, (DWORD)0);
		for (size_t i = 2; i < got.size(); ++i)
			EXPECT_EQ(step, got[i].timestamp - got[i-1].timestamp)
				<< "pas d'horodatage irregulier a l'indice " << i;
	}

	transcoder.Dettach();
	transcoder.RemoveListener(&sink);
	transcoder.End();
}

// Deux runs d'encodage successifs (le second ouvert par un SetCodec) ne
// partagent pas leur SSRC : c'est ce qui permet au pair de resynchroniser sa
// base de temps.
TEST(TranscoderCharacterization, EachAudioEncodingRunGetsItsOwnSsrc)
{
	std::wstring name = L"carac-audio-ssrc";
	AudioTranscoder transcoder(name);
	ASSERT_EQ(1, transcoder.Init(/*allowBriding=*/false));
	ASSERT_EQ(1, transcoder.SetCodec(AudioCodec::OPUS));

	CharacterizingSink sink;
	transcoder.AddListener(&sink);

	auto source = std::make_shared<CountingSource>();
	ASSERT_EQ(1, transcoder.Attach(source));

	WORD seq = 1;
	ASSERT_TRUE(WarmUpAudio(*source, sink, seq)) << "l'encodeur n'a jamais demarre";

	for (WORD n = 0; n < 5; ++n, ++seq)
	{
		RTPPacket packet = MakePcmuPacket(seq, (DWORD)seq * 160);
		source->Multiplex(packet);
		std::this_thread::sleep_for(Ms(20));
	}
	ASSERT_GT(sink.WaitSettled(), (size_t)0);
	std::set<DWORD> first = sink.DistinctSsrc();
	ASSERT_EQ((size_t)1, first.size());

	// Renégociation : nouvel encodeur, donc nouveau run
	ASSERT_EQ(1, transcoder.SetCodec(AudioCodec::PCMA));
	for (WORD n = 0; n < 10; ++n, ++seq)
	{
		RTPPacket packet = MakePcmuPacket(seq, (DWORD)seq * 160);
		source->Multiplex(packet);
		std::this_thread::sleep_for(Ms(20));
	}
	sink.WaitSettled();

	std::set<DWORD> all = sink.DistinctSsrc();
	EXPECT_EQ((size_t)2, all.size())
		<< "un nouveau run d'encodage annonce une nouvelle base de temps par un SSRC neuf";

	transcoder.Dettach();
	transcoder.RemoveListener(&sink);
	transcoder.End();
}

// ── Vidéo : VP8 vers H.264 ──────────────────────────────────────────────────

// Fabrique des paquets RTP VP8 à partir d'une image de synthèse, en reprenant
// la paquetisation de RTPMultiplexerSmoother::SmoothFrame : pas de fixture à
// versionner, et le flux est un vrai flux VP8 que le décodeur saura lire.
class Vp8Source
{
public:
	bool Open(int width, int height, int fps, int kbits)
	{
		Properties none;
		encoder = VideoCodecFactory::CreateEncoder(VideoCodec::VP8, none);
		if (!encoder)
			return false;
		encoder->SetFrameRate(fps, kbits, 1);	// intra à chaque image
		encoder->SetSize(width, height);
		this->width = width;
		this->height = height;
		return true;
	}

	~Vp8Source() { delete encoder; }

	// Livre les paquets d'une image un par un, seq/ts continus, SANS jamais en
	// copier : `RTPPacket` porte un `header` qui pointe dans son propre
	// `buffer` et n'a pas de constructeur de copie — une copie garde le
	// pointeur de l'original, et le lire après la mort de l'original est un
	// accès à de la mémoire libérée. Le bit de marque s'y perdait, donc aucune
	// image n'était jamais complète pour le décodeur. D'où le rappel plutôt
	// qu'un `std::vector<RTPPacket>`.
	template <typename Sink>
	int NextFrame(BYTE luma, DWORD timestamp, Sink sink)
	{
		PictPtr pic = Pict::CreateColor(width, height, luma, 128, 128);
		if (!pic)
			return 0;

		VideoFramePtr frame = encoder->EncodeFrame(pic);
		if (!frame || !frame->HasRtpPacketizationInfo())
			return 0;

		MediaFrame::RtpPacketizationInfo& info = frame->GetRtpPacketizationInfo();
		int sent = 0;
		for (int i = 0; i < (int)info.size(); ++i)
		{
			MediaFrame::RtpPacketization* rtp = info[i];

			RTPPacket packet(MediaFrame::Video, VideoCodec::VP8);
			if (rtp->GetPrefixLen() + rtp->GetSize() > packet.GetMaxMediaLength())
				continue;

			BYTE* dst = packet.GetMediaData();
			memcpy(dst, rtp->GetPrefixData(), rtp->GetPrefixLen());
			memcpy(dst + rtp->GetPrefixLen(), frame->GetData() + rtp->GetPos(), rtp->GetSize());
			packet.SetMediaLength(rtp->GetPrefixLen() + rtp->GetSize());
			packet.SetSeqNum(seq++);
			packet.SetTimestamp(timestamp);
			packet.SetMark(i + 1 == (int)info.size());
			sink(packet);
			sent++;
		}
		return sent;
	}

	WORD NextSeq() const { return seq; }
	void SkipSeq(WORD howMany) { seq += howMany; }

private:
	VideoEncoder*	encoder = NULL;
	int		width = 0;
	int		height = 0;
	WORD		seq = 1;
};

TEST(TranscoderCharacterization, Vp8ToH264Output)
{
	Vp8Source vp8;
	if (!vp8.Open(GetWidth(CIF), GetHeight(CIF), 10, 256))
		GTEST_SKIP() << "encodeur VP8 indisponible : rien a caracteriser";

	std::wstring name = L"carac-video";
	VideoTranscoder transcoder(name);
	ASSERT_EQ(1, transcoder.Init(/*adaptative=*/false, /*allowBridging=*/false));

	Properties props;
	ASSERT_EQ(1, transcoder.SetCodec(VideoCodec::H264, CIF, 10, 256, 10, props));

	CharacterizingSink sink;
	transcoder.AddListener(&sink);

	auto source = std::make_shared<CountingSource>();
	ASSERT_EQ(1, transcoder.Attach(source));

	// Une seconde d'images : de quoi laisser l'encodeur H.264 s'ouvrir et
	// produire. Le NOMBRE d'images sortantes n'est pas figé ici (§3.3).
	for (int n = 0; n < 10; ++n)
	{
		int sent = vp8.NextFrame((BYTE)(16 + n * 8), (DWORD)n * 9000,
					 [&](RTPPacket& p){ source->Multiplex(p); });
		ASSERT_GT(sent, 0) << "l'encodeur VP8 n'a rien produit a l'image " << n;
		std::this_thread::sleep_for(Ms(100));
	}

	ASSERT_GT(sink.WaitSettled(), (size_t)0)
		<< "un flux VP8 decodable doit ressortir en H.264";

	std::vector<CharacterizingSink::Received> got = sink.Snapshot();
	for (const CharacterizingSink::Received& r : got)
	{
		EXPECT_EQ((int)VideoCodec::H264, r.codec);
		EXPECT_GT(r.length, (DWORD)0);
	}

	EXPECT_EQ((size_t)1, sink.DistinctSsrc().size())
		<< "un seul run d'encodage : un seul SSRC";

	// Une image se termine par un paquet marqué : la paquetisation de sortie
	// reste une paquetisation d'images, pas un flot.
	size_t marked = 0;
	for (const CharacterizingSink::Received& r : got)
		if (r.mark) marked++;
	EXPECT_GT(marked, (size_t)0) << "aucune fin d'image marquee";

	transcoder.Dettach();
	transcoder.RemoveListener(&sink);
	transcoder.End();
}

// La perte d'un paquet vidéo fait remonter une demande d'intra à la SOURCE.
// C'est le seul retour amont du décodeur, et le lot 2 le déplace sous le verrou
// du port : il doit rester.
TEST(TranscoderCharacterization, VideoLossRequestsAnIntraFromTheSource)
{
	Vp8Source vp8;
	if (!vp8.Open(GetWidth(CIF), GetHeight(CIF), 10, 256))
		GTEST_SKIP() << "encodeur VP8 indisponible : rien a caracteriser";

	std::wstring name = L"carac-video-fpu";
	VideoTranscoder transcoder(name);
	ASSERT_EQ(1, transcoder.Init(/*adaptative=*/false, /*allowBridging=*/false));

	Properties props;
	ASSERT_EQ(1, transcoder.SetCodec(VideoCodec::H264, CIF, 10, 256, 10, props));

	CharacterizingSink sink;
	transcoder.AddListener(&sink);

	auto source = std::make_shared<CountingSource>();
	ASSERT_EQ(1, transcoder.Attach(source));

	// Première image complète : elle pose `lastSeq`, sans quoi rien n'est perdu.
	ASSERT_GT(vp8.NextFrame(16, 0, [&](RTPPacket& p){ source->Multiplex(p); }), 0);

	// Trou franc dans la numérotation : plus d'un paquet manquant.
	vp8.SkipSeq(10);
	ASSERT_GT(vp8.NextFrame(200, 9000, [&](RTPPacket& p){ source->Multiplex(p); }), 0);

	// Le worker porte encore un thread : borner l'attente, pas la fixer.
	for (int waited = 0; waited < 3000 && source->updates == 0; waited += 25)
		std::this_thread::sleep_for(Ms(25));

	EXPECT_GT(source->updates.load(), 0)
		<< "une perte doit remonter une demande d'intra a la source";

	transcoder.Dettach();
	transcoder.RemoveListener(&sink);
	transcoder.End();
}

}  // namespace
