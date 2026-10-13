/**
 * test_rtp_bundle.cpp — le transport partagé (BUNDLE, RFC 8843) : une session
 * porteuse, des sessions jointes.
 *
 * Conception : docs/conception/UNIFIED-PLAN/SPEC.md §4.4, lot 2 ; arbitrage :
 * docs/architecture/adr-004-bundle-session-porteuse.md. Les deux côtés sont de
 * vraies RTPSession en boucle locale : audio porteuse, vidéo MAIN, vidéo de
 * rôle 2 et texte jointes, un port de chaque côté. Ces tests figent :
 *   - une session jointe n'ouvre aucun port : elle rend celui de sa porteuse ;
 *   - la réception est routée par MID, puis par SSRC déclaré, puis par payload
 *     type propre à une section ; un paquet qu'aucune règle n'attribue est
 *     jeté, sans toucher aux autres sections ;
 *   - l'émission d'une session jointe part par la porteuse, MID compris ;
 *   - un PLI reçu va à la session dont il vise le SSRC ;
 *   - le SRTP de la porteuse chiffre et déchiffre pour toutes les sections, et
 *     une clé posée sur une section jointe va à la porteuse qui n'en a pas ;
 *   - le watchdog d'une jointe qui se tait sonne, même si l'audio passe ;
 *   - un participant de conférence créé en BUNDLE annonce un seul port.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "mcu.h"
#include "multiconf.h"
#include "rtp.h"
#include "rtpsession.h"
#include "xmlstreaminghandler.h"

namespace {

const char* const kMidUri = "urn:ietf:params:rtp-hdrext:sdes:mid";
const DWORD kPcmu = 0;
const DWORD kVp8Pt = 96;
const DWORD kVp8BisPt = 97;

class CountingListener : public RTPSession::Listener
{
public:
	void onFPURequested(RTPSession*) override { fpu++; }
	void onReceiverEstimatedMaxBitrate(RTPSession*, DWORD) override {}
	void onTempMaxMediaStreamBitrateRequest(RTPSession*, DWORD, DWORD) override {}
	void onRTPTimeout(RTPSession*) override { timeouts++; }

	std::atomic<int> fpu{0};
	std::atomic<int> timeouts{0};
};

struct Options
{
	bool mids = true;		// a=mid + extmap sdes:mid
	DWORD video2Pt = kVp8Pt;	// même PT que la vidéo MAIN par défaut
};

// Un côté de l'appel : une porteuse audio et trois sections jointes.
class Side
{
public:
	Side()
		: audio(MediaFrame::Audio, &la),
		  video(MediaFrame::Video, &lv),
		  video2(MediaFrame::Video, &lv2, (MediaFrame::MediaRole)2),
		  text(MediaFrame::Text, &lt)
	{
	}

	~Side()
	{
		if (!ok)
			return;
		text.End();
		video2.End();
		video.End();
		audio.End();
	}

	bool Init(const Options& options)
	{
		if (!video.JoinCarrier(&audio) || !video2.JoinCarrier(&audio) || !text.JoinCarrier(&audio))
			return false;
		ok = audio.Init() && video.Init() && video2.Init() && text.Init();
		if (!ok)
			return false;
		Setup(audio, kPcmu, AudioCodec::PCMU, "0", options);
		Setup(video, kVp8Pt, VideoCodec::VP8, "1", options);
		Setup(video2, options.video2Pt, VideoCodec::VP8, "2", options);
		return true;
	}

	void Connect(Side& other)
	{
		char ip[] = "127.0.0.1";
		audio.SetRemotePort(ip, other.audio.GetLocalPort());
	}

	CountingListener la, lv, lv2, lt;
	RTPSession audio;
	RTPSession video;
	RTPSession video2;
	RTPSession text;
	bool ok = false;

private:
	static void Setup(RTPSession& session, DWORD pt, DWORD codec, const char* mid, const Options& options)
	{
		RTPMap map;
		map[(BYTE)pt] = (BYTE)codec;
		session.SetSendingRTPMap(map);
		session.SetReceivingRTPMap(map);
		Properties props;
		props["rtcp-mux"] = "1";
		if (options.mids)
		{
			props[kMidUri] = "4";
			props["mid"] = mid;
		}
		session.SetProperties(props);
	}
};

void Send(RTPSession& session, MediaFrame::Type media, DWORD codec, int count = 5)
{
	for (int i = 0; i < count; ++i)
	{
		RTPPacket packet(media, codec, codec);
		memset(packet.GetMediaData(), 0xAA, 8);
		packet.SetMediaLength(8);
		session.SendPacket(packet, i * 3000);
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
}

// Rend le SSRC du premier paquet reçu, 0 si rien n'arrive dans le délai.
DWORD Receive(RTPSession& session, int timeoutMs = 1000)
{
	for (int waited = 0; waited < timeoutMs; waited += 100)
	{
		RTPPacket* packet = session.GetPacket(0, 100);
		if (packet)
		{
			DWORD ssrc = packet->GetSSRC();
			delete packet;
			return ssrc;
		}
	}
	return 0;
}

class RtpBundle : public ::testing::Test
{
protected:
	bool Start(const Options& options = Options())
	{
		if (!server.Init(options) || !peer.Init(options))
			return false;
		server.Connect(peer);
		peer.Connect(server);
		return true;
	}

	Side server;
	Side peer;
};

#define START(...)								\
	do {									\
		if (!Start(__VA_ARGS__))					\
			GTEST_SKIP() << "impossible de binder une paire de ports RTP";	\
	} while (0)

TEST_F(RtpBundle, UneSessionJointeNOuvreAucunPort)
{
	START();
	EXPECT_GT(server.audio.GetLocalPort(), 0);
	EXPECT_EQ(server.video.GetLocalPort(), server.audio.GetLocalPort());
	EXPECT_EQ(server.video2.GetLocalPort(), server.audio.GetLocalPort());
	EXPECT_EQ(server.text.GetLocalPort(), server.audio.GetLocalPort());
	EXPECT_TRUE(server.video.IsJoined());
	EXPECT_FALSE(server.audio.IsJoined());
}

TEST_F(RtpBundle, LeMidRouteChaqueSection)
{
	START();

	Send(peer.video, MediaFrame::Video, VideoCodec::VP8);
	Send(peer.video2, MediaFrame::Video, VideoCodec::VP8);
	Send(peer.audio, MediaFrame::Audio, AudioCodec::PCMU);

	EXPECT_EQ(Receive(server.video), peer.video.GetSendSSRC());
	EXPECT_EQ(Receive(server.video2), peer.video2.GetSendSSRC());
	EXPECT_EQ(Receive(server.audio), peer.audio.GetSendSSRC());
}

TEST_F(RtpBundle, SansMidLeSsrcDeclareRoute)
{
	Options options;
	options.mids = false;
	START(options);

	Properties v, v2;
	v["remote-ssrc"] = std::to_string(peer.video.GetSendSSRC());
	v2["remote-ssrc"] = std::to_string(peer.video2.GetSendSSRC());
	server.video.SetProperties(v);
	server.video2.SetProperties(v2);

	Send(peer.video2, MediaFrame::Video, VideoCodec::VP8);
	Send(peer.video, MediaFrame::Video, VideoCodec::VP8);

	EXPECT_EQ(Receive(server.video), peer.video.GetSendSSRC());
	EXPECT_EQ(Receive(server.video2), peer.video2.GetSendSSRC());
}

TEST_F(RtpBundle, SansMidNiSsrcLePayloadTypeRoute)
{
	Options options;
	options.mids = false;
	options.video2Pt = kVp8BisPt;
	START(options);

	Send(peer.video2, MediaFrame::Video, VideoCodec::VP8);
	Send(peer.video, MediaFrame::Video, VideoCodec::VP8);

	EXPECT_EQ(Receive(server.video), peer.video.GetSendSSRC());
	EXPECT_EQ(Receive(server.video2), peer.video2.GetSendSSRC());
}

TEST_F(RtpBundle, UnPaquetSansSectionEstJeteEtLAudioPasse)
{
	Options options;
	options.mids = false;
	START(options);

	Send(peer.video, MediaFrame::Video, VideoCodec::VP8);
	Send(peer.audio, MediaFrame::Audio, AudioCodec::PCMU);

	EXPECT_EQ(Receive(server.audio), peer.audio.GetSendSSRC());
	EXPECT_EQ(Receive(server.video, 300), 0u) << "deux sections portent ce PT : rien ne doit être deviné";
	EXPECT_EQ(Receive(server.video2, 300), 0u);
}

TEST_F(RtpBundle, UnPliVaALaSessionQuIlVise)
{
	START();

	//Le pair doit connaître le SSRC qu'il vise : la section 2 lui parle d'abord.
	Send(server.video2, MediaFrame::Video, VideoCodec::VP8);
	ASSERT_EQ(Receive(peer.video2), server.video2.GetSendSSRC());

	const int before = server.lv.fpu.load();
	peer.video2.RequestFPU();

	for (int i = 0; i < 50 && server.lv2.fpu.load() == 0; ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	EXPECT_GT(server.lv2.fpu.load(), 0);
	EXPECT_EQ(server.lv.fpu.load(), before) << "le PLI de la section 2 n'est pas pour la caméra";
}

TEST_F(RtpBundle, LeSrtpDeLaPorteuseSertToutesLesSections)
{
	START();

	const char suite[] = "AES_CM_128_HMAC_SHA1_80";
	const char keyA[] = "WVNfX19zZW1jdGwgKCkgewkyMjA7fQp9CnVubGVz";
	const char keyB[] = "d2VhayBrZXlzIGFyZSBnb29kIGVub3VnaCBmb3Ig";

	//Côté serveur, les clés arrivent par une section jointe : la porteuse, qui
	//n'en a pas encore, les prend.
	ASSERT_TRUE(server.video.SetLocalCryptoSDES(suite, keyA));
	ASSERT_TRUE(server.video.SetRemoteCryptoSDES(suite, keyB));
	ASSERT_TRUE(peer.audio.SetLocalCryptoSDES(suite, keyB));
	ASSERT_TRUE(peer.audio.SetRemoteCryptoSDES(suite, keyA));

	Send(peer.video2, MediaFrame::Video, VideoCodec::VP8);
	Send(server.video, MediaFrame::Video, VideoCodec::VP8);

	EXPECT_EQ(Receive(server.video2), peer.video2.GetSendSSRC());
	EXPECT_EQ(Receive(peer.video), server.video.GetSendSSRC());
}

TEST_F(RtpBundle, LeWatchdogDUneJointeSonneQuandElleSeTait)
{
	START();

	server.video2.ArmRTPTimeout(300);

	//L'audio continue de passer, toutes les 20 ms, pendant 1,5 s au plus.
	for (int i = 0; i < 75 && server.lv2.timeouts.load() == 0; ++i)
	{
		Send(peer.audio, MediaFrame::Audio, AudioCodec::PCMU, 1);
		std::this_thread::sleep_for(std::chrono::milliseconds(15));
	}

	EXPECT_EQ(server.lv2.timeouts.load(), 1) << "la section 2 s'est tue alors que l'audio passait";
	EXPECT_EQ(server.la.timeouts.load(), 0);
}

TEST(RtpBundleParticipant, UnParticipantEnBundleAnnonceUnSeulPort)
{
	RTPSession::SetAnnouncedIp("127.0.0.1");
	XmlStreamingHandler handler;
	MCU mcu;
	mcu.Init(&handler, 0);
	const int confId = mcu.CreateConference(L"bundle", 0);
	ASSERT_GT(confId, 0);
	std::shared_ptr<MultiConf> conf;
	ASSERT_TRUE(mcu.GetConferenceRef(confId, conf));
	ASSERT_TRUE(conf->Init(0, 8000));

	const int partId = conf->CreateParticipant(0, 0, L"alice", Participant::RTP, true);
	ASSERT_GT(partId, 0);
	int role = 0, sourceId = 0;
	ASSERT_TRUE(conf->CreateVideoStream(partId, role, sourceId));

	RTPMap audioMap, videoMap, extraMap, textMap;
	audioMap[0] = AudioCodec::PCMU;
	videoMap[96] = VideoCodec::VP8;
	extraMap[97] = VideoCodec::VP8;
	textMap[98] = TextCodec::T140;

	const int audioPort = conf->StartReceiving(partId, MediaFrame::Audio, audioMap);
	EXPECT_GT(audioPort, 0);
	EXPECT_EQ(conf->StartReceiving(partId, MediaFrame::Video, videoMap), audioPort);
	EXPECT_EQ(conf->StartReceiving(partId, MediaFrame::Video, extraMap, (MediaFrame::MediaRole)role), audioPort);
	EXPECT_EQ(conf->StartReceiving(partId, MediaFrame::Text, textMap), audioPort);

	EXPECT_TRUE(conf->DeleteParticipant(partId));
	conf.reset();
	mcu.End();
}

}
