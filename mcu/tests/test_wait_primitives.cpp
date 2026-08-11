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
 * En plus de la caractérisation, la fin du fichier porte des tests CIBLES
 * (rouges sur l'implémentation pthread historique, verts après la rénovation
 * std::mutex/std::condition_variable) qui spécifient la correction des défauts
 * découverts :
 *   - WaitQueue::Skip() sur file vide = pop_front() d'une liste vide (UB) →
 *     doit devenir un no-op (SkipOnEmptyIsSafeAndSkipsHead).
 *   - ~Wait()/~WaitQueue() détruisaient mutex/cond avec un waiter encore
 *     dedans (UB) → le destructeur doit annuler PUIS drainer les waiters
 *     (DestroyWhileWaiterInsideIsSafe).
 *   - Cancel() réveillait UN seul waiter (pthread_cond_signal) → doit tous
 *     les réveiller (CancelWakesAllWaiters).
 *   - RTPBuffer::Wait() mélangeait ms et µs dans son timespec → échéance
 *     toujours passée → attente ACTIVE pendant le comblement d'un trou →
 *     l'attente doit être passive (GapWaitDoesNotBurnCpu, mesure CPU thread).
 *   - RTPBuffer : un doublon de seq écrasait le pointeur en map sans delete →
 *     plus de fuite (DuplicateDoesNotLeak, sous-classe compteur d'instances).
 *   - RTPBuffer : changement de SSRC non détecté si la file est vide → doit
 *     être détecté en mémorisant le dernier SSRC vu, plus en lisant la file
 *     (SsrcChangeOnEmptyBufferDropsPacket devenu ...IsAccepted).
 *
 * Les seuils temporels sont volontairement larges (marges ±) pour rester
 * déterministes sur machine chargée.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <ctime>
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

// CIBLE (défaut historique corrigé) : le changement de SSRC est détecté MÊME
// file vide — le dernier SSRC vu est mémorisé, il n'est plus lu depuis la
// file. L'ancienne implémentation droppait ce paquet comme « tardif ».
TEST(RtpBufferPrimitive, SsrcChangeOnEmptyBufferIsAccepted)
{
	RTPBuffer buf;
	EXPECT_TRUE(buf.Add(MakePacket(100, 0xAAAA)));
	RTPPacket* p = buf.Wait();	// next = 101, file vide
	ASSERT_NE(p, (RTPPacket*)NULL);
	delete p;

	// Nouvelle source, numérotation plus basse : acceptée (resynchro).
	EXPECT_TRUE(buf.Add(MakePacket(5, 0xBBBB)));
	p = buf.Wait();
	ASSERT_NE(p, (RTPPacket*)NULL);
	EXPECT_EQ(p->GetSeqNum(), 5);
	delete p;
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

// LE scénario central du jitter buffer : Wait() bloque sur un trou (le 2
// manque), et le paquet manquant arrive PENDANT l'attente → il est livré
// aussitôt, sans attendre le plein maxWaitTime.
TEST(RtpBufferPrimitive, MissingPacketArrivingDuringWaitIsDeliveredImmediately)
{
	RTPBuffer buf;
	buf.SetMaxWaitTime(5000);
	EXPECT_TRUE(buf.Add(MakePacket(1)));
	RTPPacket* p = buf.Wait();	// next = 2
	ASSERT_NE(p, (RTPPacket*)NULL);
	delete p;

	EXPECT_TRUE(buf.Add(MakePacket(3)));	// le 2 manque encore
	std::thread straggler([&buf]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		buf.Add(MakePacket(2));	// le retardataire arrive pendant le Wait
	});

	Clock::time_point t0 = Clock::now();
	p = buf.Wait();
	long ms = ElapsedMs(t0);
	ASSERT_NE(p, (RTPPacket*)NULL);
	EXPECT_EQ(p->GetSeqNum(), 2);	// livré dès son arrivée…
	EXPECT_LT(ms, 2000);		// …pas au bout des 5000 ms
	delete p;
	straggler.join();

	p = buf.Wait();			// puis le 3, sans attente (seq == next)
	ASSERT_NE(p, (RTPPacket*)NULL);
	EXPECT_EQ(p->GetSeqNum(), 3);
	delete p;
}

// Un tardif injecté PENDANT qu'un Wait bloque sur un trou est éliminé sans
// perturber ni l'ordre ni la livraison du reste.
TEST(RtpBufferPrimitive, LatePacketDuringWaitDoesNotDisruptOrder)
{
	RTPBuffer buf;
	buf.SetMaxWaitTime(400);
	EXPECT_TRUE(buf.Add(MakePacket(5)));
	RTPPacket* p = buf.Wait();	// next = 6
	ASSERT_NE(p, (RTPPacket*)NULL);
	delete p;

	EXPECT_TRUE(buf.Add(MakePacket(8)));	// trou : 6 et 7 manquent
	std::thread mixed([&buf]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		buf.Add(MakePacket(3));	// tardif (3 < 6) : droppé
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		buf.Add(MakePacket(6));	// le 6 arrive, le 7 jamais
	});

	p = buf.Wait();			// le 6, dès son arrivée
	ASSERT_NE(p, (RTPPacket*)NULL);
	EXPECT_EQ(p->GetSeqNum(), 6);
	delete p;
	mixed.join();

	p = buf.Wait();			// le 8, après ~maxWaitTime (7 abandonné)
	ASSERT_NE(p, (RTPPacket*)NULL);
	EXPECT_EQ(p->GetSeqNum(), 8);
	delete p;
	// Le tardif 3 n'apparaît jamais : plus rien en file.
	EXPECT_EQ(buf.Length(), (DWORD)0);
}

