/**
 * test_video_encoder_inline.cpp — l'encodeur vidéo JSR309 encode sur le thread
 * de la source, et suit la cadence RÉELLE de cette source.
 *
 * Lot 4 et lot 4 bis de `jsr309_transcode_sans_thread.md`. Deux propriétés
 * nouvelles :
 *
 *  1. dans un transcodeur, plus de `VideoPipe` ni de thread d'encodage : la
 *     sortie est là au retour de `Multiplex` (§3.3). Le port de MIXEUR, lui,
 *     garde sa boucle cadencée — rien ici ne l'exerce ;
 *  2. la cadence de l'encodeur suit celle de la source, mesurée sur les
 *     horodatages RTP (§3.6). Sans cela, une source à 15 im/s dans un encodeur
 *     ouvert à 30 sortirait à la moitié du débit négocié.
 *
 * Piège d'écriture (trouvé au lot 2) : ne JAMAIS copier un `RTPPacket`. Il
 * porte un `header` qui pointe dans son propre `buffer` et n'a pas de
 * constructeur de copie — d'où les sources par rappel.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <dirent.h>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

#include "rtp.h"
#include "medkit/video.h"
#include "../src/jsr309/VideoTranscoder.h"
#include "../src/jsr309/RTPMultiplexer.h"

namespace {

typedef std::chrono::milliseconds Ms;

class CountingSink : public Joinable::Listener
{
public:
	void onRTPPacket(RTPPacket &packet) override
	{
		std::lock_guard<std::mutex> lock(mutex);
		count++;
		codecs.insert(packet.GetCodec());
	}

	void onResetStream() override {}
	void onEndStream() override {}
	int TryCheckCodec(int codec) override { return -1; }

	int Count()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return count;
	}

	std::set<int> Codecs()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return codecs;
	}

private:
	std::mutex	mutex;
	int		count = 0;
	std::set<int>	codecs;
};

// Source VP8 de synthèse : un vrai flux, paquetisé comme le fait
// RTPMultiplexerSmoother, et dont l'appelant maîtrise l'HORODATAGE RTP — c'est
// lui, et non l'heure d'arrivée, qui porte la cadence mesurée (§3.6).
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

	template <typename Sink>
	int NextFrame(BYTE luma, DWORD timestamp, Sink sink)
	{
		PictPtr pic = Pict::CreateColor(width, height, luma, 128, 128);
		if (!pic)
			return 0;

		VideoFrame* frame = encoder->EncodeFrame(pic);
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

private:
	VideoEncoder*	encoder = NULL;
	int		width = 0;
	int		height = 0;
	WORD		seq = 1;
};

int CountThreads()
{
	DIR* dir = opendir("/proc/self/task");
	if (!dir)
		return -1;

	int count = 0;
	while (struct dirent* entry = readdir(dir))
		if (entry->d_name[0] != '.')
			count++;

	closedir(dir);
	return count;
}

// Nourrit `frames` images à la cadence `fps` (horodatage RTP), en respectant
// aussi cette cadence en temps réel : la borne de sortie du transcodeur est une
// vraie horloge. Rend le nombre d'images livrées.
int FeedAt(Vp8Source& vp8, RTPMultiplexer& source, int frames, int fps,
	   DWORD& timestamp, int startLuma = 16)
{
	const DWORD step = 90000 / (DWORD)fps;
	const int sleepMs = 1000 / fps;
	int delivered = 0;
	for (int n = 0; n < frames; ++n)
	{
		int sent = vp8.NextFrame((BYTE)(startLuma + (n * 7) % 200), timestamp,
					 [&](RTPPacket& p){ source.Multiplex(p); });
		if (sent > 0)
			delivered++;
		timestamp += step;
		std::this_thread::sleep_for(Ms(sleepMs));
	}
	return delivered;
}

// ── Lot 4 : plus de thread, plus de file ────────────────────────────────────

// L'image traverse décodage, redimensionnement, encodage et lissage sur le
// thread qui a livré le paquet. Le lisseur reste : c'est un pacer, il mesure du
// temps — d'où la seule attente bornée de ce fichier.
TEST(VideoEncoderInline, LImageEstEncodeeAuRetourDeMultiplex)
{
	Vp8Source vp8;
	if (!vp8.Open(GetWidth(CIF), GetHeight(CIF), 10, 256))
		GTEST_SKIP() << "encodeur VP8 indisponible";

	std::wstring name = L"inline-video";
	VideoTranscoder transcoder(name);
	ASSERT_EQ(1, transcoder.Init(/*adaptative=*/false, /*allowBridging=*/false));

	Properties props;
	ASSERT_EQ(1, transcoder.SetCodec(VideoCodec::H264, CIF, 10, 256, 10, props));

	CountingSink sink;
	transcoder.AddListener(&sink);

	auto source = std::make_shared<RTPMultiplexer>();
	ASSERT_EQ(1, transcoder.Attach(source));

	DWORD timestamp = 0;
	ASSERT_GT(FeedAt(vp8, *source, 5, 10, timestamp), 0);

	// Seule attente du test : l'étalement du lisseur, borné par MaxAheadUs.
	for (int waited = 0; waited < 2000 && sink.Count() == 0; waited += 25)
		std::this_thread::sleep_for(Ms(25));

	ASSERT_GT(sink.Count(), 0) << "un flux VP8 decodable doit ressortir en H.264";
	std::set<int> codecs = sink.Codecs();
	ASSERT_EQ((size_t)1, codecs.size());
	EXPECT_EQ((int)VideoCodec::H264, *codecs.begin());

	transcoder.Dettach();
	transcoder.RemoveListener(&sink);
	transcoder.End();
}

