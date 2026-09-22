/**
 * test_jsr309_recorder_file.cpp — le Recorder JSR-309 écrit ses fichiers par le
 * FfMediaFileWriter de libmedkit (libavformat).
 *
 * Ces tests entrent par où la production entre : une source `Joinable`, un
 * `Attach`, des paquets RTP publiés par la source. Ils relisent ensuite le
 * fichier avec libavformat, parce que c'est la seule preuve qui compte — un
 * code de retour de `ProcessFrame` ne dit pas si le fichier est lisible.
 *
 * Ce qu'ils tiennent :
 *  - le CONTENEUR suit l'extension, et avec lui la décision de transcoder :
 *    PCMU s'écrit tel quel en Matroska, il devient de l'AAC en MP4 ;
 *  - une piste dont le codec n'est connu qu'au premier paquet RTP est quand
 *    même dans le fichier, alors que libavformat fige l'en-tête à la première
 *    écriture (c'est `FfMediaFileWriter::ExpectTrack` qui l'obtient) ;
 *  - une extension que personne ne sait muxer est refusée à la création, pas
 *    silencieusement enregistrée dans le vide.
 */
#include <gtest/gtest.h>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include <unistd.h>
#include "../src/jsr309/Recorder.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

namespace {

const int W = 176, H = 144;

std::string TmpPath(const char* name)
{
	std::string p = "/tmp/mcu_recorder_";
	p += name;
	return p;
}

// Source de paquets : ce que fait RTPMultiplexer::Multiplex côté production.
class FakeLeg : public Joinable
{
public:
	void AddListener(Joinable::Listener* listener) override	{ listeners.push_back(listener); }
	void RemoveListener(Joinable::Listener* listener) override
	{
		listeners.erase(std::remove(listeners.begin(),listeners.end(),listener),listeners.end());
	}
	void Update() override		{ updates++; }
	void SetREMB(DWORD estimation) override	{}

	void Publish(RTPPacket& packet)
	{
		for (Joinable::Listener* l : listeners)
			l->onRTPPacket(packet);
	}

	int updates = 0;
private:
	std::vector<Joinable::Listener*> listeners;
};

// --- Émission de paquets ---------------------------------------------------

void PushPcmu(FakeLeg& leg, int frames, DWORD startTs = 16000)
{
	for (int i = 0; i < frames; i++)
	{
		RTPPacket packet(MediaFrame::Audio,AudioCodec::PCMU);
		BYTE* dst = packet.GetMediaData();
		//160 échantillons de 20 ms, valeur arbitraire : aucun muxer ne parse du
		//PCM par échantillon.
		memset(dst,0xFF,160);
		packet.SetMediaLength(160);
		packet.SetSeqNum(i+1);
		packet.SetTimestamp(startTs+i*160);	// horloge RTP 8 kHz
		packet.SetMark(true);
		leg.Publish(packet);
	}
}

void PushText(FakeLeg& leg, const char* utf8, DWORD ts)
{
	RTPPacket packet(MediaFrame::Text,TextCodec::T140);
	DWORD len = strlen(utf8);
	memcpy(packet.GetMediaData(),utf8,len);
	packet.SetMediaLength(len);
	packet.SetSeqNum(1);
	packet.SetTimestamp(ts);		// horloge T.140 = 1 kHz = ms
	packet.SetMark(true);
	leg.Publish(packet);
}

// Vidéo RÉELLEMENT encodée : le dépaquétiseur, puis le conteneur, lisent le
// contenu (en-tête d'image clé VP8 pour les dimensions, SPS/PPS pour l'avcC
// H264). Une charge utile arbitraire ne prouverait rien.
class VideoSource
{
public:
	bool Open(VideoCodec::Type codec, int width, int height)
	{
		Properties none;
		encoder = VideoCodecFactory::CreateEncoder(codec,none);
		if (!encoder)
			return false;
		encoder->SetFrameRate(10,256,1);	// intra à chaque image
		if (encoder->SetSize(width,height) < 0)
			return false;
		this->codec  = codec;
		this->width  = width;
		this->height = height;
		return true;
	}

	~VideoSource()	{ delete encoder; }

