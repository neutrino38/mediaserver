/**
 * test_doc_sharing.cpp — le partage de document BFCP sur le modèle des flux
 * vidéo multiples : la parole pose la source de SLIDES du présentateur dans la
 * mosaïque partagée, et l'en retire.
 *
 * Conception : docs/conception/UNIFIED-PLAN/SPEC.md §4.3, lot 1b. La parole
 * est la vraie : un client BFCP en bouclage parle TCP à l'écoute de la
 * conférence, et la conférence l'accorde par AcceptDocSharingRequest, comme le
 * fait le contrôleur. Ces tests figent :
 *   - SetAppCodec(BFCP) donne au participant une source de SLIDES à lui ;
 *   - parole accordée à A → la source de A est dans la mosaïque partagée, et
 *     l'événement ACTIVE la nomme ;
 *   - parole passée à B → A en sort, B y entre ;
 *   - libération → plus aucune source de présentation ;
 *   - la mosaïque partagée reste retenue : un partage suivant s'y affiche sans
 *     nouvel appel à SetDocSharingMosaic (décision du 2026-09-30) ;
 *   - une jambe vidéo arrêtée repart par StartSending() sans argument.
 */
#include <gtest/gtest.h>

#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "bfcp/BFCPMessage.h"
#include "mcu.h"
#include "multiconf.h"
#include "rtpparticipant.h"
#include "xmlstreaminghandler.h"

namespace {

typedef std::chrono::steady_clock Clock;

template <class Pred>
bool WaitFor(Pred pred, long deadlineMs = 3000)
{
	const Clock::time_point t0 = Clock::now();
	while (! pred() && std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count() < deadlineMs)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	return pred();
}

bool InMosaic(MultiConf& conf, int mosaicId, int sourceId)
{
	std::list<int> positions;
	conf.GetMosaicPositions(mosaicId, positions);
	return std::find(positions.begin(), positions.end(), sourceId) != positions.end();
}

// Un endpoint BFCP réduit à ce que le test demande : se présenter, demander
// la parole, et lire ce que le serveur renvoie.
class BfcpClient
{
public:
	BfcpClient() : fd(-1) {}
	~BfcpClient() { if (fd >= 0) close(fd); }

	bool Connect(WORD port)
	{
		fd = socket(AF_INET6, SOCK_STREAM, 0);
		if (fd < 0)
			return false;
		sockaddr_in6 addr = {};
		addr.sin6_family = AF_INET6;
		addr.sin6_addr	 = in6addr_loopback;
		addr.sin6_port	 = htons(port);
		return connect(fd, (sockaddr*)&addr, sizeof(addr)) == 0;
	}

	bool Send(const BFCPMessage& msg)
	{
		BYTE buffer[4096];
		const size_t n = msg.Serialize(buffer, sizeof(buffer));
		if (n == BFCPMessage::Failed)
			return false;
		return ::send(fd, buffer, n, MSG_NOSIGNAL) == (ssize_t)n;
	}

	BFCPMessage* Receive(long deadlineMs = 3000)
	{
		const Clock::time_point t0 = Clock::now();
		for (;;) {
			if (in.size() >= BFCPMessage::HeaderLen) {
				const size_t total = BFCPMessage::HeaderLen + get2(&in[0], 2) * 4;
				if (in.size() >= total) {
					BFCPMessage* msg = BFCPMessage::Parse(&in[0], total);
					in.erase(in.begin(), in.begin() + total);
					return msg;
				}
			}
			if (std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count() >= deadlineMs)
				return nullptr;
			pollfd pfd = { fd, POLLIN, 0 };
			if (poll(&pfd, 1, 50) <= 0)
				continue;
			BYTE chunk[4096];
			const ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
			if (n <= 0)
				return nullptr;
			in.insert(in.end(), chunk, chunk + n);
		}
	}

	bool Hello(int confId, int userId)
	{
		if (! Send(BFCPMsgHello(1, confId, userId)))
			return false;
		std::unique_ptr<BFCPMessage> ack(Receive());
		return ack && ack->GetPrimitive() == BFCPMessage::HelloAck;
	}

	bool RequestFloor(int confId, int userId)
	{
		BFCPMsgFloorRequest request(20, confId, userId);
		request.AddFloorId(SharedDocMixer::FloorId);
		return Send(request);
	}

private:
	int			fd;
	std::vector<BYTE>	in;
};

// Capte les événements de partage à la place du MCU.
struct DocSharingEvents : public MultiConf::Listener
{
	std::mutex		mutex;
	std::wstring		lastStatus;
	int			lastPartId = 0;
	int			lastSourceId = 0;

