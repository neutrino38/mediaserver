/**
 * test_rtp_reactor.cpp — RTPSession battue par un RtpSessionSet (lot 2 du
 * chantier, docs/conception/RTP-REACTOR/SPEC.md).
 *
 * Ce que ces tests prouvent, et que la suite RtpSessionSet* ne peut pas prouver
 * seule : une `RTPSession` n'a plus de thread. Elle s'inscrit dans un réacteur à
 * `Init()`, s'en retire à `End()`, et N sessions n'ajoutent AUCUN thread au
 * processus. Plus le point fonctionnel qui décide de tout : le réacteur lit
 * vraiment le RTP, sans qu'un consommateur ait rien à réclamer.
 */
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <thread>
#include <vector>

#include "rtp.h"
#include "rtpsession.h"
#include "rtpsessionset.h"

namespace {

typedef std::chrono::steady_clock Clock;

static long ElapsedMs(const Clock::time_point& t0)
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
}

template <class Pred>
static bool WaitFor(Pred pred, long deadlineMs = 2000)
{
	Clock::time_point t0 = Clock::now();
	while (!pred() && ElapsedMs(t0) < deadlineMs)
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	return pred();
}

// Threads du processus, lus dans /proc : c'est LA mesure du chantier.
static int ThreadCount()
{
	DIR* dir = opendir("/proc/self/task");
	if (!dir)
		return -1;

	int n = 0;
	while (struct dirent* e = readdir(dir))
		if (e->d_name[0] != '.')
			++n;
	closedir(dir);
	return n;
}

// Listener minimal : la session en exige un non nul.
//
// onNewStream AJOUTE le flux au lieu de remplacer le flux par défaut. C'est ce
// que fait `RTPParticipant` en mode partage de document : la session de MAIN
// porte deux SSRC, et la jambe SLIDES lit le second.
class StubListener : public RTPSession::Listener
{
public:
	void onFPURequested(RTPSession*) override {}
	void onReceiverEstimatedMaxBitrate(RTPSession*, DWORD) override {}
	void onTempMaxMediaStreamBitrateRequest(RTPSession*, DWORD, DWORD) override {}

	void onNewStream(RTPSession* session, DWORD ssrc, bool receiving) override
	{
		if (receiving)
			session->AddStream(true, ssrc);
	}
};

// Session audio prête à recevoir du PCMU.
class Leg
{
public:
	Leg() : session(MediaFrame::Audio, &listener) {}

	bool Init(RtpSessionSet* group = NULL)
	{
		if (group && !session.SetPollGroup(group))
			return false;
		if (session.Init() != 1)
			return false;

		RTPMap map;
		map[0] = 0;			// payload type 0 -> PCMU
		session.SetReceivingRTPMap(map);
		session.SetSendingRTPMap(map);
		return true;
	}

	StubListener	listener;
	RTPSession	session;
};

// Émet un RTP minimal mais valide (V=2, 12 octets d'en-tête) en loopback.
static bool SendRtpTo(int port, DWORD ssrc = 0x0BADF00D)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return false;

	BYTE packet[12];
	memset(packet, 0, sizeof(packet));
	packet[0]  = 0x80;			// version 2
	packet[1]  = 0;				// payload type 0
	packet[3]  = 1;				// seq
	packet[8]  = (BYTE)(ssrc >> 24);
	packet[9]  = (BYTE)(ssrc >> 16);
	packet[10] = (BYTE)(ssrc >> 8);
	packet[11] = (BYTE)ssrc;

	sockaddr_in to;
	memset(&to, 0, sizeof(to));
	to.sin_family		= AF_INET;
	to.sin_addr.s_addr	= htonl(INADDR_LOOPBACK);
	to.sin_port		= htons(port);

	bool ok = sendto(fd, packet, sizeof(packet), 0, (sockaddr*)&to, sizeof(to))
			== (ssize_t)sizeof(packet);
	close(fd);
	return ok;
}

// LE test du lot : des sessions en plus ne sont plus des threads en plus.
TEST(RtpReactor, ExtraSessionsCostNoExtraThread)
{
	// Le réacteur par défaut est créé à la première demande : une première
	// session peut donc coûter le thread du groupe, les suivantes non.
	Leg first;
	ASSERT_TRUE(first.Init());

	int baseline = ThreadCount();
	ASSERT_GT(baseline, 0);

	{
		std::vector<Leg*> legs;
		for (int i = 0; i < 6; ++i)
		{
			Leg* leg = new Leg();
			ASSERT_TRUE(leg->Init());
			legs.push_back(leg);
		}

		// Six jambes de plus : le compte de threads ne bouge pas. Avant ce lot,
		// c'étaient six threads.
		EXPECT_EQ(ThreadCount(), baseline);

		for (size_t i = 0; i < legs.size(); ++i)
			delete legs[i];
	}

	EXPECT_EQ(ThreadCount(), baseline);
}

// Init inscrit, End retire, le destructeur retire aussi (il passe par End).
TEST(RtpReactor, InitRegistersAndEndDeregisters)
{
	RtpSessionSet group("test-reactor-lifecycle");
	ASSERT_TRUE(group.Start());

	{
		Leg leg;
		ASSERT_TRUE(leg.Init(&group));
		EXPECT_EQ(group.GetHandlerCount(), 1u);

		leg.session.End();
		EXPECT_EQ(group.GetHandlerCount(), 0u);

		// End est idempotent : un second appel ne retire pas deux fois.
		leg.session.End();
		EXPECT_EQ(group.GetHandlerCount(), 0u);
	}

	{
		Leg leg;
		ASSERT_TRUE(leg.Init(&group));
		EXPECT_EQ(group.GetHandlerCount(), 1u);
	}	// destruction sans End explicite

	EXPECT_EQ(group.GetHandlerCount(), 0u)
		<< "le destructeur doit retirer la session de son groupe";

	group.Stop();
}