// Doublon de numéro de séquence : livré UNE seule fois (l'écriture dans la map
// écrase l'entrée). DÉFAUT documenté : l'ancien pointeur est écrasé SANS
// delete → fuite mémoire silencieuse sur doublon, à corriger à la migration.
TEST(RtpBufferPrimitive, DuplicateSeqDeliveredOnce)
{
	RTPBuffer buf;
	EXPECT_TRUE(buf.Add(MakePacket(1)));
	EXPECT_TRUE(buf.Add(MakePacket(2)));
	EXPECT_TRUE(buf.Add(MakePacket(2)));	// doublon (fuite de l'original)
	EXPECT_TRUE(buf.Add(MakePacket(3)));

	for (WORD expected = 1; expected <= 3; ++expected)
	{
		RTPPacket* p = buf.Wait();
		ASSERT_NE(p, (RTPPacket*)NULL);
		EXPECT_EQ(p->GetSeqNum(), expected);
		delete p;
	}
	EXPECT_EQ(buf.Length(), (DWORD)0);	// pas de second « 2 » fantôme
}

// Passage du cycle 16 bits (65534, 65535, 0, 1) : l'ordre suit le numéro de
// séquence ÉTENDU (cycles<<16 | seq), comme le pose RTPSession à la réception.
TEST(RtpBufferPrimitive, SequenceWrapAcrossCycles)
{
	RTPBuffer buf;
	buf.SetMaxWaitTime(2000);
	RTPTimedPacket* p1 = MakePacket(65534); p1->SetSeqCycles(0);
	RTPTimedPacket* p2 = MakePacket(65535); p2->SetSeqCycles(0);
	RTPTimedPacket* p3 = MakePacket(0);     p3->SetSeqCycles(1);
	RTPTimedPacket* p4 = MakePacket(1);     p4->SetSeqCycles(1);
	// Arrivée en désordre de part et d'autre du wrap.
	EXPECT_TRUE(buf.Add(p1));
	EXPECT_TRUE(buf.Add(p3));
	EXPECT_TRUE(buf.Add(p2));
	EXPECT_TRUE(buf.Add(p4));

	const WORD expected[4] = { 65534, 65535, 0, 1 };
	for (int i = 0; i < 4; ++i)
	{
		RTPPacket* p = buf.Wait();
		ASSERT_NE(p, (RTPPacket*)NULL);
		EXPECT_EQ(p->GetSeqNum(), expected[i]) << "position " << i;
		delete p;
	}
}

