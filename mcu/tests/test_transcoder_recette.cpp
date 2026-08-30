/**
 * test_transcoder_recette.cpp — les scénarios de la recette du chantier
 * « transcodeurs sans thread » qui se jouent sans appel réel.
 *
 * Lot 5 de `jsr309_transcode_sans_thread.md`. La recette elle-même demande de
 * vrais appels (`docs/maintenance/recette-transcodeur-sans-thread.md`) ; ce
 * fichier fige ce qu'un test peut prouver seul, pour qu'une régression sur ces
 * points-là n'attende pas la prochaine séance de recette :
 *
 *  - détachement puis rattachement à une NOUVELLE source : le flux repart, et
 *    l'ancienne source ne publie plus rien ;
 *  - la source qui s'arrête (`EndStream`) ferme le chemin, et un rattachement
 *    le rouvre (§3.3) ;
 *  - la source détruite sans `Dettach` ne laisse pas de pointeur pendant ;
 *  - renégociation en cours d'appel — `SetCodec` et bornes négociées — sans
 *    créer ni joindre de thread depuis le plan de contrôle (§4.4).
 *
 * Piège d'écriture (lot 2) : ne JAMAIS copier un `RTPPacket`. Il porte un
 * `header` qui pointe dans son propre `buffer` et n'a pas de constructeur de
 * copie — d'où les sources par rappel.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <dirent.h>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

#include "rtp.h"
#include "medkit/video.h"
#include "../src/jsr309/AudioTranscoder.h"
#include "../src/jsr309/VideoTranscoder.h"
#include "../src/jsr309/RTPMultiplexer.h"

namespace {

typedef std::chrono::milliseconds Ms;

// Puits instrumenté : il ne relaie aucun codec (`TryCheckCodec` négatif), donc
// le transcodeur reste sur le chemin décodage/encodage.
class CountingSink : public Joinable::Listener
{
public:
	void onRTPPacket(RTPPacket &packet) override
	{
		std::lock_guard<std::mutex> lock(mutex);
		count++;
		codecs.insert(packet.GetCodec());
		ssrcs.insert(packet.GetSSRC());
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

	std::set<DWORD> Ssrcs()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return ssrcs;
	}

	void Reset()
	{
		std::lock_guard<std::mutex> lock(mutex);
		count = 0;
		codecs.clear();
		ssrcs.clear();
	}

private:
	std::mutex	mutex;
	int		count = 0;
	std::set<int>	codecs;
	std::set<DWORD>	ssrcs;
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

// Silence PCMU : 0xFF est le zéro de la loi µ. 160 octets = 20 ms à 8 kHz.
template <typename Sink>
void SendPcmu(WORD seq, Sink sink)
{
	RTPPacket packet(MediaFrame::Audio, AudioCodec::PCMU);
	packet.SetSeqNum(seq);
	packet.SetTimestamp((DWORD)seq * 160);
	memset(packet.GetMediaData(), 0xFF, 160);
	packet.SetMediaLength(160);
	sink(packet);
}

// Source VP8 de synthèse, intra à chaque image : un rattachement décode dès le
// premier paquet, sans attendre une trame clé.
class Vp8Source
{
public:
	bool Open(int width, int height, int fps, int kbits)
	{
		Properties none;
		encoder = VideoCodecFactory::CreateEncoder(VideoCodec::VP8, none);
		if (!encoder)
			return false;
		encoder->SetFrameRate(fps, kbits, 1);
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

private:
	VideoEncoder*	encoder = NULL;
	int		width = 0;
	int		height = 0;
	WORD		seq = 1;
};

// Nourrit `frames` images à `fps` (horodatage RTP ET temps réel : la borne de
// sortie du transcodeur est une vraie horloge).
void FeedVideo(Vp8Source& vp8, RTPMultiplexer& source, int frames, int fps,
	       DWORD& timestamp, int startLuma = 16)
{
	const DWORD step = 90000 / (DWORD)fps;
	for (int n = 0; n < frames; ++n)
	{
		vp8.NextFrame((BYTE)(startLuma + (n * 7) % 200), timestamp,
			      [&](RTPPacket& p){ source.Multiplex(p); });
		timestamp += step;
		std::this_thread::sleep_for(Ms(1000 / fps));
	}
}

// Le lisseur étale les paquets d'une image : c'est la seule chose qu'on attende
// dans les tests vidéo, et elle est bornée.
bool WaitForOutput(CountingSink& sink, int atLeast, int timeoutMs)
{
	for (int waited = 0; waited < timeoutMs && sink.Count() < atLeast; waited += 25)
		std::this_thread::sleep_for(Ms(25));
	return sink.Count() >= atLeast;
}

// ── Audio ───────────────────────────────────────────────────────────────────

// Recette « détachement/rattachement ». Trois propriétés d'un coup : le flux
// repart sur la nouvelle source, l'ancienne ne publie plus rien, et rien de
// tout cela ne crée de thread.
TEST(TranscoderRecette, AudioRattachementApresDettachRepart)
{
	std::wstring name = L"recette-audio-reattach";
	AudioTranscoder transcoder(name);
	ASSERT_EQ(1, transcoder.Init(/*allowBriding=*/false));
	ASSERT_EQ(1, transcoder.SetCodec(AudioCodec::OPUS));

	CountingSink sink;
	transcoder.AddListener(&sink);

	const int threads = CountThreads();
	ASSERT_GT(threads, 0);

	auto first = std::make_shared<RTPMultiplexer>();
	ASSERT_EQ(1, transcoder.Attach(first));

	WORD seq = 1;
	for (int n = 0; n < 10; ++n, ++seq)
		SendPcmu(seq, [&](RTPPacket& p){ first->Multiplex(p); });
	ASSERT_GT(sink.Count(), 0);

	transcoder.Dettach();

	//L'ancienne source ne doit plus nous atteindre : c'est ce que garantit le
	//RemoveListener fait avant l'arrêt du décodeur.
	sink.Reset();
	for (int n = 0; n < 10; ++n, ++seq)
		SendPcmu(seq, [&](RTPPacket& p){ first->Multiplex(p); });
	EXPECT_EQ(0, sink.Count()) << "une source detachee ne doit plus publier dans le transcodeur";

	auto second = std::make_shared<RTPMultiplexer>();
	ASSERT_EQ(1, transcoder.Attach(second));

	sink.Reset();
	for (int n = 0; n < 10; ++n, ++seq)
		SendPcmu(seq, [&](RTPPacket& p){ second->Multiplex(p); });
	EXPECT_GT(sink.Count(), 0) << "le rattachement doit rouvrir le chemin de decodage";

	std::set<int> codecs = sink.Codecs();
	ASSERT_EQ((size_t)1, codecs.size());
	EXPECT_EQ((int)AudioCodec::OPUS, *codecs.begin());

	EXPECT_EQ(threads, CountThreads())
		<< "ni le detachement ni le rattachement ne creent de thread";

	transcoder.Dettach();
	transcoder.RemoveListener(&sink);
	transcoder.End();
}

