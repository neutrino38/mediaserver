/**
 * test_video_start_fpu.cpp — un flux vidéo reçu demande une image clé dès son
 * premier paquet.
 *
 * Sans cette demande, un flux qui démarre sans image clé décodable attend
 * celle que l'émetteur voudra bien envoyer : 16 s d'écran vide constatées sur
 * un partage d'écran Chrome en BUNDLE (recette Unified Plan, 2026-10-01). Ici
 * le pair n'envoie que des paquets sans bit de marque : rien ne se décode,
 * rien n'échoue, aucun paquet ne manque. Ces tests figent :
 *   - le pair reçoit une demande d'image clé (RTCP) ;
 *   - le contrôleur n'en est pas averti (pas d'onRequestFPU).
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include "rtp.h"
#include "rtpsession.h"
#include "videostream.h"

namespace {

const DWORD kVp8Pt = 96;

class PeerListener : public RTPSession::Listener
{
public:
	void onFPURequested(RTPSession*) override { fpu++; }
	void onReceiverEstimatedMaxBitrate(RTPSession*, DWORD) override {}
	void onTempMaxMediaStreamBitrateRequest(RTPSession*, DWORD, DWORD) override {}

	std::atomic<int> fpu{0};
};

class StreamListener : public VideoStream::Listener
{
public:
	void onRequestFPU() override { controller++; }
	void onFPURequested(RTPSession*) override {}
	void onReceiverEstimatedMaxBitrate(RTPSession*, DWORD) override {}
	void onTempMaxMediaStreamBitrateRequest(RTPSession*, DWORD, DWORD) override {}

	std::atomic<int> controller{0};
};

Properties RtcpMux()
{
	Properties props;
	props["rtcp-mux"] = "1";
	return props;
}

TEST(VideoStartFpu, LePremierPaquetDemandeUneImageCle)
{
	StreamListener streamListener;
	PictPtr logo;
	VideoStream stream(&streamListener, logo);
	PeerListener peerListener;
	RTPSession peer(MediaFrame::Video, &peerListener);

	if (!stream.Init(NULL, NULL) || !peer.Init())
		GTEST_SKIP() << "impossible de binder une paire de ports RTP";

	RTPMap map;
	map[kVp8Pt] = VideoCodec::VP8;
	peer.SetSendingRTPMap(map);
	peer.SetReceivingRTPMap(map);
	peer.SetProperties(RtcpMux());
	stream.GetOwnSession().SetProperties(RtcpMux());

	char ip[] = "127.0.0.1";
	stream.GetOwnSession().SetRemotePort(ip, peer.GetLocalPort());
	peer.SetRemotePort(ip, stream.GetOwnSession().GetLocalPort());

	//Le premier paquet reçu d'une adresse nouvelle (accrochage NAT) lève déjà
	//onFPURequested chez le pair : seul compte ce qui vient après.
	for (int i = 0; i < 50 && peerListener.fpu.load() == 0; ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	const int latched = peerListener.fpu.load();

	ASSERT_GT(stream.StartReceiving(map), 0);

	for (int i = 0; i < 5; ++i)
	{
		const DWORD codec = VideoCodec::VP8;
		RTPPacket packet(MediaFrame::Video, codec, codec);
		BYTE* data = packet.GetMediaData();
		memset(data, 0xAA, 8);
		data[0] = 0x00;	//descripteur VP8 minimal, milieu de trame
		packet.SetMediaLength(8);
		packet.SetMark(false);
		peer.SendPacket(packet, 0);
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}

	for (int i = 0; i < 50 && peerListener.fpu.load() == latched; ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(20));

	EXPECT_GT(peerListener.fpu.load(), latched) << "aucune demande d'image clé au démarrage du flux";
	EXPECT_EQ(streamListener.controller.load(), 0) << "la demande initiale ne concerne pas le contrôleur";

	stream.StopReceiving();
	stream.End();
	peer.End();
}

}
