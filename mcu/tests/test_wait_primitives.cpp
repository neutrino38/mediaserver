/**
 * test_wait_primitives.cpp — caractérisation des primitives de synchronisation
 * partagées (wait.h, waitqueue.h, rtpbuffer.h) AVANT leur migration vers
 * std::mutex / std::condition_variable (chantier modernisation n°2).
 *
 * Ces trois headers sont les briques d'attente utilisées par les threads du
 * mediaserver (files d'événements, jitter buffer RTP). Ils n'avaient AUCUN
 * test : cette suite fige leur comportement observable pour que la migration
 * soit vérifiable à isopérimètre. Sémantiques notables figées ici :
 *
 *   - Wait::Signal() sans waiter est PERDU (pure variable de condition, aucun
 *     état mémorisé) — un remplacement std devra préserver cette sémantique ou
 *     prouver que les appelants n'en dépendent pas.
 *   - WaitQueue::Wait() re-teste la file avant de dormir : un Add() qui précède
 *     le Wait() n'est jamais perdu (contrairement à Wait::Signal).
 *   - Cancel() est COLLANT : tout Wait ultérieur échoue (même file non vide)
 *     jusqu'à Reset().
 *   - RTPBuffer : livraison en séquence, trou livré après maxWaitTime, paquet
 *     tardif détruit (Add()==false), resynchro au 21e hors-séquence, remise à
 *     zéro sur changement de SSRC (si un paquet est encore en file), HurryUp().
 *
 * Défauts CONNUS non testés (comportement indéfini, à corriger pendant la
 * migration, pas à figer) :
 *   - WaitQueue::Skip() sur file vide = pop_front() d'une liste vide (UB).
 *   - ~Wait() signale la condition puis détruit le mutex : UB si un waiter est
 *     encore dans WaitSignal().
 *   - RTPBuffer::Wait() : le timespec du pthread_cond_timedwait est calculé en
 *     mélangeant ms et µs → échéance toujours passée → attente active (spin)
 *     pendant le comblement d'un trou. Le comportement fonctionnel (livraison
 *     après maxWaitTime) reste correct et c'est LUI qui est figé ici.
 *   - RTPBuffer : le changement de SSRC n'est détecté que si la file est non
 *     vide (test SsrcChangeOnEmptyBufferDropsPacket : défaut caractérisé).
 *
 * Les seuils temporels sont volontairement larges (marges ±) pour rester
 * déterministes sur machine chargée.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "wait.h"
#include "waitqueue.h"
#include "rtpbuffer.h"

namespace {

typedef std::chrono::steady_clock Clock;

// Millisecondes écoulées depuis t0.
static long ElapsedMs(const Clock::time_point& t0)
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
}

// =============================================================================
// Wait (wait.h) — attente signalable/annulable avec timeout
// =============================================================================

TEST(WaitPrimitive, TimesOutWhenNotSignaled)
{
	Wait w;
	Clock::time_point t0 = Clock::now();
	// Personne ne signale : échec après ~150 ms.
	EXPECT_FALSE(w.WaitSignal(150));
	long ms = ElapsedMs(t0);
	EXPECT_GE(ms, 100);
	EXPECT_LT(ms, 5000);
}

TEST(WaitPrimitive, WakesOnSignalFromOtherThread)
{
	Wait w;
	std::thread signaler([&w]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		w.Signal();
	});
	Clock::time_point t0 = Clock::now();
	EXPECT_TRUE(w.WaitSignal(5000));
	EXPECT_LT(ElapsedMs(t0), 4000);
	signaler.join();
}

// CARACTÉRISATION : un Signal() émis alors que personne n'attend est PERDU
// (aucun état mémorisé). Le WaitSignal qui suit attend son plein timeout.
// Toute migration vers std::condition_variable doit conserver (ou assumer de
// changer) cette sémantique.
TEST(WaitPrimitive, SignalBeforeWaitIsLost)
{
	Wait w;
	w.Signal();
	Clock::time_point t0 = Clock::now();
	EXPECT_FALSE(w.WaitSignal(150));
	EXPECT_GE(ElapsedMs(t0), 100);
}

TEST(WaitPrimitive, CancelBeforeWaitFailsImmediately)
{
	Wait w;
	w.Cancel();
	Clock::time_point t0 = Clock::now();
	EXPECT_FALSE(w.WaitSignal(5000));
	EXPECT_LT(ElapsedMs(t0), 1000);
}

TEST(WaitPrimitive, CancelUnblocksWaiter)
{
	Wait w;
	std::thread canceler([&w]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		w.Cancel();
	});
	Clock::time_point t0 = Clock::now();
	EXPECT_FALSE(w.WaitSignal(5000));
	EXPECT_LT(ElapsedMs(t0), 4000);
	canceler.join();
}

// Un Signal reçu pendant l'attente rend true même si un Cancel arrive après :
// l'issue dépend du premier événement. Ici Signal gagne.
TEST(WaitPrimitive, SignalThenCancelReportsSignal)
{
	Wait w;
	std::thread signaler([&w]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		w.Signal();
	});
	EXPECT_TRUE(w.WaitSignal(5000));
	signaler.join();
	// Et après un Cancel, tout WaitSignal échoue.
	w.Cancel();
	EXPECT_FALSE(w.WaitSignal(100));
}

// =============================================================================
// WaitQueue<T> (waitqueue.h) — file d'attente bloquante (T pointeur)
// =============================================================================

TEST(WaitQueuePrimitive, FifoOrderAndLength)
{
	static int a = 1, b = 2, c = 3;
	WaitQueue<int*> q;

	EXPECT_EQ(q.Length(), (DWORD)0);
	EXPECT_EQ(q.Pop(), (int*)NULL);	// Pop sur file vide = NULL
	EXPECT_EQ(q.Peek(), (int*)NULL);

	q.Add(&a);
	q.Add(&b);
	q.Add(&c);
	EXPECT_EQ(q.Length(), (DWORD)3);

	// Peek ne consomme pas.
	EXPECT_EQ(q.Peek(), &a);
	EXPECT_EQ(q.Length(), (DWORD)3);

	// Pop consomme en FIFO.
	EXPECT_EQ(q.Pop(), &a);
	EXPECT_EQ(q.Pop(), &b);
	EXPECT_EQ(q.Pop(), &c);
	EXPECT_EQ(q.Pop(), (int*)NULL);
	EXPECT_EQ(q.Length(), (DWORD)0);
}

TEST(WaitQueuePrimitive, ClearEmptiesQueue)
{
	static int a = 1, b = 2;
	WaitQueue<int*> q;
	q.Add(&a);
	q.Add(&b);
	q.Clear();
	EXPECT_EQ(q.Length(), (DWORD)0);
	EXPECT_EQ(q.Pop(), (int*)NULL);
}

// CARACTÉRISATION : contrairement à Wait::Signal, un Add qui précède le Wait
// n'est PAS perdu — Wait re-teste la file avant de dormir.
TEST(WaitQueuePrimitive, WaitReturnsImmediatelyWhenNonEmpty)
{
	static int a = 1;
	WaitQueue<int*> q;
	q.Add(&a);
	Clock::time_point t0 = Clock::now();
	EXPECT_TRUE(q.Wait(5000));
	EXPECT_LT(ElapsedMs(t0), 1000);
	EXPECT_EQ(q.Pop(), &a);
}

TEST(WaitQueuePrimitive, WaitTimesOutOnEmptyQueue)
{
	WaitQueue<int*> q;
	Clock::time_point t0 = Clock::now();
	EXPECT_FALSE(q.Wait(150));
	long ms = ElapsedMs(t0);
	EXPECT_GE(ms, 100);
	EXPECT_LT(ms, 5000);
}

TEST(WaitQueuePrimitive, WaitWakesOnAddFromOtherThread)
{
	static int a = 42;
	WaitQueue<int*> q;
	std::thread producer([&q]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		q.Add(&a);
	});
	Clock::time_point t0 = Clock::now();
	EXPECT_TRUE(q.Wait(5000));
	EXPECT_LT(ElapsedMs(t0), 4000);
	EXPECT_EQ(q.Pop(), &a);
	producer.join();
}

// CARACTÉRISATION : Cancel est collant. Tout Wait ultérieur échoue MÊME si la
// file n'est pas vide (Pop direct fonctionne toujours) ; Reset() efface le
// cancel ET vide la file.
TEST(WaitQueuePrimitive, CancelSticksUntilReset)
{
	static int a = 1;
	WaitQueue<int*> q;

	// Cancel débloque un waiter.
	std::thread canceler([&q]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		q.Cancel();
	});
	EXPECT_FALSE(q.Wait(5000));
	canceler.join();
	EXPECT_TRUE(q.IsCanceled());

	// Wait échoue immédiatement, y compris file non vide.
	q.Add(&a);
	Clock::time_point t0 = Clock::now();
	EXPECT_FALSE(q.Wait(5000));
	EXPECT_LT(ElapsedMs(t0), 1000);
	// … mais Pop direct rend l'élément.
	EXPECT_EQ(q.Peek(), &a);

	// Reset : cancel effacé ET file vidée (l'élément est perdu).
	q.Reset();
	EXPECT_FALSE(q.IsCanceled());
	EXPECT_EQ(q.Length(), (DWORD)0);
	EXPECT_FALSE(q.Wait(100));	// file vide → timeout normal
}

// Producteur/consommateur : rien n'est perdu, l'ordre est respecté.
TEST(WaitQueuePrimitive, ProducerConsumerKeepsOrderAndCount)
{
	enum { N = 500 };
	static int values[N];
	for (int i = 0; i < N; ++i)
		values[i] = i;

	WaitQueue<int*> q;
	std::thread producer([&q]() {
		for (int i = 0; i < N; ++i)
		{
			q.Add(&values[i]);
			if (i % 64 == 0)
				std::this_thread::yield();
		}
	});

	for (int i = 0; i < N; ++i)
	{
		ASSERT_TRUE(q.Wait(5000)) << "élément " << i << " jamais arrivé";
		int* v = q.Pop();
		ASSERT_NE(v, (int*)NULL);
		EXPECT_EQ(*v, i);
	}
	EXPECT_EQ(q.Length(), (DWORD)0);
	producer.join();
}

// =============================================================================
// RTPBuffer (rtpbuffer.h) — jitter buffer réordonnanceur
// =============================================================================

// Fabrique un paquet audio minimal. RTPBuffer prend possession du paquet
// (delete sur drop) ; les paquets rendus par Wait() sont à libérer par nous.
static RTPTimedPacket* MakePacket(WORD seq, DWORD ssrc = 0x11111111)
{
	RTPTimedPacket* rtp = new RTPTimedPacket(MediaFrame::Audio, 0);
	rtp->SetSeqNum(seq);
	rtp->SetSSRC(ssrc);
	return rtp;
}

TEST(RtpBufferPrimitive, InOrderDeliveryImmediate)
{
	RTPBuffer buf;	// maxWaitTime = 0 : aucune attente de réordonnancement
	EXPECT_TRUE(buf.Add(MakePacket(1)));
	EXPECT_TRUE(buf.Add(MakePacket(2)));
	EXPECT_TRUE(buf.Add(MakePacket(3)));

	for (WORD expected = 1; expected <= 3; ++expected)
	{
		RTPPacket* p = buf.Wait();
		ASSERT_NE(p, (RTPPacket*)NULL);
		EXPECT_EQ(p->GetSeqNum(), expected);
		delete p;
	}
}

// Arrivée 1,3,2 : le buffer rend 1,2,3 (le 2 arrive avant qu'on réclame le
// suivant, aucune attente nécessaire).
TEST(RtpBufferPrimitive, ReordersOutOfOrderArrival)
{
	RTPBuffer buf;
	buf.SetMaxWaitTime(2000);
	EXPECT_TRUE(buf.Add(MakePacket(1)));
	EXPECT_TRUE(buf.Add(MakePacket(3)));
	EXPECT_TRUE(buf.Add(MakePacket(2)));

	for (WORD expected = 1; expected <= 3; ++expected)
	{
		RTPPacket* p = buf.Wait();
		ASSERT_NE(p, (RTPPacket*)NULL);
		EXPECT_EQ(p->GetSeqNum(), expected);
		delete p;
	}
}

// Trou définitif (le 2 ne viendra jamais) : le 3 est livré quand même, après
// ~maxWaitTime compté depuis son ARRIVÉE.
TEST(RtpBufferPrimitive, GapDeliveredAfterMaxWaitTime)
{
	RTPBuffer buf;
	buf.SetMaxWaitTime(200);
	EXPECT_TRUE(buf.Add(MakePacket(1)));
	RTPPacket* p = buf.Wait();
	ASSERT_NE(p, (RTPPacket*)NULL);
	EXPECT_EQ(p->GetSeqNum(), 1);
	delete p;

	EXPECT_TRUE(buf.Add(MakePacket(3)));	// il manque le 2
	Clock::time_point t0 = Clock::now();
	p = buf.Wait();
	long ms = ElapsedMs(t0);
	ASSERT_NE(p, (RTPPacket*)NULL);
	EXPECT_EQ(p->GetSeqNum(), 3);
	EXPECT_GE(ms, 100);	// a bien attendu (marge sous les 200 ms nominaux)
	EXPECT_LT(ms, 5000);
	delete p;
}

// Paquet plus vieux que le prochain attendu : détruit, Add() rend false.
TEST(RtpBufferPrimitive, LatePacketIsDropped)
{
	RTPBuffer buf;
	EXPECT_TRUE(buf.Add(MakePacket(5)));
	RTPPacket* p = buf.Wait();	// next = 6
	ASSERT_NE(p, (RTPPacket*)NULL);
	delete p;

	EXPECT_FALSE(buf.Add(MakePacket(3)));	// 3 < 6 : tardif, droppé
}

// Au 21e paquet hors séquence consécutif, le buffer se resynchronise et
// accepte le paquet.
TEST(RtpBufferPrimitive, ResyncsAfterTwentyOutOfSequence)
{
	RTPBuffer buf;
	EXPECT_TRUE(buf.Add(MakePacket(100)));
	RTPPacket* p = buf.Wait();	// next = 101
	ASSERT_NE(p, (RTPPacket*)NULL);
	delete p;

	for (int i = 0; i < 20; ++i)
		EXPECT_FALSE(buf.Add(MakePacket(3))) << "itération " << i;

	// 21e : resynchro (next oublié), le paquet entre.
	EXPECT_TRUE(buf.Add(MakePacket(3)));
	p = buf.Wait();
	ASSERT_NE(p, (RTPPacket*)NULL);
	EXPECT_EQ(p->GetSeqNum(), 3);
	delete p;
}

// Changement de SSRC avec un paquet encore en file : la séquence attendue est
// oubliée, le flux repart sur la nouvelle source.
TEST(RtpBufferPrimitive, SsrcChangeWithPendingPacketResets)
{
	RTPBuffer buf;
	EXPECT_TRUE(buf.Add(MakePacket(100, 0xAAAA)));
	EXPECT_TRUE(buf.Add(MakePacket(101, 0xAAAA)));
	RTPPacket* p = buf.Wait();	// rend 100, next = 101, le 101 reste en file
	ASSERT_NE(p, (RTPPacket*)NULL);
	EXPECT_EQ(p->GetSeqNum(), 100);
	delete p;

	// Nouvelle source, numérotation repartie plus bas : accepté (reset).
	EXPECT_TRUE(buf.Add(MakePacket(5, 0xBBBB)));
	p = buf.Wait();
	ASSERT_NE(p, (RTPPacket*)NULL);
	EXPECT_EQ(p->GetSeqNum(), 5);
	delete p;
	buf.Cancel();	// ne pas bloquer sur le 101 restant
}

// DÉFAUT CARACTÉRISÉ : si la file est VIDE au moment du changement de SSRC,
// la détection (qui compare au dernier paquet en file) ne joue pas, et le
// paquet de la nouvelle source est droppé comme « tardif ». À corriger lors
// de la migration (mémoriser le dernier SSRC vu plutôt que lire la file).
TEST(RtpBufferPrimitive, SsrcChangeOnEmptyBufferDropsPacket)
{
	RTPBuffer buf;
	EXPECT_TRUE(buf.Add(MakePacket(100, 0xAAAA)));
	RTPPacket* p = buf.Wait();	// next = 101, file vide
	ASSERT_NE(p, (RTPPacket*)NULL);
	delete p;

	// Nouvelle source : droppé faute de paquet en file pour comparer le SSRC.
	EXPECT_FALSE(buf.Add(MakePacket(5, 0xBBBB)));
}

// HurryUp : vide le buffer sans attendre le comblement des trous.
TEST(RtpBufferPrimitive, HurryUpBypassesReorderWait)
{
	RTPBuffer buf;
	buf.SetMaxWaitTime(5000);
	EXPECT_TRUE(buf.Add(MakePacket(1)));
	RTPPacket* p = buf.Wait();	// next = 2
	ASSERT_NE(p, (RTPPacket*)NULL);
	delete p;

	EXPECT_TRUE(buf.Add(MakePacket(9)));	// trou 2..8
	buf.HurryUp();
	Clock::time_point t0 = Clock::now();
	p = buf.Wait();
	ASSERT_NE(p, (RTPPacket*)NULL);
	EXPECT_EQ(p->GetSeqNum(), 9);
	EXPECT_LT(ElapsedMs(t0), 1000);	// pas d'attente des 5000 ms
	delete p;
}

TEST(RtpBufferPrimitive, CancelUnblocksWait)
{
	RTPBuffer buf;
	std::thread canceler([&buf]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		buf.Cancel();
	});
	Clock::time_point t0 = Clock::now();
	RTPPacket* p = buf.Wait();
	EXPECT_EQ(p, (RTPPacket*)NULL);
	EXPECT_LT(ElapsedMs(t0), 4000);
	canceler.join();

	// Cancel est collant jusqu'à Reset : Wait rend NULL immédiatement,
	// même avec un paquet en file.
	EXPECT_TRUE(buf.Add(MakePacket(1)));
	EXPECT_EQ(buf.Wait(), (RTPPacket*)NULL);

	// Reset(false) : réarme sans vider ; le paquet est alors livré.
	buf.Reset(false);
	p = buf.Wait();
	ASSERT_NE(p, (RTPPacket*)NULL);
	EXPECT_EQ(p->GetSeqNum(), 1);
	delete p;
}

} // namespace
