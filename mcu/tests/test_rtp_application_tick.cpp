/**
 * test_rtp_application_tick.cpp — la cadence que RTPSession prête à un data channel.
 *
 * Une pile SCTP en espace utilisateur (usrsctp, mode sans thread) n'a pas de
 * thread à elle : ses retransmissions et ses heartbeats doivent être battus de
 * l'extérieur. C'est la boucle `poll` de `RTPSession` qui s'en charge, ce qui
 * garde tout le chemin d'un data channel sur un seul thread — l'objet `SSL`
 * n'étant pas concurrent. Voir docs/conception/T140-DC/SPEC.md §5.2.
 *
 * Deux propriétés, et rien de plus :
 *
 *  - un consommateur qui déclare une période est appelé à cette cadence, avec
 *    l'écoulement RÉEL en paramètre (le poll rend la main plus tôt sur un paquet
 *    entrant, plus tard sous charge) ;
 *  - un consommateur qui n'en déclare pas (le défaut) n'est JAMAIS appelé, et la
 *    session garde son attente infinie — c'est ce qui fait qu'une jambe RTP
 *    ordinaire ne se réveille pas pour rien.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "rtpsession.h"

namespace {

// La session exige un listener non nul.
class StubListener : public RTPSession::Listener
{
public:
	void onFPURequested(RTPSession*) override {}
	void onReceiverEstimatedMaxBitrate(RTPSession*, DWORD) override {}
	void onTempMaxMediaStreamBitrateRequest(RTPSession*, DWORD, DWORD) override {}
};

// Consommateur applicatif qui compte ses battements. `tickMs` à 0 = pas de
// cadence demandée, ce qui est le défaut de l'interface.
class CountingConsumer : public RTPSession::ApplicationListener
{
public:
	explicit CountingConsumer(DWORD tickMs) : tickMs(tickMs) {}

	DWORD GetApplicationTickMs() override { return tickMs; }

	void onApplicationTick(DWORD elapsedMs) override
	{
		ticks++;
		if (elapsedMs > maxElapsed)
			maxElapsed = elapsedMs;
	}

	void onDTLSApplicationData(const BYTE*, DWORD) override {}

	DWORD              tickMs;
	std::atomic<int>   ticks{0};
	std::atomic<DWORD> maxElapsed{0};
};

TEST(RTPApplicationTick, LaCadenceDemandeeEstBattue)
{
	StubListener     listener;
	CountingConsumer consumer(10);
	RTPSession       session(MediaFrame::Text, &listener);

	// À poser AVANT Init : c'est Init qui démarre la boucle.
	session.SetDTLSApplicationListener(&consumer);
	ASSERT_EQ(1, session.Init());

	std::this_thread::sleep_for(std::chrono::milliseconds(200));

	session.End();

	// 200 ms à 10 ms : on en attend une vingtaine. Le seuil est bas exprès —
	// c'est la cadence qu'on teste, pas l'ordonnanceur de la machine.
	EXPECT_GE(consumer.ticks.load(), 8);
	// L'écoulement passé au consommateur est le vrai : sur une boucle qui ne
	// reçoit rien, il doit rester du même ordre que la période demandée.
	EXPECT_LT(consumer.maxElapsed.load(), (DWORD)200);
}

TEST(RTPApplicationTick, SansCadenceDemandeeAucunBattement)
{
	StubListener     listener;
	CountingConsumer consumer(0);
	RTPSession       session(MediaFrame::Text, &listener);

	session.SetDTLSApplicationListener(&consumer);
	ASSERT_EQ(1, session.Init());

	std::this_thread::sleep_for(std::chrono::milliseconds(200));

	session.End();

	EXPECT_EQ(0, consumer.ticks.load());
}

} // namespace