// Recette « arrêt de la source pendant l'appel » (§3.3). `EndStream` ferme le
// chemin ; les paquets qui suivraient ne sont plus décodés. Un rattachement le
// rouvre — c'est la reprise d'appel côté contrôleur.
TEST(TranscoderRecette, AudioSourceArreteeFermeLeChemin)
{
	std::wstring name = L"recette-audio-endstream";
	AudioTranscoder transcoder(name);
	ASSERT_EQ(1, transcoder.Init(/*allowBriding=*/false));
	ASSERT_EQ(1, transcoder.SetCodec(AudioCodec::OPUS));

	CountingSink sink;
	transcoder.AddListener(&sink);

	auto source = std::make_shared<RTPMultiplexer>();
	ASSERT_EQ(1, transcoder.Attach(source));

	WORD seq = 1;
	for (int n = 0; n < 10; ++n, ++seq)
		SendPcmu(seq, [&](RTPPacket& p){ source->Multiplex(p); });
	ASSERT_GT(sink.Count(), 0);

	source->EndStream();

	sink.Reset();
	for (int n = 0; n < 10; ++n, ++seq)
		SendPcmu(seq, [&](RTPPacket& p){ source->Multiplex(p); });
	EXPECT_EQ(0, sink.Count()) << "apres EndStream, plus rien ne doit etre decode";

	//Reprise : la même source, rattachée, redémarre le décodeur.
	ASSERT_EQ(1, transcoder.Attach(source));
	for (int n = 0; n < 10; ++n, ++seq)
		SendPcmu(seq, [&](RTPPacket& p){ source->Multiplex(p); });
	EXPECT_GT(sink.Count(), 0) << "le rattachement doit rouvrir le chemin apres EndStream";

	transcoder.Dettach();
	transcoder.RemoveListener(&sink);
	transcoder.End();
}