// Rafale produite en désordre local (paires permutées) pendant que le
// consommateur lit : tout ressort strictement en séquence, rien n'est perdu.
TEST(RtpBufferPrimitive, ShuffledStreamComesOutInOrder)
{
	enum { N = 200 };
	RTPBuffer buf;
	buf.SetMaxWaitTime(1000);

	// Amorce : consommer le 1 pour fixer next=2 AVANT le désordre — sinon le
	// tout premier Wait (next==-1) livre ce qu'il trouve et le test dépendrait
	// de la course producteur/consommateur.
	EXPECT_TRUE(buf.Add(MakePacket(1)));
	RTPPacket* first = buf.Wait();
	ASSERT_NE(first, (RTPPacket*)NULL);
	EXPECT_EQ(first->GetSeqNum(), 1);
	delete first;

	std::thread producer([&buf]() {
		// 3,2, 5,4, 7,6… : chaque paire arrive permutée.
		for (WORD base = 2; base + 1 <= N; base += 2)
		{
			buf.Add(MakePacket(base + 1));
			buf.Add(MakePacket(base));
			if (base % 16 == 2)
				std::this_thread::yield();
		}
	});

	// Les paires produisent 2..N-1 (base+1 <= N avec base pair).
	for (WORD expected = 2; expected < N; ++expected)
	{
		RTPPacket* p = buf.Wait();
		ASSERT_NE(p, (RTPPacket*)NULL) << "paquet " << expected << " jamais livré";
		ASSERT_EQ(p->GetSeqNum(), expected);
		delete p;
	}
	producer.join();
	EXPECT_EQ(buf.Length(), (DWORD)0);
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

// =============================================================================
// Tests CIBLES — spécifient la correction des défauts découverts.
// Rouges sur l'implémentation pthread historique, verts après rénovation.
// =============================================================================

// CPU consommée par CE thread (et lui seul), en ms.
static long ThreadCpuMs()
{
	timespec ts;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

// Paquet instrumenté : compteur d'instances vivantes (le dtor de RTPPacket est
// virtuel, le delete du buffer passe donc bien par ici).
struct TrackedPacket : public RTPTimedPacket
{
	static inline std::atomic<int> alive{0};
	TrackedPacket(WORD seq, DWORD ssrc = 0x11111111)
		: RTPTimedPacket(MediaFrame::Audio, 0)
	{
		SetSeqNum(seq);
		SetSSRC(ssrc);
		++alive;
	}
	~TrackedPacket() override { --alive; }
};

// CIBLE : plus de fuite sur doublon de numéro de séquence (l'implémentation
// historique écrasait le pointeur en map sans delete).
TEST(RtpBufferPrimitive, DuplicateDoesNotLeak)
{
	TrackedPacket::alive = 0;
	{
		RTPBuffer buf;
		EXPECT_TRUE(buf.Add(new TrackedPacket(1)));
		EXPECT_TRUE(buf.Add(new TrackedPacket(2)));
		buf.Add(new TrackedPacket(2));	// doublon
		EXPECT_TRUE(buf.Add(new TrackedPacket(3)));

		for (WORD expected = 1; expected <= 3; ++expected)
		{
			RTPPacket* p = buf.Wait();
			ASSERT_NE(p, (RTPPacket*)NULL);
			EXPECT_EQ(p->GetSeqNum(), expected);
			delete p;
		}
		EXPECT_EQ(buf.Length(), (DWORD)0);
	}
	// Tout ce qui est entré est sorti ou a été détruit par le buffer.
	EXPECT_EQ(TrackedPacket::alive.load(), 0);
}

// Le destructeur libère les paquets encore en file (déjà vrai : filet).
TEST(RtpBufferPrimitive, DestructorFreesPendingPackets)
{
	TrackedPacket::alive = 0;
	{
		RTPBuffer buf;
		EXPECT_TRUE(buf.Add(new TrackedPacket(1)));
		EXPECT_TRUE(buf.Add(new TrackedPacket(2)));
		EXPECT_TRUE(buf.Add(new TrackedPacket(3)));
	}
	EXPECT_EQ(TrackedPacket::alive.load(), 0);
}

// CIBLE : l'attente de comblement d'un trou doit être PASSIVE. L'implémentation
// historique calcule son échéance en mélangeant ms et µs → timedwait toujours
// expiré → spin : ~300 ms de CPU pour 300 ms d'attente.
TEST(RtpBufferPrimitive, GapWaitDoesNotBurnCpu)
{
	RTPBuffer buf;
	buf.SetMaxWaitTime(300);
	EXPECT_TRUE(buf.Add(MakePacket(1)));
	RTPPacket* p = buf.Wait();	// next = 2
	ASSERT_NE(p, (RTPPacket*)NULL);
	delete p;

	EXPECT_TRUE(buf.Add(MakePacket(3)));	// le 2 manque
	Clock::time_point t0 = Clock::now();
	long cpu0 = ThreadCpuMs();
	p = buf.Wait();				// bloque ~300 ms
	long cpuMs = ThreadCpuMs() - cpu0;
	long wallMs = ElapsedMs(t0);
	ASSERT_NE(p, (RTPPacket*)NULL);
	EXPECT_EQ(p->GetSeqNum(), 3);
	EXPECT_GE(wallMs, 100);			// on a bien attendu…
	EXPECT_LT(cpuMs, 100);			// …sans brûler le CPU
	delete p;
}

// CIBLE : Cancel() doit réveiller TOUS les waiters (l'implémentation historique
// fait pthread_cond_signal = un seul ; les autres attendent leur plein timeout).
TEST(WaitPrimitive, CancelWakesAllWaiters)
{
	Wait w;
	std::atomic<long> elapsed[3];
	std::atomic<int> result[3];
	std::thread th[3];
	for (int i = 0; i < 3; ++i)
	{
		elapsed[i] = -1;
		th[i] = std::thread([&w, &elapsed, &result, i]() {
			Clock::time_point t0 = Clock::now();
			result[i] = w.WaitSignal(3000) ? 1 : 0;
			elapsed[i] = ElapsedMs(t0);
		});
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	w.Cancel();
	for (int i = 0; i < 3; ++i)
		th[i].join();
	for (int i = 0; i < 3; ++i)
	{
		EXPECT_EQ(result[i].load(), 0) << "waiter " << i;
		EXPECT_LT(elapsed[i].load(), 1500) << "waiter " << i << " a attendu son plein timeout";
	}
}

TEST(WaitQueuePrimitive, CancelWakesAllWaiters)
{
	WaitQueue<int*> q;
	std::atomic<long> elapsed[3];
	std::thread th[3];
	for (int i = 0; i < 3; ++i)
	{
		elapsed[i] = -1;
		th[i] = std::thread([&q, &elapsed, i]() {
			Clock::time_point t0 = Clock::now();
			q.Wait(3000);
			elapsed[i] = ElapsedMs(t0);
		});
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	q.Cancel();
	for (int i = 0; i < 3; ++i)
		th[i].join();
	for (int i = 0; i < 3; ++i)
		EXPECT_LT(elapsed[i].load(), 1500) << "waiter " << i << " a attendu son plein timeout";
}

// CIBLE : détruire l'objet pendant qu'un waiter est dans WaitSignal doit être
// SÛR — le destructeur annule puis draine les waiters avant de libérer
// mutex/condition. (Historique : Signal + destroy immédiat = UB.)
TEST(WaitPrimitive, DestroyWhileWaiterInsideIsSafe)
{
	Wait* w = new Wait();
	std::atomic<int> result{-1};
	std::atomic<long> waited{-1};
	std::thread waiter([&]() {
		Clock::time_point t0 = Clock::now();
		result = w->WaitSignal(4000) ? 1 : 0;
		waited = ElapsedMs(t0);
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	delete w;	// doit annuler + drainer, pas détruire sous le waiter
	waiter.join();
	EXPECT_EQ(result.load(), 0);	// réveillé en « annulé »
	EXPECT_LT(waited.load(), 2000);	// sans attendre le plein timeout
}

TEST(WaitQueuePrimitive, DestroyWhileWaiterInsideIsSafe)
{
	WaitQueue<int*>* q = new WaitQueue<int*>();
	std::atomic<int> result{-1};
	std::atomic<long> waited{-1};
	std::thread waiter([&]() {
		Clock::time_point t0 = Clock::now();
		result = q->Wait(4000) ? 1 : 0;
		waited = ElapsedMs(t0);
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	delete q;
	waiter.join();
	EXPECT_EQ(result.load(), 0);
	EXPECT_LT(waited.load(), 2000);
}

// CIBLE : Skip() sur file vide doit être un no-op (historique : pop_front d'une
// liste vide = UB) ; sur file non vide il élimine la tête.
TEST(WaitQueuePrimitive, SkipOnEmptyIsSafeAndSkipsHead)
{
	static int a = 1, b = 2;
	WaitQueue<int*> q;
	q.Skip();			// file vide : ne doit rien faire
	EXPECT_EQ(q.Length(), (DWORD)0);

	q.Add(&a);
	q.Add(&b);
	q.Skip();			// élimine la tête (a)
	EXPECT_EQ(q.Length(), (DWORD)1);
	EXPECT_EQ(q.Pop(), &b);
}

// Tempête producteurs multiples : rien n'est perdu (l'ordre inter-producteurs
// n'est pas défini, on compte).
TEST(WaitQueuePrimitive, MultipleProducersLoseNothing)
{
	enum { P = 4, PER = 100 };
	static int values[P * PER];
	WaitQueue<int*> q;
	std::thread producers[P];
	for (int t = 0; t < P; ++t)
		producers[t] = std::thread([&q, t]() {
			for (int i = 0; i < PER; ++i)
			{
				values[t * PER + i] = t * PER + i;
				q.Add(&values[t * PER + i]);
				if (i % 16 == 0)
					std::this_thread::yield();
			}
		});

	int got = 0;
	while (got < P * PER)
	{
		ASSERT_TRUE(q.Wait(5000)) << "après " << got << " éléments";
		while (int* v = q.Pop())
		{
			ASSERT_NE(v, (int*)NULL);
			++got;
		}
	}
	EXPECT_EQ(got, P * PER);
	for (int t = 0; t < P; ++t)
		producers[t].join();
}

} // namespace