	// Livre les paquets d'une image un par un, sans jamais copier de RTPPacket
	// (son `header` pointe dans son propre buffer : une copie le garde).
	int NextFrame(FakeLeg& leg, BYTE luma, DWORD timestamp)
	{
		PictPtr pic = Pict::CreateColor(width,height,luma,128,128);
		if (!pic)
			return 0;

		// Un encodeur MATERIEL retient sa premiere image : EncodeFrame rend alors
		// nullptr sans que rien n'aille mal. On represente la meme image jusqu'a ce
		// qu'une trame sorte — sinon ce harnais mesure la latence de l'encodeur au
		// lieu de ce que le test veut prouver, et tout le fichier devient rouge sur
		// une machine equipee d'un GPU.
		VideoFramePtr frame;
		for (int attempt = 0; attempt < 4 && !frame; ++attempt)
			frame = encoder->EncodeFrame(pic);

		if (!frame || !frame->HasRtpPacketizationInfo())
			return 0;

		MediaFrame::RtpPacketizationInfo& info = frame->GetRtpPacketizationInfo();
		int sent = 0;
		for (int i = 0; i < (int)info.size(); ++i)
		{
			MediaFrame::RtpPacketization* rtp = info[i];

			RTPPacket packet(MediaFrame::Video,codec);
			if (rtp->GetPrefixLen()+rtp->GetSize() > packet.GetMaxMediaLength())
				continue;

			BYTE* dst = packet.GetMediaData();
			memcpy(dst,rtp->GetPrefixData(),rtp->GetPrefixLen());
			memcpy(dst+rtp->GetPrefixLen(),frame->GetData()+rtp->GetPos(),rtp->GetSize());
			packet.SetMediaLength(rtp->GetPrefixLen()+rtp->GetSize());
			packet.SetSeqNum(seq++);
			packet.SetTimestamp(timestamp);		// horloge RTP 90 kHz
			packet.SetMark(i+1==(int)info.size());
			leg.Publish(packet);
			sent++;
		}
		return sent;
	}

private:
	VideoEncoder*		encoder = NULL;
	VideoCodec::Type	codec = VideoCodec::H264;
	int			width = 0;
	int			height = 0;
	WORD			seq = 1;
};

// --- Relecture du fichier --------------------------------------------------

struct StreamInfo
{
	int		type;		// AVMEDIA_TYPE_*
	int		codec;		// AV_CODEC_ID_*
	int		width, height;
	int		sampleRate;
	int		packets;
	std::string	payloads;	// sous-titres concaténés
};

struct FileInfo
{
	std::string		format;
	std::vector<StreamInfo>	streams;
};

bool Probe(const std::string& path, FileInfo& out)
{
	AVFormatContext* fmt = NULL;

	if (avformat_open_input(&fmt,path.c_str(),NULL,NULL) < 0)
		return false;
	if (avformat_find_stream_info(fmt,NULL) < 0)
	{
		avformat_close_input(&fmt);
		return false;
	}

	out.format = fmt->iformat->name;
	out.streams.resize(fmt->nb_streams);
	for (unsigned i = 0; i < fmt->nb_streams; i++)
	{
		AVCodecParameters* par = fmt->streams[i]->codecpar;
		StreamInfo& s = out.streams[i];
		s.type       = par->codec_type;
		s.codec      = par->codec_id;
		s.width      = par->width;
		s.height     = par->height;
		s.sampleRate = par->sample_rate;
		s.packets    = 0;
	}

	AVPacket* pkt = av_packet_alloc();
	while (pkt && av_read_frame(fmt,pkt) >= 0)
	{
		StreamInfo& s = out.streams[pkt->stream_index];
		s.packets++;
		if (s.type==AVMEDIA_TYPE_SUBTITLE && pkt->size > 0)
			s.payloads.append((const char*)pkt->data,pkt->size);
		av_packet_unref(pkt);
	}
	av_packet_free(&pkt);

	avformat_close_input(&fmt);
	return true;
}

const StreamInfo* FindStream(const FileInfo& info, int type)
{
	for (size_t i = 0; i < info.streams.size(); i++)
		if (info.streams[i].type==type)
			return &info.streams[i];
	return NULL;
}

} // namespace