	void onParticipantRequestFPU(MultiConf*, int) override {}
	void onParticipantRequestDocSharing(MultiConf*, int partId, std::wstring status, int sourceId) override
	{
		std::lock_guard<std::mutex> lock(mutex);
		lastStatus = status;
		lastPartId = partId;
		lastSourceId = sourceId;
	}
	bool Is(const wchar_t* status, int partId)
	{
		std::lock_guard<std::mutex> lock(mutex);
		return lastStatus == status && lastPartId == partId;
	}
};

class DocSharing : public ::testing::Test
{
protected:
	void SetUp() override
	{
		mcu.Init(&handler, 0);
		confId = mcu.CreateConference(L"doc-sharing", 0);
		ASSERT_GT(confId, 0);
		ASSERT_TRUE(mcu.GetConferenceRef(confId, conf));
		ASSERT_TRUE(conf->Init(0, 8000));
		conf->SetListener(&events);

		shared = conf->CreateMosaic(Mosaic::mosaic2x2, CIF);
		ASSERT_GT(shared, 0);

		alice = conf->CreateParticipant(0, 0, L"alice", Participant::RTP);
		bob = conf->CreateParticipant(0, 0, L"bob", Participant::RTP);
		ASSERT_GT(alice, 0);
		ASSERT_GT(bob, 0);

		port = JoinBfcp(alice);
		ASSERT_GT(port, 0);
		ASSERT_EQ(JoinBfcp(bob), port) << "une écoute TCP par conférence";

		ASSERT_TRUE(conf->SetDocSharingMosaic(shared));
	}

	void TearDown() override
	{
		conf->SetListener(NULL);
		conf.reset();
		mcu.End();
	}

	int JoinBfcp(int partId)
	{
		if (! conf->SetAppCodec(confId, partId, AppCodec::BFCP))
			return 0;
		RTPMap map;
		map[100] = AppCodec::BFCP;
		return conf->StartReceiving(partId, MediaFrame::Application, map, MediaFrame::VIDEO_MAIN, confId, MediaFrame::TCP);
	}

	// La demande arrive sur le thread du réacteur BFCP : l'accepter dès
	// qu'elle est connue, comme le ferait le contrôleur sur WAITING_ACCEPT.
	bool Share(BfcpClient& client, int partId)
	{
		if (! client.RequestFloor(confId, partId))
			return false;
		bool accepted = false;
		WaitFor([&]{ return accepted || (accepted = conf->AcceptDocSharingRequest(confId, partId) == 1); });
		return accepted;
	}