// La source disparaît sans que le contrôleur ait détaché : le lien retour est un
// `weak_ptr`, donc `Dettach` et `End` doivent s'en apercevoir au lieu de
// déréférencer. À jouer aussi sous ASan.
TEST(TranscoderRecette, AudioSourceDetruiteSansDettach)
{
	std::wstring name = L"recette-audio-source-morte";
	AudioTranscoder transcoder(name);
	ASSERT_EQ(1, transcoder.Init(/*allowBriding=*/false));
	ASSERT_EQ(1, transcoder.SetCodec(AudioCodec::OPUS));

	CountingSink sink;
	transcoder.AddListener(&sink);

	{
		auto source = std::make_shared<RTPMultiplexer>();
		ASSERT_EQ(1, transcoder.Attach(source));

		for (WORD seq = 1; seq <= 10; ++seq)
			SendPcmu(seq, [&](RTPPacket& p){ source->Multiplex(p); });
		ASSERT_GT(sink.Count(), 0);
	}

	EXPECT_EQ(1, transcoder.Dettach());

	transcoder.RemoveListener(&sink);
	transcoder.End();
}

// ── Vidéo ───────────────────────────────────────────────────────────────────

// Même recette côté vidéo. Le lisseur reste le seul thread : le compte ne doit
// pas bouger d'un détachement à l'autre.
TEST(TranscoderRecette, VideoRattachementApresDettachRepart)
{
	Vp8Source vp8;
	if (!vp8.Open(GetWidth(CIF), GetHeight(CIF), 10, 256))
		GTEST_SKIP() << "encodeur VP8 indisponible";

	std::wstring name = L"recette-video-reattach";
	VideoTranscoder transcoder(name);
	ASSERT_EQ(1, transcoder.Init(/*adaptative=*/false, /*allowBridging=*/false));

	Properties props;
	ASSERT_EQ(1, transcoder.SetCodec(VideoCodec::H264, CIF, 10, 256, 10, props));

	CountingSink sink;
	transcoder.AddListener(&sink);

	auto first = std::make_shared<RTPMultiplexer>();
	ASSERT_EQ(1, transcoder.Attach(first));

	DWORD timestamp = 0;
	FeedVideo(vp8, *first, 5, 10, timestamp);
	ASSERT_TRUE(WaitForOutput(sink, 1, 2000)) << "le premier attachement doit produire";

	const int threads = CountThreads();
	ASSERT_GT(threads, 0);

	transcoder.Dettach();

	//Laisser le lisseur finir d'étaler ce qui restait avant de compter.
	std::this_thread::sleep_for(Ms(500));
	sink.Reset();
	FeedVideo(vp8, *first, 5, 10, timestamp);
	std::this_thread::sleep_for(Ms(300));
	EXPECT_EQ(0, sink.Count()) << "une source detachee ne doit plus publier dans le transcodeur";

	auto second = std::make_shared<RTPMultiplexer>();
	ASSERT_EQ(1, transcoder.Attach(second));

	sink.Reset();
	FeedVideo(vp8, *second, 5, 10, timestamp);
	EXPECT_TRUE(WaitForOutput(sink, 1, 2000)) << "le rattachement doit rouvrir le chemin";

	std::set<int> codecs = sink.Codecs();
	ASSERT_EQ((size_t)1, codecs.size());
	EXPECT_EQ((int)VideoCodec::H264, *codecs.begin());

	EXPECT_EQ(threads, CountThreads())
		<< "ni le detachement ni le rattachement ne creent de thread";

	transcoder.Dettach();
	transcoder.RemoveListener(&sink);
	transcoder.End();
}