// Il ne reste qu'UN thread dans un transcodeur vidéo : le lisseur
// (RTPMultiplexerSmoother), qui étale les paquets d'une image dans le temps.
// Le décodeur (lot 2) et la boucle d'encodage (lot 4) ont disparu. Compté avant
// la première image, donc avant que x264 n'existe et ne crée les siens.
TEST(VideoEncoderInline, IlNeResteQueLeThreadDuLisseur)
{
	std::wstring name = L"inline-video-threads";
	VideoTranscoder transcoder(name);
	ASSERT_EQ(1, transcoder.Init(/*adaptative=*/false, /*allowBridging=*/false));

	const int before = CountThreads();
	ASSERT_GT(before, 0);

	Properties props;
	ASSERT_EQ(1, transcoder.SetCodec(VideoCodec::H264, CIF, 10, 256, 10, props));

	CountingSink sink;
	transcoder.AddListener(&sink);

	auto source = std::make_shared<RTPMultiplexer>();
	ASSERT_EQ(1, transcoder.Attach(source));

	EXPECT_EQ(before + 1, CountThreads())
		<< "un seul thread doit rester dans un transcodeur video : le lisseur";

	transcoder.Dettach();
	transcoder.RemoveListener(&sink);
	transcoder.End();
}

// `Dettach` puis `RemoveListener` détruisent décodeur et encodeur. Ils ne
// peuvent le faire qu'une fois le `Multiplex` en cours terminé (§4.3).
// À jouer aussi sous ASan/TSan.
TEST(VideoEncoderInline, DettachConcurrentNeLibereRienSousLImage)
{
	Vp8Source vp8;
	if (!vp8.Open(GetWidth(CIF), GetHeight(CIF), 10, 256))
		GTEST_SKIP() << "encodeur VP8 indisponible";

	std::wstring name = L"inline-video-race";
	VideoTranscoder transcoder(name);
	ASSERT_EQ(1, transcoder.Init(/*adaptative=*/false, /*allowBridging=*/false));

	Properties props;
	ASSERT_EQ(1, transcoder.SetCodec(VideoCodec::H264, CIF, 10, 256, 10, props));

	CountingSink sink;
	transcoder.AddListener(&sink);

	auto source = std::make_shared<RTPMultiplexer>();
	ASSERT_EQ(1, transcoder.Attach(source));

	std::atomic<bool> stop { false };
	std::atomic<int> pushed { 0 };
	DWORD timestamp = 0;
	std::thread producer([&]{
		while (!stop.load())
		{
			vp8.NextFrame((BYTE)(16 + (pushed.load() * 7) % 200), timestamp,
				      [&](RTPPacket& p){ source->Multiplex(p); });
			timestamp += 9000;
			pushed++;
		}
	});

	while (pushed.load() < 5)
		std::this_thread::yield();

	transcoder.Dettach();
	transcoder.RemoveListener(&sink);

	// Le lisseur est arrêté par RemoveListener : sa file ne se vide plus.
	const int atDettach = sink.Count();
	std::this_thread::sleep_for(Ms(200));
	EXPECT_EQ(atDettach, sink.Count())
		<< "RemoveListener est la barriere : plus rien ne doit passer apres";

	stop = true;
	producer.join();

	transcoder.End();
}

