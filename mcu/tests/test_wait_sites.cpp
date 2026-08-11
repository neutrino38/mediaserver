/**
 * test_wait_sites.cpp — caractérisation des sites qui réimplémentent le motif
 * d'attente à la main (wait-primitive-unification), AVANT leur conversion vers
 * les primitives Wait/WaitQueue. Ces tests fixent le comportement observable
 * par l'API publique ; ils doivent passer À L'IDENTIQUE après conversion.
 *
 * Sites couverts (les plus simples/testables) :
 *   - XmlEventQueue (xmlstreaminghandler) : file d'événements du long-poll
 *     XML-RPC. SÉMANTIQUE PIÈGE à préserver : WaitForEvent rend true MÊME sur
 *     timeout (c'est ce qui déclenche le keep-alive « \r\n » du long-poll) ;
 *     false SEULEMENT si annulée à l'ENTRÉE — un Cancel pendant l'attente rend
 *     true une dernière fois, puis false à l'appel suivant.
 *   - VideoPipe : pont poussé/tiré entre mixeur et encodeur vidéo. SÉMANTIQUE
 *     PIÈGE : sur timeout sans nouvelle image, GrabFrame RELIVRE la dernière
 *     trame (gel d'image plutôt que famine — l'encodeur continue de cadencer) ;
 *     End() pendant un grab rend encore `last`, le NULL n'arrive qu'au grab
 *     suivant.
 *   - RTPMultiplexerSmoother : lissage de l'émission RTP — les paquets d'une
 *     trame sont émis étalés sur sa durée (SetSendingTime cumulés), observé
 *     ici par un Joinable::Listener factice.
 *
 * Les seuils temporels sont larges pour rester déterministes sous charge.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "xmlstreaminghandler.h"
#include "videopipe.h"
#include "../src/jsr309/RTPMultiplexerSmoother.h"
#include "medkit/audio.h"

namespace {

typedef std::chrono::steady_clock Clock;

static long ElapsedMs(const Clock::time_point& t0)
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
}

// =============================================================================
// XmlEventQueue — file d'événements du long-poll XML-RPC
// =============================================================================

// Événement instrumenté : compteur d'instances (possession par la file) +
// valeur xmlrpc entière lisible côté Peek.
struct FakeXmlEvent : public XmlEvent
{
	static inline std::atomic<int> alive{0};
	int id;
	explicit FakeXmlEvent(int id = 0) : id(id) { ++alive; }
	~FakeXmlEvent() override { --alive; }
	xmlrpc_value* GetXmlValue(xmlrpc_env* env) override { return xmlrpc_int_new(env, id); }
};

// Lit l'entier du premier événement ; -1 si file vide.
static int PeekInt(XmlEventQueue& q)
{
	xmlrpc_env env;
	xmlrpc_env_init(&env);
	xmlrpc_value* val = q.PeekXMLEvent(&env);
	int out = -1;
	if (val)
	{
		xmlrpc_read_int(&env, val, &out);
		xmlrpc_DECREF(val);
	}
	xmlrpc_env_clean(&env);
	return out;
}

// SÉMANTIQUE PIÈGE : true même sur timeout — c'est le déclencheur du
// keep-alive du long-poll (Peek rend alors NULL et le handler envoie "\r\n").
TEST(XmlEventQueueSite, WaitForEventTrueOnTimeout)
{
	XmlEventQueue q;
	Clock::time_point t0 = Clock::now();
	EXPECT_TRUE(q.WaitForEvent(150));
	long ms = ElapsedMs(t0);
	EXPECT_GE(ms, 100);
	EXPECT_LT(ms, 5000);
	EXPECT_EQ(PeekInt(q), -1);	// rien à lire : keep-alive
}

TEST(XmlEventQueueSite, EventFlowAndOwnership)
{
	FakeXmlEvent::alive = 0;
	{
		XmlEventQueue q;
		q.AddEvent(new FakeXmlEvent(7));
		q.AddEvent(new FakeXmlEvent(8));

		// Immédiat quand il y a un événement.
		Clock::time_point t0 = Clock::now();
		EXPECT_TRUE(q.WaitForEvent(5000));
		EXPECT_LT(ElapsedMs(t0), 1000);

		// Peek ne consomme pas ; Pop détruit (possession par la file).
		EXPECT_EQ(PeekInt(q), 7);
		EXPECT_EQ(PeekInt(q), 7);
		q.PopEvent();
		EXPECT_EQ(FakeXmlEvent::alive.load(), 1);
		EXPECT_EQ(PeekInt(q), 8);
		// Le destructeur libère l'événement restant.
	}
	EXPECT_EQ(FakeXmlEvent::alive.load(), 0);
}

TEST(XmlEventQueueSite, AddDuringWaitWakes)
{
	XmlEventQueue q;
	std::thread producer([&q]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		q.AddEvent(new FakeXmlEvent(42));
	});
	Clock::time_point t0 = Clock::now();
	EXPECT_TRUE(q.WaitForEvent(5000));
	EXPECT_LT(ElapsedMs(t0), 4000);
	EXPECT_EQ(PeekInt(q), 42);
	q.PopEvent();
	producer.join();
}

// SÉMANTIQUE PIÈGE : Cancel pendant l'attente → l'attente en cours rend
// encore true (le handler boucle une dernière fois), puis false à l'entrée
// suivante — c'est ainsi que le long-poll se termine proprement.
TEST(XmlEventQueueSite, CancelIsTwoStep)
{
	XmlEventQueue q;
	std::thread canceler([&q]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		q.Cancel();
	});
	Clock::time_point t0 = Clock::now();
	EXPECT_TRUE(q.WaitForEvent(5000));	// réveillée par Cancel : true
	EXPECT_LT(ElapsedMs(t0), 4000);
	canceler.join();
	EXPECT_FALSE(q.WaitForEvent(5000));	// annulée à l'entrée : false
}

// =============================================================================
// VideoPipe — pont mixeur → encodeur
// =============================================================================

TEST(VideoPipeSite, GrabWithoutInitFails)
{
	VideoPipe pipe;
	Clock::time_point t0 = Clock::now();
	EXPECT_EQ(pipe.GrabFrame(1000), nullptr);
	EXPECT_LT(ElapsedMs(t0), 500);	// échec immédiat, sans attendre
}

TEST(VideoPipeSite, DeliversFrameImmediately)
{
	VideoPipe pipe;
	pipe.Init();
	pipe.StartVideoCapture(320, 240, 30);

	EXPECT_TRUE(pipe.NextFrame(Pict::CreateBlack(320, 240)));
	Clock::time_point t0 = Clock::now();
	PictPtr pic = pipe.GrabFrame(5000);
	ASSERT_NE(pic, nullptr);
	EXPECT_LT(ElapsedMs(t0), 1000);
	EXPECT_EQ(pic->GetWidth(), 320);
	EXPECT_EQ(pic->GetHeight(), 240);
	pipe.End();
}

// SÉMANTIQUE PIÈGE (gel d'image) : sans nouvelle trame, GrabFrame attend son
// timeout puis RELIVRE la dernière trame — l'encodeur continue de cadencer
// sur l'image gelée au lieu d'être affamé.
TEST(VideoPipeSite, TimeoutRedeliversLastFrame)
{
	VideoPipe pipe;
	pipe.Init();
	pipe.StartVideoCapture(320, 240, 30);
	pipe.NextFrame(Pict::CreateBlack(320, 240));
	PictPtr first = pipe.GrabFrame(5000);
	ASSERT_NE(first, nullptr);

	Clock::time_point t0 = Clock::now();
	PictPtr again = pipe.GrabFrame(150);	// personne ne pousse
	long ms = ElapsedMs(t0);
	ASSERT_NE(again, nullptr);		// la MÊME image, pas un échec
	EXPECT_EQ(again, first);
	EXPECT_GE(ms, 100);
	EXPECT_LT(ms, 5000);
	pipe.End();
}

TEST(VideoPipeSite, NextFrameDuringGrabWakes)
{
	VideoPipe pipe;
	pipe.Init();
	pipe.StartVideoCapture(320, 240, 30);

	std::thread producer([&pipe]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		pipe.NextFrame(Pict::CreateBlack(320, 240));
	});
	Clock::time_point t0 = Clock::now();
	PictPtr pic = pipe.GrabFrame(5000);
	EXPECT_NE(pic, nullptr);
	EXPECT_LT(ElapsedMs(t0), 4000);
	producer.join();
	pipe.End();
}

TEST(VideoPipeSite, CancelGrabReturnsNull)
{
	VideoPipe pipe;
	pipe.Init();
	pipe.StartVideoCapture(320, 240, 30);

	std::thread canceler([&pipe]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		pipe.CancelGrabFrame();
	});
	Clock::time_point t0 = Clock::now();
	EXPECT_EQ(pipe.GrabFrame(5000), nullptr);	// Cancel vide `last`
	EXPECT_LT(ElapsedMs(t0), 4000);
	canceler.join();
	pipe.End();
}

// End() pendant un grab : le grab en cours rend encore `last` (la dernière
// image), et c'est le grab SUIVANT qui échoue immédiatement.
TEST(VideoPipeSite, EndUnblocksGrabThenFails)
{
	VideoPipe pipe;
	pipe.Init();
	pipe.StartVideoCapture(320, 240, 30);
	pipe.NextFrame(Pict::CreateBlack(320, 240));
	PictPtr first = pipe.GrabFrame(5000);
	ASSERT_NE(first, nullptr);

	std::thread ender([&pipe]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		pipe.End();
	});
	Clock::time_point t0 = Clock::now();
	PictPtr pic = pipe.GrabFrame(5000);
	EXPECT_LT(ElapsedMs(t0), 4000);
	EXPECT_EQ(pic, first);			// relivre la dernière image
	ender.join();

	EXPECT_EQ(pipe.GrabFrame(1000), nullptr);	// plus inité : échec direct
}

// =============================================================================
// RTPMultiplexerSmoother — lissage de l'émission RTP
// =============================================================================

// Collecteur : horodate chaque paquet reçu.
struct PacketCollector : public Joinable::Listener
{
	std::mutex m;
	std::vector<long> arrivals;
	Clock::time_point t0;
	void onRTPPacket(RTPPacket&) override
	{
		std::lock_guard<std::mutex> g(m);
		arrivals.push_back(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count());
	}
	void onResetStream() override {}
	void onEndStream() override {}
	size_t Count()
	{
		std::lock_guard<std::mutex> g(m);
		return arrivals.size();
	}
	long Arrival(size_t i)
	{
		std::lock_guard<std::mutex> g(m);
		return arrivals[i];
	}
};

// Une trame de 3 paquets étalée sur 300 ms : les paquets sortent espacés
// (~100 ms), pas en rafale.
TEST(RtpSmootherSite, PacesPacketsOverFrameDuration)
{
	RTPMultiplexerSmoother smoother;
	PacketCollector collector;
	smoother.AddListener(&collector);
	ASSERT_TRUE(smoother.Start());

	AudioFrame frame(AudioCodec::PCMU, 8000);
	BYTE payload[480] = {0};
	frame.SetMedia(payload, sizeof(payload));
	frame.SetTimestamp(0);
	frame.AddRtpPacket(0,   160, NULL, 0, false);
	frame.AddRtpPacket(160, 160, NULL, 0, false);
	frame.AddRtpPacket(320, 160, NULL, 0, true);

	collector.t0 = Clock::now();
	ASSERT_TRUE(smoother.SmoothFrame(&frame, 300));

	// Attendre les 3 paquets (généreux).
	Clock::time_point t0 = Clock::now();
	while (collector.Count() < 3 && ElapsedMs(t0) < 3000)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	ASSERT_EQ(collector.Count(), (size_t)3);

	// Étalement : chaque paquet ~100 ms après le précédent, pas en rafale.
	long gap1 = collector.Arrival(1) - collector.Arrival(0);
	long gap2 = collector.Arrival(2) - collector.Arrival(1);
	EXPECT_GE(gap1, 30);
	EXPECT_LT(gap1, 500);
	EXPECT_GE(gap2, 30);
	EXPECT_LT(gap2, 500);

	smoother.Stop();
	smoother.RemoveListener(&collector);
}

// L'arrêt est borné : Stop() pendant l'attente de file vide revient vite.
TEST(RtpSmootherSite, StopIsResponsive)
{
	RTPMultiplexerSmoother smoother;
	ASSERT_TRUE(smoother.Start());
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	Clock::time_point t0 = Clock::now();
	smoother.Stop();
	EXPECT_LT(ElapsedMs(t0), 1500);	// legacy : réveil par Cancel + msleep(200)
}

} // namespace
