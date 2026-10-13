/**
 * test_jsr309_multi_video.cpp — plusieurs flux vidéo et BUNDLE sur un Endpoint
 * JSR-309.
 *
 * Conception : docs/conception/UNIFIED-PLAN/SPEC.md §4.6, lot 3. Ces tests
 * figent :
 *   - un flux de rôle >= 2 a sa jambe, sur son propre port sans BUNDLE ;
 *   - un flux supprimé n'existe plus, et son rôle n'est pas réattribué ;
 *   - une vidéo ne passe pas en WebSocket : refus, pas d'exception ;
 *   - un B2BUA en BUNDLE relaie flux à flux : la vidéo de rang 2 d'un endpoint
 *     arrive sur la vidéo de rang 2 de l'autre, et nulle part ailleurs.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include "../src/jsr309/JSR309Manager.h"
#include "../src/jsr309/MediaSession.h"
#include "rtp.h"
#include "rtpsession.h"
#include "xmlstreaminghandler.h"

//Les handlers XML-RPC, appelés tels que le serveur les appelle.
xmlrpc_value* EndpointCreateVideoStream(xmlrpc_env *env, xmlrpc_value *param_array, void *user_data);
xmlrpc_value* EndpointStartReceiving(xmlrpc_env *env, xmlrpc_value *param_array, void *user_data);

namespace {

const char* const kMidUri = "urn:ietf:params:rtp-hdrext:sdes:mid";
const MediaFrame::MediaRole kExtra = MediaFrame::VIDEO_EXTRA;

class NullListener : public RTPSession::Listener
{
public:
	void onFPURequested(RTPSession*) override {}
	void onReceiverEstimatedMaxBitrate(RTPSession*, DWORD) override {}
	void onTempMaxMediaStreamBitrateRequest(RTPSession*, DWORD, DWORD) override {}
};

Properties LegProperties(const char* mid)
{
	Properties props;
	props["rtcp-mux"] = "1";
	props[kMidUri] = "4";
	props["mid"] = mid;
	return props;
}

RTPMap MapOf(BYTE pt, BYTE codec)
{
	RTPMap map;
	map[pt] = codec;
	return map;
}

// Le navigateur en face d'un endpoint : une porteuse audio, la caméra et
// l'écran, un seul port.
class Browser
{
public:
	Browser()
		: audio(MediaFrame::Audio, &listener),
		  video(MediaFrame::Video, &listener),
		  video2(MediaFrame::Video, &listener, kExtra)
	{
	}

	~Browser()
	{
		if (!ok)
			return;
		video2.End();
		video.End();
		audio.End();
	}

	bool Init()
	{
		if (!video.JoinCarrier(&audio) || !video2.JoinCarrier(&audio))
			return false;
		ok = audio.Init() && video.Init() && video2.Init();
		if (!ok)
			return false;
		Setup(audio, 0, AudioCodec::PCMU, "0");
		Setup(video, 96, VideoCodec::VP8, "1");
		Setup(video2, 96, VideoCodec::VP8, "2");
		return true;
	}

	NullListener listener;
	RTPSession audio;
	RTPSession video;
	RTPSession video2;
	bool ok = false;

private:
	static void Setup(RTPSession& session, BYTE pt, BYTE codec, const char* mid)
	{
		RTPMap map = MapOf(pt, codec);
		session.SetSendingRTPMap(map);
		session.SetReceivingRTPMap(map);
		session.SetProperties(LegProperties(mid));
	}
};

void SendVideo(RTPSession& session, int count = 10)
{
	for (int i = 0; i < count; ++i)
	{
		DWORD codec = VideoCodec::VP8;
		RTPPacket packet(MediaFrame::Video, codec, codec);
		memset(packet.GetMediaData(), 0xAA, 16);
		packet.SetMediaLength(16);
		session.SendPacket(packet, i * 3000);
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
}

bool Receives(RTPSession& session, int timeoutMs = 1500)
{
	for (int waited = 0; waited < timeoutMs; waited += 100)
	{
		RTPPacket* packet = session.GetPacket(0, 100);
		if (packet)
		{
			delete packet;
			return true;
		}
	}
	return false;
}

class Jsr309MultiVideo : public ::testing::Test
{
protected:
	void SetUp() override
	{
		ASSERT_TRUE(manager.Init(&handler, 0));
		sessionId = manager.CreateMediaSession(L"multi-video", 0);
		ASSERT_GT(sessionId, 0);
		ASSERT_TRUE(manager.GetMediaSessionRef(sessionId, session));
	}

	void TearDown() override
	{
		session.reset();
		manager.DeleteMediaSession(sessionId);
		manager.End();
	}

	// Configure une jambe comme le ferait le contrôleur, section par section, et
	// la tourne vers `peerPort`. Rend le port annoncé.
	int Leg(int endpointId, MediaFrame::Type media, MediaFrame::MediaRole role, const char* mid,
		BYTE pt, BYTE codec, int peerPort)
	{
		session->EndpointSetRTPProperties(endpointId, media, LegProperties(mid), role);
		RTPMap map = MapOf(pt, codec);
		std::map<int,std::string> fmtp;
		const int port = session->EndpointStartReceiving(endpointId, media, map, fmtp, NULL, role);
		char ip[] = "127.0.0.1";
		RTPMap send = MapOf(pt, codec);
		session->EndpointStartSending(endpointId, media, ip, peerPort, send, role);
		return port;
	}

	XmlStreamingHandler handler;
	JSR309Manager manager;
	int sessionId = 0;
	std::shared_ptr<MediaSession> session;
};

TEST_F(Jsr309MultiVideo, UnFluxSupplementaireASaJambe)
{
	const int ep = session->EndpointCreate(L"a", true, true, false);
	ASSERT_GT(ep, 0);
	EXPECT_EQ(session->EndpointCreateVideoStream(ep), (int)kExtra);

	RTPMap map = MapOf(96, VideoCodec::VP8);
	std::map<int,std::string> fmtp;
	const int main = session->EndpointStartReceiving(ep, MediaFrame::Video, map, fmtp, NULL, MediaFrame::VIDEO_MAIN);
	RTPMap map2 = MapOf(96, VideoCodec::VP8);
	const int extra = session->EndpointStartReceiving(ep, MediaFrame::Video, map2, fmtp, NULL, kExtra);

	EXPECT_GT(main, 0);
	EXPECT_GT(extra, 0);
	EXPECT_NE(main, extra) << "sans BUNDLE, chaque flux a son port";
}

TEST_F(Jsr309MultiVideo, UnFluxSupprimeNExistePlusEtSonRoleNeRevientPas)
{
	const int ep = session->EndpointCreate(L"a", true, true, false);
	ASSERT_EQ(session->EndpointCreateVideoStream(ep), (int)kExtra);

	EXPECT_FALSE(session->EndpointDeleteVideoStream(ep, MediaFrame::VIDEO_MAIN));
	EXPECT_FALSE(session->EndpointDeleteVideoStream(ep, MediaFrame::VIDEO_SLIDES));
	EXPECT_TRUE(session->EndpointDeleteVideoStream(ep, kExtra));
	EXPECT_FALSE(session->EndpointDeleteVideoStream(ep, kExtra));

	RTPMap map = MapOf(96, VideoCodec::VP8);
	std::map<int,std::string> fmtp;
	EXPECT_LE(session->EndpointStartReceiving(ep, MediaFrame::Video, map, fmtp, NULL, kExtra), 0);

	EXPECT_EQ(session->EndpointCreateVideoStream(ep), (int)kExtra + 1);
}

// Une vidéo ne passe pas en WebSocket : c'est un refus, pas une exception qui
// termine le processus depuis le fil XML-RPC.
TEST_F(Jsr309MultiVideo, UneVideoEnWebSocketEstRefusee)
{
	const int ep = session->EndpointCreate(L"a", true, true, false);
	std::shared_ptr<Endpoint> endpoint = session->GetEndpoint(ep);
	ASSERT_TRUE((bool)endpoint);

	EXPECT_FALSE(endpoint->ConfigureMediaConnection(MediaFrame::Video, MediaFrame::VIDEO_SLIDES, MediaFrame::WS, NULL));
	EXPECT_FALSE(endpoint->ConfigureMediaConnection(MediaFrame::Video, MediaFrame::VIDEO_MAIN, MediaFrame::WS, NULL));
}

// Le premier entier de returnVal d'une réponse réussie, -1 sinon.
int FirstReturned(xmlrpc_env* env, xmlrpc_value* response)
{
	int result = -1;
	if (!response || env->fault_occurred)
	{
		if (response)
			xmlrpc_DECREF(response);
		xmlrpc_env_clean(env);
		xmlrpc_env_init(env);
		return result;
	}
	int code = 0;
	xmlrpc_value* val = NULL;
	xmlrpc_parse_value(env, response, "{s:i,s:A,*}", "returnCode", &code, "returnVal", &val);
	if (!env->fault_occurred && code == 1 && xmlrpc_array_size(env, val) > 0)
	{
		xmlrpc_value* first = NULL;
		xmlrpc_array_read_item(env, val, 0, &first);
		xmlrpc_read_int(env, first, &result);
		xmlrpc_DECREF(first);
	}
	xmlrpc_DECREF(response);
	if (env->fault_occurred)
		result = -1;
	//Le serveur donne un env neuf à chaque requête : le test aussi.
	xmlrpc_env_clean(env);
	xmlrpc_env_init(env);
	return result;
}

TEST_F(Jsr309MultiVideo, LeRoleEnDernierArgumentViseLeFlux)
{
	const int ep = session->EndpointCreate(L"a", true, true, false);
	xmlrpc_env env;
	xmlrpc_env_init(&env);

	xmlrpc_value* create = xmlrpc_build_value(&env, "(ii)", sessionId, ep);
	EXPECT_EQ(FirstReturned(&env, EndpointCreateVideoStream(&env, create, &manager)), (int)kExtra);
	xmlrpc_DECREF(create);

	xmlrpc_value* map = xmlrpc_build_value(&env, "{s:i}", "96", (int)VideoCodec::VP8);
	xmlrpc_value* offer = xmlrpc_struct_new(&env);

	xmlrpc_value* withRole = xmlrpc_build_value(&env, "(iiiSSsi)", sessionId, ep, (int)MediaFrame::Video, map, offer, "", (int)kExtra);
	xmlrpc_value* without = xmlrpc_build_value(&env, "(iiiSSs)", sessionId, ep, (int)MediaFrame::Video, map, offer, "");
	xmlrpc_value* unknown = xmlrpc_build_value(&env, "(iiiSSsi)", sessionId, ep, (int)MediaFrame::Video, map, offer, "", 9);

	const int extraPort = FirstReturned(&env, EndpointStartReceiving(&env, withRole, &manager));
	const int mainPort = FirstReturned(&env, EndpointStartReceiving(&env, without, &manager));
	EXPECT_GT(extraPort, 0);
	EXPECT_GT(mainPort, 0);
	EXPECT_NE(extraPort, mainPort) << "le rôle en dernier argument vise le flux 2, son absence la caméra";
	EXPECT_LE(FirstReturned(&env, EndpointStartReceiving(&env, unknown, &manager)), 0) << "un rôle inconnu est une erreur";

	//Deux arguments de trop : une erreur, pas un rôle deviné.
	xmlrpc_value* tooLong = xmlrpc_build_value(&env, "(iiiSSsii)", sessionId, ep, (int)MediaFrame::Video, map, offer, "", (int)kExtra, 7);
	EXPECT_LE(FirstReturned(&env, EndpointStartReceiving(&env, tooLong, &manager)), 0);
	xmlrpc_DECREF(tooLong);

	xmlrpc_DECREF(withRole);
	xmlrpc_DECREF(without);
	xmlrpc_DECREF(unknown);
	xmlrpc_DECREF(map);
	xmlrpc_DECREF(offer);
	xmlrpc_env_clean(&env);
}

TEST_F(Jsr309MultiVideo, EnBundleLeRangDeuxArriveAuRangDeux)
{
	Browser alice, bob;
	if (!alice.Init() || !bob.Init())
		GTEST_SKIP() << "impossible de binder une paire de ports RTP";

	const int a = session->EndpointCreate(L"a", true, true, false, true);
	const int b = session->EndpointCreate(L"b", true, true, false, true);
	ASSERT_GT(a, 0);
	ASSERT_GT(b, 0);
	ASSERT_EQ(session->EndpointCreateVideoStream(a), (int)kExtra);
	ASSERT_EQ(session->EndpointCreateVideoStream(b), (int)kExtra);

	const int aPort = Leg(a, MediaFrame::Audio, MediaFrame::VIDEO_MAIN, "0", 0, AudioCodec::PCMU, alice.audio.GetLocalPort());
	EXPECT_EQ(Leg(a, MediaFrame::Video, MediaFrame::VIDEO_MAIN, "1", 96, VideoCodec::VP8, alice.audio.GetLocalPort()), aPort);
	EXPECT_EQ(Leg(a, MediaFrame::Video, kExtra, "2", 96, VideoCodec::VP8, alice.audio.GetLocalPort()), aPort);
	const int bPort = Leg(b, MediaFrame::Audio, MediaFrame::VIDEO_MAIN, "0", 0, AudioCodec::PCMU, bob.audio.GetLocalPort());
	EXPECT_EQ(Leg(b, MediaFrame::Video, MediaFrame::VIDEO_MAIN, "1", 96, VideoCodec::VP8, bob.audio.GetLocalPort()), bPort);
	EXPECT_EQ(Leg(b, MediaFrame::Video, kExtra, "2", 96, VideoCodec::VP8, bob.audio.GetLocalPort()), bPort);

	char ip[] = "127.0.0.1";
	alice.audio.SetRemotePort(ip, aPort);
	bob.audio.SetRemotePort(ip, bPort);

	//Le B2BUA : chaque flux de A vers le flux de même rang de B.
	ASSERT_TRUE(session->EndpointAttachToEndpoint(b, a, MediaFrame::Video, MediaFrame::VIDEO_MAIN));
	ASSERT_TRUE(session->EndpointAttachToEndpoint(b, a, MediaFrame::Video, kExtra));

	SendVideo(alice.video2);

	EXPECT_TRUE(Receives(bob.video2)) << "l'écran d'Alice doit arriver sur l'écran de Bob";
	EXPECT_FALSE(Receives(bob.video, 300)) << "et pas sur sa caméra";
}

}