TEST(Jsr309RecorderFile, LeMatroskaGardeLAudioTelecomEtLeTexteSansTranscodage)
{
	std::string path = TmpPath("telecom.mkv");
	::unlink(path.c_str());

	auto audioLeg = std::make_shared<FakeLeg>();
	auto textLeg  = std::make_shared<FakeLeg>();

	{
		Recorder recorder(L"rec");
		//Les Attach précèdent l'enregistrement : c'est d'eux que le fichier
		//apprend quelles pistes il doit ouvrir.
		ASSERT_EQ(1,recorder.Attach(MediaFrame::Audio,audioLeg));
		ASSERT_EQ(1,recorder.Attach(MediaFrame::Text,textLeg));

		ASSERT_TRUE(recorder.Create(path.c_str()));
		ASSERT_TRUE(recorder.Record());

		PushPcmu(*audioLeg,25);
		PushText(*textLeg,"bonjour",1000);
		PushPcmu(*audioLeg,25,16000+25*160);

		ASSERT_TRUE(recorder.Close());
	}

	FileInfo info;
	ASSERT_TRUE(Probe(path,info));
	EXPECT_EQ("matroska,webm",info.format);

	const StreamInfo* a = FindStream(info,AVMEDIA_TYPE_AUDIO);
	ASSERT_TRUE(a != NULL);
	//Le muxer Matroska porte le PCMU : aucune raison de transcoder.
	EXPECT_EQ(AV_CODEC_ID_PCM_MULAW,a->codec);
	EXPECT_EQ(8000,a->sampleRate);
	EXPECT_EQ(50,a->packets);

	const StreamInfo* t = FindStream(info,AVMEDIA_TYPE_SUBTITLE);
	ASSERT_TRUE(t != NULL);
	EXPECT_NE(std::string::npos,t->payloads.find("bonjour"));

	::unlink(path.c_str());
}

TEST(Jsr309RecorderFile, LeMp4TranscodeLAudioTelecomEnAac)
{
	std::string path = TmpPath("telecom.mp4");
	::unlink(path.c_str());

	auto audioLeg = std::make_shared<FakeLeg>();

	{
		Recorder recorder(L"rec");
		ASSERT_EQ(1,recorder.Attach(MediaFrame::Audio,audioLeg));

		ASSERT_TRUE(recorder.Create(path.c_str()));
		ASSERT_TRUE(recorder.Record());

		//80 trames de 20 ms : de quoi remplir plusieurs trames AAC de 1024
		//échantillons (128 ms à 8 kHz).
		PushPcmu(*audioLeg,80);

		ASSERT_TRUE(recorder.Close());
	}

	FileInfo info;
	ASSERT_TRUE(Probe(path,info));

	const StreamInfo* a = FindStream(info,AVMEDIA_TYPE_AUDIO);
	ASSERT_TRUE(a != NULL);
	//Le muxer mp4 refuse tous les codecs télécom : la seule façon d'enregistrer
	//cet appel en .mp4 est de le transcoder.
	EXPECT_EQ(AV_CODEC_ID_AAC,a->codec);
	EXPECT_GT(a->packets,0);

	::unlink(path.c_str());
}

/*
 * Le cas que libavformat rend piégeux : l'audio coule dès le premier paquet,
 * mais le codec vidéo n'est connu qu'à la première image — après l'en-tête,
 * qui se fige à la première écriture. Sans l'attente de piste, le fichier
 * n'aurait aucune vidéo, et rien ne le signalerait.
 */
TEST(Jsr309RecorderFile, LaVideoRestePresenteQuandLAudioCouleAvantElle)
{
	std::string path = TmpPath("video_tardive.mkv");
	::unlink(path.c_str());

	auto audioLeg = std::make_shared<FakeLeg>();
	auto videoLeg = std::make_shared<FakeLeg>();

	VideoSource source;
	ASSERT_TRUE(source.Open(VideoCodec::VP8,W,H));

	{
		Recorder recorder(L"rec");
		ASSERT_EQ(1,recorder.Attach(MediaFrame::Audio,audioLeg));
		ASSERT_EQ(1,recorder.Attach(MediaFrame::Video,videoLeg));

		//L'appelant a demandé de ne pas attendre la vidéo : c'est ce réglage
		//qui met l'audio en tête, donc qui expose la course.
		recorder.SetWaitVideo(false);
		ASSERT_TRUE(recorder.Create(path.c_str()));
		ASSERT_TRUE(recorder.Record());

		PushPcmu(*audioLeg,25);
		for (int i = 0; i < 5; i++)
			ASSERT_GT(source.NextFrame(*videoLeg,(BYTE)(16+i*8),i*9000),0);
		PushPcmu(*audioLeg,25,16000+25*160);

		ASSERT_TRUE(recorder.Close());
	}

	FileInfo info;
	ASSERT_TRUE(Probe(path,info));
	ASSERT_EQ(2u,info.streams.size());

	const StreamInfo* a = FindStream(info,AVMEDIA_TYPE_AUDIO);
	const StreamInfo* v = FindStream(info,AVMEDIA_TYPE_VIDEO);
	ASSERT_TRUE(a != NULL);
	ASSERT_TRUE(v != NULL);
	//Matroska porte VP8 : la vidéo aussi s'enregistre sans transcodage, et
	//l'audio antérieur à l'en-tête n'est pas perdu pour autant.
	EXPECT_EQ(AV_CODEC_ID_VP8,v->codec);
	EXPECT_EQ(W,v->width);
	EXPECT_EQ(H,v->height);
	EXPECT_GT(v->packets,0);
	EXPECT_EQ(50,a->packets);

	::unlink(path.c_str());
}