// Recette « renégociation en cours d'appel », premier volet : `SetCodec` change
// le codec de sortie pendant que la source émet. Le plan de contrôle ne fait
// plus de Stop/Start (§4.4) : il lève un drapeau, et c'est le chemin des
// paquets qui rouvre l'encodeur — avec un SSRC neuf, base de temps neuve.
TEST(TranscoderRecette, VideoRenegociationDeCodecAChaud)
{
	Vp8Source vp8;
	if (!vp8.Open(GetWidth(CIF), GetHeight(CIF), 10, 256))
		GTEST_SKIP() << "encodeur VP8 indisponible";

	std::wstring name = L"recette-video-setcodec";
	VideoTranscoder transcoder(name);
	ASSERT_EQ(1, transcoder.Init(/*adaptative=*/false, /*allowBridging=*/false));

	Properties props;
	ASSERT_EQ(1, transcoder.SetCodec(VideoCodec::H264, CIF, 10, 256, 10, props));

	CountingSink sink;
	transcoder.AddListener(&sink);

	auto source = std::make_shared<RTPMultiplexer>();
	ASSERT_EQ(1, transcoder.Attach(source));

	DWORD timestamp = 0;
	FeedVideo(vp8, *source, 5, 10, timestamp);
	ASSERT_TRUE(WaitForOutput(sink, 1, 2000));

	std::set<DWORD> before = sink.Ssrcs();
	ASSERT_FALSE(before.empty());

	const int threads = CountThreads();
	Properties again;
	ASSERT_EQ(1, transcoder.SetCodec(VideoCodec::VP8, CIF, 10, 256, 10, again));
	EXPECT_EQ(threads, CountThreads())
		<< "SetCodec ne doit ni lancer ni joindre de thread";

	sink.Reset();
	FeedVideo(vp8, *source, 8, 10, timestamp);
	ASSERT_TRUE(WaitForOutput(sink, 1, 2000)) << "le nouveau codec doit produire";

	std::set<int> codecs = sink.Codecs();
	EXPECT_EQ((size_t)1, codecs.count((int)VideoCodec::VP8))
		<< "la sortie doit passer au codec renegocie";

	std::set<DWORD> after = sink.Ssrcs();
	bool renewed = false;
	for (std::set<DWORD>::const_iterator it = after.begin(); it != after.end(); ++it)
		if (!before.count(*it))
			renewed = true;
	EXPECT_TRUE(renewed)
		<< "un encodeur rouvert annonce une nouvelle base de temps par un SSRC neuf";

	transcoder.Dettach();
	transcoder.RemoveListener(&sink);
	transcoder.End();
}

// Second volet : les bornes négociées de la patte émettrice changent en cours
// d'appel. Elles ne sont lues qu'à la création de l'encodeur, donc elles
// exigent de le rouvrir — par le même drapeau que `SetCodec`, sans thread, et
// sans interrompre le flux.
TEST(TranscoderRecette, VideoBornesNegocieesAChaudRouvrentLEncodeur)
{
	Vp8Source vp8;
	if (!vp8.Open(GetWidth(CIF), GetHeight(CIF), 10, 256))
		GTEST_SKIP() << "encodeur VP8 indisponible";

	std::wstring name = L"recette-video-nego";
	VideoTranscoder transcoder(name);
	ASSERT_EQ(1, transcoder.Init(/*adaptative=*/false, /*allowBridging=*/false));

	Properties props;
	ASSERT_EQ(1, transcoder.SetCodec(VideoCodec::H264, CIF, 10, 256, 10, props));

	CountingSink sink;
	transcoder.AddListener(&sink);

	auto source = std::make_shared<RTPMultiplexer>();
	ASSERT_EQ(1, transcoder.Attach(source));

	DWORD timestamp = 0;
	FeedVideo(vp8, *source, 5, 10, timestamp);
	ASSERT_TRUE(WaitForOutput(sink, 1, 2000));

	std::set<DWORD> before = sink.Ssrcs();
	ASSERT_FALSE(before.empty());

	//Ce que la négociation SDP produit pour H.264 : le profil et le niveau que
	//le pair a déclaré savoir décoder.
	std::map<int,Properties> negotiated;
	negotiated[(int)VideoCodec::H264].SetProperty("h264.profile-level-id", "42e01e");

	const int threads = CountThreads();
	transcoder.SetNegotiatedCodecProperties(negotiated);
	EXPECT_EQ(threads, CountThreads())
		<< "pousser des bornes negociees ne doit ni lancer ni joindre de thread";

	sink.Reset();
	FeedVideo(vp8, *source, 8, 10, timestamp);
	ASSERT_TRUE(WaitForOutput(sink, 1, 2000)) << "le flux ne doit pas s'interrompre";

	std::set<int> codecs = sink.Codecs();
	ASSERT_EQ((size_t)1, codecs.size());
	EXPECT_EQ((int)VideoCodec::H264, *codecs.begin());

	std::set<DWORD> after = sink.Ssrcs();
	bool renewed = false;
	for (std::set<DWORD>::const_iterator it = after.begin(); it != after.end(); ++it)
		if (!before.count(*it))
			renewed = true;
	EXPECT_TRUE(renewed)
		<< "l'encodeur rouvert avec les nouvelles bornes tire un SSRC neuf";

	transcoder.Dettach();
	transcoder.RemoveListener(&sink);
	transcoder.End();
}