// ── Lot 4 bis : la cadence réelle de la source (§3.6) ───────────────────────

// Une source à 15 im/s dans un encodeur négocié à 30 : après une fenêtre pleine
// (30 écarts, soit 2 s), l'encodeur est rouvert à 15 im/s. Sans cela il donne
// un budget de bitrate/30 par image et sort à la moitié du débit négocié.
// La période intra suit, pour rester CONSTANTE EN SECONDES : 300 images à 30
// im/s valent 150 images à 15 im/s.
TEST(VideoEncoderInline, UneSourceLenteAbaisseLaCadenceDeLEncodeur)
{
	Vp8Source vp8;
	if (!vp8.Open(GetWidth(CIF), GetHeight(CIF), 15, 256))
		GTEST_SKIP() << "encodeur VP8 indisponible";

	std::wstring name = L"fps-source-lente";
	VideoTranscoder transcoder(name);
	ASSERT_EQ(1, transcoder.Init(/*adaptative=*/false, /*allowBridging=*/false));

	Properties props;
	ASSERT_EQ(1, transcoder.SetCodec(VideoCodec::H264, CIF, 30, 256, 300, props));

	CountingSink sink;
	transcoder.AddListener(&sink);

	auto source = std::make_shared<RTPMultiplexer>();
	ASSERT_EQ(1, transcoder.Attach(source));

	DWORD timestamp = 0;
	// 45 images à 15 im/s : la fenêtre (30 écarts) se remplit au bout de 31, la
	// marge couvre une image qui ne se décoderait pas.
	ASSERT_GT(FeedAt(vp8, *source, 45, 15, timestamp), 30);

	EXPECT_EQ(15, transcoder.GetEffectiveFps())
		<< "l'encodeur doit suivre la cadence reelle de la source";
	EXPECT_EQ(150, transcoder.GetEffectiveIntraPeriod())
		<< "la periode intra doit rester constante EN SECONDES";

	transcoder.Dettach();
	transcoder.RemoveListener(&sink);
	transcoder.End();
}

// Hystérésis : moins de 25 % d'écart ne coûte pas de trame clé. 28 im/s pour une
// consigne de 30, c'est le rendu irrégulier d'un navigateur, pas une bascule.
TEST(VideoEncoderInline, UneSourceA28ImagesNeChangeRien)
{
	Vp8Source vp8;
	if (!vp8.Open(GetWidth(CIF), GetHeight(CIF), 28, 256))
		GTEST_SKIP() << "encodeur VP8 indisponible";

	std::wstring name = L"fps-hysteresis";
	VideoTranscoder transcoder(name);
	ASSERT_EQ(1, transcoder.Init(/*adaptative=*/false, /*allowBridging=*/false));

	Properties props;
	ASSERT_EQ(1, transcoder.SetCodec(VideoCodec::H264, CIF, 30, 256, 300, props));

	CountingSink sink;
	transcoder.AddListener(&sink);

	auto source = std::make_shared<RTPMultiplexer>();
	ASSERT_EQ(1, transcoder.Attach(source));

	DWORD timestamp = 0;
	ASSERT_GT(FeedAt(vp8, *source, 40, 28, timestamp), 20);

	EXPECT_EQ(30, transcoder.GetEffectiveFps())
		<< "un ecart de 7 % ne doit rien declencher";
	EXPECT_EQ(300, transcoder.GetEffectiveIntraPeriod());

	transcoder.Dettach();
	transcoder.RemoveListener(&sink);
	transcoder.End();
}