/*
 * Le cas nominal d'un appel elixip : audio télécom + vidéo H264 en .mp4, avec
 * l'attente de la première image clé (waitVideo, réglage par défaut).
 */
TEST(Jsr309RecorderFile, LAppelH264EnMp4DonneUnePisteVideoEtUnePisteAac)
{
	std::string path = TmpPath("appel.mp4");
	::unlink(path.c_str());

	auto audioLeg = std::make_shared<FakeLeg>();
	auto videoLeg = std::make_shared<FakeLeg>();

	VideoSource source;
	ASSERT_TRUE(source.Open(VideoCodec::H264,W,H));

	int audioBeforeVideo = 25;
	{
		Recorder recorder(L"rec");
		ASSERT_EQ(1,recorder.Attach(MediaFrame::Audio,audioLeg));
		ASSERT_EQ(1,recorder.Attach(MediaFrame::Video,videoLeg));

		ASSERT_TRUE(recorder.Create(path.c_str()));
		ASSERT_TRUE(recorder.Record());

		//waitVideo est armé : cet audio-là est écarté, il précède la vidéo.
		PushPcmu(*audioLeg,audioBeforeVideo);
		for (int i = 0; i < 10; i++)
			ASSERT_GT(source.NextFrame(*videoLeg,(BYTE)(16+i*8),i*9000),0);
		PushPcmu(*audioLeg,80,16000+audioBeforeVideo*160);

		ASSERT_TRUE(recorder.Close());
	}

	FileInfo info;
	ASSERT_TRUE(Probe(path,info));
	ASSERT_EQ(2u,info.streams.size());

	const StreamInfo* a = FindStream(info,AVMEDIA_TYPE_AUDIO);
	const StreamInfo* v = FindStream(info,AVMEDIA_TYPE_VIDEO);
	ASSERT_TRUE(a != NULL);
	ASSERT_TRUE(v != NULL);
	EXPECT_EQ(AV_CODEC_ID_H264,v->codec);
	EXPECT_EQ(W,v->width);
	EXPECT_EQ(H,v->height);
	EXPECT_GT(v->packets,0);
	//L'audio est là, et il ne contient pas ce qui a précédé la vidéo : 80
	//trames de 20 ms remises, réunies par paquets AAC de 1024 échantillons.
	EXPECT_EQ(AV_CODEC_ID_AAC,a->codec);
	EXPECT_GT(a->packets,0);
	EXPECT_LT(a->packets,audioBeforeVideo+80);

	::unlink(path.c_str());
}

TEST(Jsr309RecorderFile, UneExtensionQueRienNeMuxeEstRefuseeALaCreation)
{
	std::string path = TmpPath("inconnu.avi");
	::unlink(path.c_str());

	auto audioLeg = std::make_shared<FakeLeg>();

	Recorder recorder(L"rec");
	ASSERT_EQ(1,recorder.Attach(MediaFrame::Audio,audioLeg));

	//Le conteneur vient de l'extension : l'appelant XML-RPC doit recevoir une
	//erreur, pas un enregistrement qui n'enregistre rien.
	EXPECT_FALSE(recorder.Create(path.c_str()));
	EXPECT_FALSE(recorder.Record());

	//Et un paquet reçu dans cet état ne doit rien casser.
	PushPcmu(*audioLeg,5);
}