	XmlStreamingHandler	handler;
	MCU			mcu;
	DocSharingEvents	events;
	int			confId = 0;
	std::shared_ptr<MultiConf> conf;
	int			shared = 0;
	int			alice = 0;
	int			bob = 0;
	int			port = 0;
};

TEST_F(DocSharing, ChaqueParticipantBfcpASaSourceDeSlides)
{
	const int aliceSlides = conf->GetVideoStreamSource(alice, MediaFrame::VIDEO_SLIDES);
	const int bobSlides = conf->GetVideoStreamSource(bob, MediaFrame::VIDEO_SLIDES);

	EXPECT_GT(aliceSlides, 0);
	EXPECT_GT(bobSlides, 0);
	EXPECT_NE(aliceSlides, bobSlides);
	EXPECT_NE(aliceSlides, alice);
	EXPECT_FALSE(InMosaic(*conf, shared, aliceSlides)) << "personne n'a la parole";
	EXPECT_FALSE(conf->DeleteVideoStream(alice, MediaFrame::VIDEO_SLIDES));
}

TEST_F(DocSharing, LaParoleAccordeePoseLaSourceDansLaMosaiquePartagee)
{
	BfcpClient client;
	ASSERT_TRUE(client.Connect(port));
	ASSERT_TRUE(client.Hello(confId, alice));
	ASSERT_TRUE(Share(client, alice));

	const int aliceSlides = conf->GetVideoStreamSource(alice, MediaFrame::VIDEO_SLIDES);
	EXPECT_TRUE(WaitFor([&]{ return InMosaic(*conf, shared, aliceSlides); }));
	ASSERT_TRUE(WaitFor([&]{ return events.Is(L"ACTIVE", alice); }));
	EXPECT_EQ(events.lastSourceId, aliceSlides);
}

TEST_F(DocSharing, LaParolePasseeABRemplaceLaSourceDeA)
{
	BfcpClient a, b;
	ASSERT_TRUE(a.Connect(port));
	ASSERT_TRUE(a.Hello(confId, alice));
	ASSERT_TRUE(b.Connect(port));
	ASSERT_TRUE(b.Hello(confId, bob));

	const int aliceSlides = conf->GetVideoStreamSource(alice, MediaFrame::VIDEO_SLIDES);
	const int bobSlides = conf->GetVideoStreamSource(bob, MediaFrame::VIDEO_SLIDES);

	ASSERT_TRUE(Share(a, alice));
	ASSERT_TRUE(WaitFor([&]{ return InMosaic(*conf, shared, aliceSlides); }));

	ASSERT_TRUE(Share(b, bob));
	EXPECT_TRUE(WaitFor([&]{ return InMosaic(*conf, shared, bobSlides); }));
	EXPECT_FALSE(InMosaic(*conf, shared, aliceSlides));
}

TEST_F(DocSharing, LaLiberationRetireLaSourceEtGardeLaMosaique)
{
	BfcpClient client;
	ASSERT_TRUE(client.Connect(port));
	ASSERT_TRUE(client.Hello(confId, alice));

	const int aliceSlides = conf->GetVideoStreamSource(alice, MediaFrame::VIDEO_SLIDES);

	ASSERT_TRUE(Share(client, alice));
	ASSERT_TRUE(WaitFor([&]{ return InMosaic(*conf, shared, aliceSlides); }));

	ASSERT_TRUE(conf->StopDocSharing(confId, alice));
	EXPECT_TRUE(WaitFor([&]{ return ! InMosaic(*conf, shared, aliceSlides); }));
	EXPECT_TRUE(WaitFor([&]{ return events.Is(L"NONE", alice); }));

	//Aucun nouvel appel à SetDocSharingMosaic.
	ASSERT_TRUE(Share(client, alice));
	EXPECT_TRUE(WaitFor([&]{ return InMosaic(*conf, shared, aliceSlides); }));
}

TEST(VideoStreamRestart, UneJambeArreteeRepartSansArgument)
{
	std::shared_ptr<RTPParticipant> part = std::make_shared<RTPParticipant>(1, L"restart");
	ASSERT_TRUE(part->Init());

	RTPMap map;
	map[96] = VideoCodec::VP8;
	Properties none;
	ASSERT_TRUE(part->SetVideoCodec(VideoCodec::VP8, CIF, 15, 300, 0, none));

	char ip[] = "127.0.0.1";
	ASSERT_TRUE(part->StartSending(MediaFrame::Video, ip, 40000, map));
	ASSERT_TRUE(part->StopSending(MediaFrame::Video));

	EXPECT_TRUE(part->StartSending(MediaFrame::Video)) << "reprise avec l'adresse déjà connue";

	part->End();
}

}

namespace {

// Un SSRC inconnu sur une jambe d'un participant BFCP. Seule une jambe vidéo
// MAIN ou SLIDES porte un contenu BFCP ; toute autre suit la règle ordinaire,
// qui remplace l'ancien SSRC.
class BfcpNewStream : public ::testing::Test
{
protected:
	void SetUp() override
	{
		part = std::make_shared<RTPParticipant>(1, L"bfcp-new-stream");
		part->SetDocSharingMode(Participant::BFCP_TCP);
	}

	void TearDown() override
	{
		part->End();
	}

	std::shared_ptr<RTPParticipant> part;
};

TEST_F(BfcpNewStream, UnNouveauSsrcAudioRemplaceLAncien)
{
	RTPSession audio(MediaFrame::Audio, NULL);
	ASSERT_TRUE(audio.SetDefaultStream(true, 0x1111));

	part->onNewStream(&audio, 0x2222, true);

	EXPECT_EQ(audio.GetDefaultStream(true), 0x2222u) << "l'audio doit suivre le nouveau SSRC";
}

TEST_F(BfcpNewStream, UnNouveauSsrcSurMainResteLeDocument)
{
	RTPSession main(MediaFrame::Video, NULL);
	ASSERT_TRUE(main.SetDefaultStream(true, 0x1111));

	part->onNewStream(&main, 0x2222, true);

	EXPECT_EQ(main.GetDefaultStream(true), 0x1111u) << "la caméra garde son SSRC, le second est le document";
}

}