// Le groupe posé est celui qui bat la session — pas celui du processus.
TEST(RtpReactor, AnExplicitGroupIsUsedInsteadOfTheDefaultOne)
{
	RtpSessionSet mine("test-reactor-explicit");
	ASSERT_TRUE(mine.Start());

	DWORD defaultBefore = RtpSessionSet::Default().GetHandlerCount();

	{
		Leg leg;
		ASSERT_TRUE(leg.Init(&mine));
		EXPECT_EQ(mine.GetHandlerCount(), 1u);
		EXPECT_EQ(RtpSessionSet::Default().GetHandlerCount(), defaultBefore);
	}

	EXPECT_EQ(mine.GetHandlerCount(), 0u);
	mine.Stop();
}

// Après Init, changer de groupe ferait retirer End() d'un groupe où personne n'a
// jamais inscrit la session : c'est refusé, pas silencieusement accepté.
TEST(RtpReactor, ChangingGroupAfterInitIsRefused)
{
	RtpSessionSet first("test-reactor-first");
	RtpSessionSet second("test-reactor-second");
	ASSERT_TRUE(first.Start());

	Leg leg;
	ASSERT_TRUE(leg.Init(&first));
	EXPECT_EQ(first.GetHandlerCount(), 1u);

	EXPECT_FALSE(leg.session.SetPollGroup(&second));
	EXPECT_EQ(first.GetHandlerCount(), 1u);
	EXPECT_EQ(second.GetHandlerCount(), 0u);

	leg.session.End();
	first.Stop();
}

// Le point fonctionnel : le réacteur LIT le RTP. Personne n'a réclamé de paquet
// avant qu'il n'arrive — c'est la pompe continue, celle qui amorce aussi ICE et
// DTLS et qui sert le RTCP d'une session sendonly.
TEST(RtpReactor, TheReactorReallyReadsIncomingRtp)
{
	RtpSessionSet group("test-reactor-read");
	ASSERT_TRUE(group.Start());

	Leg leg;
	ASSERT_TRUE(leg.Init(&group));

	const int port = leg.session.GetLocalPort();
	ASSERT_GT(port, 0);
	ASSERT_TRUE(SendRtpTo(port));

	// GetPacket rend NULL tant que le flux n'existe pas : on boucle jusqu'à
	// l'échéance, comme le consommateur d'aujourd'hui.
	RTPPacket* packet = NULL;
	Clock::time_point t0 = Clock::now();
	while (!packet && ElapsedMs(t0) < 2000)
		packet = leg.session.GetPacket();

	ASSERT_NE(packet, nullptr) << "le reacteur n'a pas lu le paquet RTP";
	EXPECT_EQ(packet->GetSSRC(), 0x0BADF00Du);
	EXPECT_EQ(packet->GetSeqNum(), 1);
	delete packet;

	leg.session.End();
	group.Stop();
}

// Deux consommateurs sur UNE session, chacun sur son SSRC : c'est le partage de
// document BFCP, où la jambe vidéo SLIDES lit la session de MAIN. L'invariant
// « une seule pompe par session » que supposait la conception n°1 était donc
// déjà faux ; le réacteur le règle en étant LUI la pompe.
//
// Ce cas n'est pas recettable en appel faute de client BFCP sous la main : il est
// figé ici, et c'est la seule preuve qu'on en a.
TEST(RtpReactor, TwoConsumersOnOneSessionEachGetItsOwnSsrc)
{
	const DWORD kMain   = 0xA0000001;
	const DWORD kSlides = 0xB0000002;

	RtpSessionSet group("test-reactor-two-ssrc");
	ASSERT_TRUE(group.Start());

	Leg leg;
	ASSERT_TRUE(leg.Init(&group));

	const int port = leg.session.GetLocalPort();
	ASSERT_GT(port, 0);

	// Le 1er SSRC devient le flux par défaut, le 2e passe par onNewStream.
	ASSERT_TRUE(SendRtpTo(port, kMain));
	ASSERT_TRUE(WaitFor([&] { return leg.session.GetDefaultStream(true) == kMain; }));
	ASSERT_TRUE(SendRtpTo(port, kSlides));

	// Deux threads consommateurs, comme les deux VideoStream d'un participant.
	DWORD mainSsrc   = kMain;
	DWORD slidesSsrc = kSlides;
	RTPPacket* fromMain   = NULL;
	RTPPacket* fromSlides = NULL;

	std::thread a([&] {
		Clock::time_point t0 = Clock::now();
		while (!fromMain && ElapsedMs(t0) < 2000)
			fromMain = leg.session.GetPacket(mainSsrc);
	});
	std::thread b([&] {
		Clock::time_point t0 = Clock::now();
		while (!fromSlides && ElapsedMs(t0) < 2000)
			fromSlides = leg.session.GetPacket(slidesSsrc);
	});
	a.join();
	b.join();

	ASSERT_NE(fromMain, nullptr)   << "le flux par defaut n'a rien rendu";
	ASSERT_NE(fromSlides, nullptr) << "le second SSRC n'a rien rendu";

	// Chacun a eu SON flux, pas celui de l'autre.
	EXPECT_EQ(fromMain->GetSSRC(),   kMain);
	EXPECT_EQ(fromSlides->GetSSRC(), kSlides);

	delete fromMain;
	delete fromSlides;

	leg.session.End();
	group.Stop();
}

} // namespace