// Une PAUSE n'est pas une cadence. Cinq secondes sans image, puis reprise à la
// même cadence : ni changement de cadence, ni trame clé de réouverture. Sans
// cette règle, l'écart de pause tomberait dans la moyenne, l'encodeur rouvrirait
// à 1 im/s, et la reprise sortirait à une image par seconde.
TEST(VideoEncoderInline, UnePauseNeFaitPasTomberLaCadence)
{
	Vp8Source vp8;
	if (!vp8.Open(GetWidth(CIF), GetHeight(CIF), 30, 256))
		GTEST_SKIP() << "encodeur VP8 indisponible";

	std::wstring name = L"fps-pause";
	VideoTranscoder transcoder(name);
	ASSERT_EQ(1, transcoder.Init(/*adaptative=*/false, /*allowBridging=*/false));

	Properties props;
	ASSERT_EQ(1, transcoder.SetCodec(VideoCodec::H264, CIF, 30, 256, 300, props));

	CountingSink sink;
	transcoder.AddListener(&sink);

	auto source = std::make_shared<RTPMultiplexer>();
	ASSERT_EQ(1, transcoder.Attach(source));

	DWORD timestamp = 0;
	ASSERT_GT(FeedAt(vp8, *source, 35, 30, timestamp), 20);
	ASSERT_EQ(30, transcoder.GetEffectiveFps());

	// Mute vidéo de 5 s : l'horodatage saute, aucune image n'arrive.
	timestamp += 5 * 90000;

	// Reprise à la même cadence : la fenêtre est vide, elle se remplit à
	// nouveau et rend la MÊME valeur — donc rien à appliquer.
	ASSERT_GT(FeedAt(vp8, *source, 35, 30, timestamp), 20);

	EXPECT_EQ(30, transcoder.GetEffectiveFps())
		<< "une pause ne doit pas etre comptee comme un ecart de cadence";
	EXPECT_EQ(300, transcoder.GetEffectiveIntraPeriod());

	transcoder.Dettach();
	transcoder.RemoveListener(&sink);
	transcoder.End();
}

// Après une pause, la fenêtre est VIDE : une reprise à une autre cadence ne peut
// prendre effet qu'une fois 30 écarts postérieurs à la pause accumulés. Entre
// temps l'encodeur garde le `fps` d'avant la pause — la seule valeur connue.
TEST(VideoEncoderInline, ApresUnePauseLaFenetreDoitSeRemplirAvantDAgir)
{
	Vp8Source vp8;
	if (!vp8.Open(GetWidth(CIF), GetHeight(CIF), 30, 256))
		GTEST_SKIP() << "encodeur VP8 indisponible";

	std::wstring name = L"fps-pause-bascule";
	VideoTranscoder transcoder(name);
	ASSERT_EQ(1, transcoder.Init(/*adaptative=*/false, /*allowBridging=*/false));

	Properties props;
	ASSERT_EQ(1, transcoder.SetCodec(VideoCodec::H264, CIF, 30, 256, 300, props));

	CountingSink sink;
	transcoder.AddListener(&sink);

	auto source = std::make_shared<RTPMultiplexer>();
	ASSERT_EQ(1, transcoder.Attach(source));

	DWORD timestamp = 0;
	ASSERT_GT(FeedAt(vp8, *source, 35, 30, timestamp), 20);
	ASSERT_EQ(30, transcoder.GetEffectiveFps());

	timestamp += 5 * 90000;

	// Dix images à 15 im/s : la fenêtre n'est pas pleine, rien ne bouge.
	ASSERT_GT(FeedAt(vp8, *source, 10, 15, timestamp), 5);
	EXPECT_EQ(30, transcoder.GetEffectiveFps())
		<< "une fenetre incomplete ne doit rien appliquer";

	// Trente-cinq de plus : la fenêtre est pleine et postérieure à la pause.
	ASSERT_GT(FeedAt(vp8, *source, 35, 15, timestamp), 25);
	EXPECT_EQ(15, transcoder.GetEffectiveFps())
		<< "la nouvelle cadence doit s'appliquer une fois la fenetre pleine";
	EXPECT_EQ(150, transcoder.GetEffectiveIntraPeriod());

	transcoder.Dettach();
	transcoder.RemoveListener(&sink);
	transcoder.End();
}

}  // namespace
