/**
 * test_rtp_stream_lock.cpp — le réacteur ne doit jamais attendre un
 * consommateur parqué.
 *
 * `ChangeStream`, `AddStream` et `SetDefaultStream` s'exécutent sur le thread du
 * réacteur, sous le verrou ÉCRIVAIN `streamUse`. `GetPacket` s'exécute sur un
 * thread consommateur, sous le verrou LECTEUR : il ne doit le tenir que le
 * temps de trouver son flux, jamais pendant son attente bornée
 * (`ConsumerPollMs`). Sinon un changement de SSRC fait attendre le réacteur, et
 * toutes les jambes de son groupe avec lui (docs/reference/threads-rtp.md) ;
 * avec deux consommateurs sur la même session (partage de document BFCP), le
 * passage de témoin de `use.h` ne suffit même plus, et le renommage échoue sur
 * sa borne à CHAQUE paquet.
 *
 * Aucune socket : les entrées publiques `SetDefaultStream`, `ChangeStream` et
 * `GetPacket` suffisent à rejouer la course.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "rtpsession.h"

namespace {

typedef std::chrono::steady_clock Clock;

static long ElapsedMs(const Clock::time_point& t0)
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
}

class StubListener : public RTPSession::Listener
{
public:
	void onFPURequested(RTPSession*) override {}
	void onReceiverEstimatedMaxBitrate(RTPSession*, DWORD) override {}
	void onTempMaxMediaStreamBitrateRequest(RTPSession*, DWORD, DWORD) override {}
};

const DWORD kSsrcA = 0xA0000001;
const DWORD kSsrcB = 0xB0000002;

// Un consommateur parqué sur le flux par défaut, comme RecVideo entre deux
// paquets : IncUse, attente bornée de ConsumerPollMs, DecUse, et on recommence.
class ParkedConsumer
{
public:
	explicit ParkedConsumer(RTPSession& session) : session(session) {}

	void Start()
	{
		thread = std::thread([this] {
			while (!stop)
			{
				RTPPacket* packet = session.GetPacket(0, RTPSession::ConsumerPollMs);
				delete packet;
				++rounds;
			}
		});
	}

	void Stop()
	{
		stop = true;
		session.CancelGetPacket();
		thread.join();
	}

	std::atomic<int> rounds{0};

private:
	RTPSession&		session;
	std::atomic<bool>	stop{false};
	std::thread		thread;
};

// UN consommateur parqué : le renommage doit aboutir tout de suite, pas au
// prochain réveil du consommateur. Avant le correctif, ChangeStream attendait
// le reste de la fenêtre de ConsumerPollMs (jusqu'à 200 ms).
TEST(RTPStreamLock, LeRenommageNAttendPasUnConsommateurParque)
{
	StubListener listener;
	RTPSession   session(MediaFrame::Video, &listener);

	ASSERT_TRUE(session.SetDefaultStream(true, kSsrcA));
	ASSERT_EQ(session.GetDefaultStream(true), kSsrcA);

	ParkedConsumer consumer(session);
	consumer.Start();

	long worst = 0;
	DWORD from = kSsrcA, to = kSsrcB;
	for (int round = 0; round < 10; ++round)
	{
		//Le consommateur vient d'entrer dans son attente : c'est le pire moment.
		std::this_thread::sleep_for(std::chrono::milliseconds(20));

		Clock::time_point t0 = Clock::now();
		bool renamed = session.ChangeStream(from, to);
		long ms = ElapsedMs(t0);

		EXPECT_TRUE(renamed) << "tour " << round;
		EXPECT_EQ(session.GetDefaultStream(true), to);
		if (ms > worst)
			worst = ms;
		std::swap(from, to);
	}

	consumer.Stop();

	EXPECT_LT(worst, 50) << "le reacteur a attendu le consommateur : " << worst << " ms";
}

// DEUX consommateurs sur la même session, comme MAIN et SLIDES en partage de
// document. Leurs attentes se chevauchent sans trou : le compteur de lecteurs
// ne retombe jamais à zéro, et le passage de témoin ne peut rien pour
// l'écrivain, qui expire sur sa borne (StreamLockTimeoutMs) et ne renomme
// pas. C'est le cas que le BUNDLE rend courant.
TEST(RTPStreamLock, LeRenommageAboutitMalgreDeuxConsommateurs)
{
	StubListener listener;
	RTPSession   session(MediaFrame::Video, &listener);

	ASSERT_TRUE(session.SetDefaultStream(true, kSsrcA));

	ParkedConsumer first(session), second(session);
	first.Start();
	//Décalé d'une demi-fenêtre : les deux attentes se recouvrent toujours.
	std::this_thread::sleep_for(std::chrono::milliseconds(RTPSession::ConsumerPollMs / 2));
	second.Start();
	std::this_thread::sleep_for(std::chrono::milliseconds(20));

	int  failed = 0;
	long worst  = 0;
	DWORD from = kSsrcA, to = kSsrcB;
	for (int round = 0; round < 10; ++round)
	{
		Clock::time_point t0 = Clock::now();
		bool renamed = session.ChangeStream(from, to);
		long ms = ElapsedMs(t0);

		if (!renamed)
			++failed;
		else
			std::swap(from, to);
		if (ms > worst)
			worst = ms;
	}

	first.Stop();
	second.Stop();

	EXPECT_EQ(failed, 0) << "renommages echoues sur la borne du verrou";
	EXPECT_LT(worst, 50) << "pire attente de l'ecrivain : " << worst << " ms";
	EXPECT_GT(first.rounds.load(), 0);
	EXPECT_GT(second.rounds.load(), 0);
}

} // namespace