// Recette « l'image n'est pas déformée » (appel du 2026-08-28 : Bob en 16:9 vu
// écrasé par Alice). Un transcodeur ADAPTATIF suit la géométrie de la source, et
// il la garde après une réouverture d'encodeur — c'est le second point qui
// manquait : `ComputeEffective` remet la géométrie au `mode` du contrôleur, et
// rien ne la ré-appliquait.
TEST(TranscoderRecette, VideoAdaptatifSuitLaGeometrieDeLaSourceMemeApresReouverture)
{
	const int sourceWidth = 640;	// 16:9, alors que le mode demande du 4:3
	const int sourceHeight = 360;

	Vp8Source vp8;
	if (!vp8.Open(sourceWidth, sourceHeight, 10, 256))
		GTEST_SKIP() << "encodeur VP8 indisponible";

	std::wstring name = L"recette-video-ratio";
	VideoTranscoder transcoder(name);
	ASSERT_EQ(1, transcoder.Init(/*adaptative=*/true, /*allowBridging=*/false));

	Properties props;
	ASSERT_EQ(1, transcoder.SetCodec(VideoCodec::H264, CIF, 10, 256, 10, props));

	CountingSink sink;
	transcoder.AddListener(&sink);

	auto source = std::make_shared<RTPMultiplexer>();
	ASSERT_EQ(1, transcoder.Attach(source));

	DWORD timestamp = 0;
	FeedVideo(vp8, *source, 5, 10, timestamp);
	ASSERT_TRUE(WaitForOutput(sink, 1, 2000));

	EXPECT_EQ(sourceWidth, transcoder.GetEffectiveWidth())
		<< "un transcodeur adaptatif encode a la taille de la source, pas au mode";
	EXPECT_EQ(sourceHeight, transcoder.GetEffectiveHeight());

	// Renégociation : l'encodeur est jeté et recréé. La géométrie de la source
	// doit survivre — sinon le pair reçoit du 4:3 etire a partir d'une source 16:9.
	Properties again;
	ASSERT_EQ(1, transcoder.SetCodec(VideoCodec::VP8, CIF, 10, 256, 10, again));

	sink.Reset();
	FeedVideo(vp8, *source, 8, 10, timestamp);
	ASSERT_TRUE(WaitForOutput(sink, 1, 2000));

	EXPECT_EQ(sourceWidth, transcoder.GetEffectiveWidth())
		<< "la reouverture ne doit pas ramener l'encodeur au mode du controleur";
	EXPECT_EQ(sourceHeight, transcoder.GetEffectiveHeight());

	transcoder.Dettach();
	transcoder.RemoveListener(&sink);
	transcoder.End();
}

// Sans le drapeau adaptatif, le contrôleur impose sa géométrie : c'est le
// comportement que `useInputSize=0` doit continuer à donner.
TEST(TranscoderRecette, VideoNonAdaptatifGardeLaGeometrieDuControleur)
{
	Vp8Source vp8;
	if (!vp8.Open(640, 360, 10, 256))
		GTEST_SKIP() << "encodeur VP8 indisponible";

	std::wstring name = L"recette-video-mode-impose";
	VideoTranscoder transcoder(name);
	ASSERT_EQ(1, transcoder.Init(/*adaptative=*/false, /*allowBridging=*/false));

	Properties props;
	ASSERT_EQ(1, transcoder.SetCodec(VideoCodec::H264, CIF, 10, 256, 10, props));

	CountingSink sink;
	transcoder.AddListener(&sink);

	auto source = std::make_shared<RTPMultiplexer>();
	ASSERT_EQ(1, transcoder.Attach(source));

	DWORD timestamp = 0;
	FeedVideo(vp8, *source, 5, 10, timestamp);
	ASSERT_TRUE(WaitForOutput(sink, 1, 2000));

	EXPECT_EQ(GetWidth(CIF), transcoder.GetEffectiveWidth());
	EXPECT_EQ(GetHeight(CIF), transcoder.GetEffectiveHeight());

	transcoder.Dettach();
	transcoder.RemoveListener(&sink);
	transcoder.End();
}

}	// namespace
