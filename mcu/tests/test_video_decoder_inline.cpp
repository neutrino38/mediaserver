/**
 * test_video_decoder_inline.cpp — le décodeur vidéo JSR309 décode sur le
 * thread de la source.
 *
 * Lot 2 de `jsr309_transcode_sans_thread.md` : `VideoDecoderJoinableWorker`
 * n'a plus qu'un chemin. Le drapeau `useThread`, la `WaitQueue` et la boucle
 * `Decode()` — avec son membre `videoDecoder` masqué par un homonyme local —
 * ont disparu. Comme pour l'audio (lot 1), ce qui est vérifié ici c'est qu'il
 * n'y a plus rien à attendre : l'image est livrée au puits avant que
 * `Multiplex` rende la main.
 *
 * Le dernier test est le pendant de sûreté : `Dettach` pendant un
 * `onRTPPacket` concurrent ne peut pas détruire le décodeur sous le paquet,
 * parce que `RemoveListener` ne rend la main que le `Multiplex` en cours
 * terminé (§4.1 et §4.3 du plan). À jouer aussi sous ASan/TSan.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <dirent.h>
#include <memory>
#include <thread>

#include "rtp.h"
#include "medkit/video.h"
#include "../src/jsr309/VideoDecoderWorker.h"
#include "../src/jsr309/RTPMultiplexer.h"

namespace {

// Puits vidéo instrumenté : il compte les images décodées qu'on lui livre.
class CountingVideoOutput : public VideoOutput
{
public:
	int NextFrame(PictPtr pic) override
	{
		if (pic) frames++;
		return 1;
	}

	int SetVideoSize(int width, int height) override
	{
		this->width = width;
		this->height = height;
		return 1;
	}

	std::atomic<int>	frames { 0 };
	std::atomic<int>	width { 0 };
	std::atomic<int>	height { 0 };
};

// Source VP8 réelle : une image de synthèse encodée par le vrai encodeur, puis
// paquetisée comme le fait RTPMultiplexerSmoother::SmoothFrame. Pas de fixture
// à versionner, et le décodeur voit un flux qu'il sait lire.
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

	// Livre les paquets d'une image un par un, SANS jamais en copier :
	// `RTPPacket` porte un `header` qui pointe dans son propre `buffer` et n'a
	// pas de constructeur de copie — une copie garde le pointeur de
	// l'original, et le lire après la mort de l'original est un accès à de la
	// mémoire libérée (le bit de marque s'y perd en premier). D'où le rappel
	// plutôt qu'un `std::vector<RTPPacket>`.
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

TEST(VideoDecoderInline, LImageDecodeeEstLaAuRetourDeMultiplex)
{
	Vp8Source vp8;
	if (!vp8.Open(GetWidth(CIF), GetHeight(CIF), 10, 256))
		GTEST_SKIP() << "encodeur VP8 indisponible : rien a decoder";

	CountingVideoOutput output;
	VideoDecoderJoinableWorker decoder;
	decoder.Init(&output);

	auto source = std::make_shared<RTPMultiplexer>();
	ASSERT_EQ(1, decoder.Attach(source));

	const int sent = vp8.NextFrame(80, 0, [&](RTPPacket& p){ source->Multiplex(p); });
	ASSERT_GT(sent, 0) << "l'encodeur VP8 n'a rien produit";

	// Pas d'attente, pas de sommeil : le dernier paquet porte la marque de fin
	// d'image, l'image est donc livrée avant que Multiplex rende la main.
	EXPECT_EQ(1, output.frames.load())
		<< "l'image doit etre decodee avant que Multiplex rende la main";
	EXPECT_EQ(GetWidth(CIF), output.width.load());
	EXPECT_EQ(GetHeight(CIF), output.height.load());

	decoder.Dettach();
	decoder.End();
}

TEST(VideoDecoderInline, DemarrerLeDecodeurNeCreeAucunThread)
{
	CountingVideoOutput output;
	VideoDecoderJoinableWorker decoder;
	decoder.Init(&output);

	const int before = CountThreads();
	ASSERT_GT(before, 0);

	auto source = std::make_shared<RTPMultiplexer>();
	ASSERT_EQ(1, decoder.Attach(source));

	EXPECT_EQ(before, CountThreads())
		<< "le decodeur video ne doit plus porter de thread";

	decoder.Dettach();
	decoder.End();
}

// `Dettach` détruit le décodeur ffmpeg. Il ne peut le faire qu'une fois le
// `Multiplex` en cours terminé : après son retour, plus aucune image n'arrive,
// et rien n'a travaillé sur un décodeur libéré.
TEST(VideoDecoderInline, DettachConcurrentNeLibereRienSousLePaquet)
{
	Vp8Source vp8;
	if (!vp8.Open(GetWidth(CIF), GetHeight(CIF), 10, 256))
		GTEST_SKIP() << "encodeur VP8 indisponible : rien a decoder";

	CountingVideoOutput output;
	VideoDecoderJoinableWorker decoder;
	decoder.Init(&output);

	auto source = std::make_shared<RTPMultiplexer>();
	ASSERT_EQ(1, decoder.Attach(source));

	std::atomic<bool> stop { false };
	std::thread producer([&]{
		for (int n = 0; !stop.load(); ++n)
			vp8.NextFrame((BYTE)(16 + (n % 24) * 8), (DWORD)n * 9000,
				      [&](RTPPacket& p){ source->Multiplex(p); });
	});

	// Laisser le producteur entrer dans la boucle, puis détacher au milieu.
	while (output.frames.load() < 5)
		std::this_thread::yield();

	decoder.Dettach();

	const int atDettach = output.frames.load();
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	EXPECT_EQ(atDettach, output.frames.load())
		<< "RemoveListener est la barriere : plus rien ne doit passer apres";

	stop = true;
	producer.join();

	decoder.End();
}

}	// namespace
