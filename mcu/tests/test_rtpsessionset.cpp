/**
 * test_rtpsessionset.cpp — RtpSessionSet (rtpsessionset.h) : le reacteur qui
 * bat un groupe de PollHandler avec un seul thread et un seul poll().
 *
 * Conception : docs/conception/RTP-REACTOR/SPEC.md §3.3 et §3.4. Ecrit AVANT
 * que RTPSession n'en devienne un handler (lot 1 du chantier) : la classe est
 * prouvee seule.
 *
 * Ce qui est fixe ici : un seul thread pour N handlers, aiguillage des
 * evenements au bon handler, minimum des echeances, OnPeriodic appele meme sur
 * le reveil d'un voisin, retrait SYNCHRONE (le contrat qui autorise l'appelant
 * a fermer ses sockets), retrait reentrant non bloquant, et un handler mort qui
 * ne rend pas sourd son groupe.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <unistd.h>

#include "rtpsessionset.h"

namespace {

typedef std::chrono::steady_clock Clock;

static long ElapsedMs(const Clock::time_point& t0)
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
}

template <class Pred>
static bool WaitFor(Pred pred, long deadlineMs = 3000)
{
	Clock::time_point t0 = Clock::now();
	while (!pred() && ElapsedMs(t0) < deadlineMs)
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	return pred();
}

// Handler de test sur un tube : le bout ecriture simule le reseau.
struct FakeHandler : public PollHandler
{
	int	rd	= -1;
	int	wr	= -1;
	int	timeoutMs = -1;

	std::atomic<int>	polls{0};	// OnPollEvents
	std::atomic<int>	periodics{0};
	std::atomic<int>	bytes{0};
	std::atomic<int>	errors{0};
	std::atomic<short>	lastError{0};

	// Nombre de fds a declarer (0 = handler purement temporel).
	int	fdCount	= 1;

	// Retrait reentrant : depuis OnPollEvents si `set` est pose.
	RtpSessionSet*		selfRemoveFrom = nullptr;

	// Travail long simule, en ms, dans OnPollEvents.
	int	workMs	= 0;

	std::atomic<bool>	sawForeignThread{false};
	std::thread::id		seenThread;
	std::atomic<bool>	threadSeen{false};

	FakeHandler()
	{
		int fd[2];
		if (pipe(fd) == 0)
		{
			rd = fd[0];
			wr = fd[1];
		}
	}

	~FakeHandler() override
	{
		CloseRead();
		CloseWrite();
	}

	void CloseRead()	{ if (rd >= 0) { close(rd); rd = -1; } }
	void CloseWrite()	{ if (wr >= 0) { close(wr); wr = -1; } }

	void Feed(char c = 'x')
	{
		if (wr >= 0)
		{
			ssize_t r = write(wr, &c, 1);
			(void)r;
		}
	}

	// --- PollHandler ---

	int GetPollFds(pollfd* fds, int max) override
	{
		if (fdCount <= 0 || max <= 0 || rd < 0)
			return 0;
		fds[0].fd	= rd;
		fds[0].events	= POLLIN;
		fds[0].revents	= 0;
		return 1;
	}

	int GetNextTimeoutMs(QWORD) override { return timeoutMs; }

	void OnPollEvents(const pollfd* fds, int count, QWORD) override
	{
		Note();
		++polls;

		for (int i = 0; i < count; ++i)
		{
			if (!(fds[i].revents & POLLIN))
				continue;
			char buf[64];
			ssize_t n = read(fds[i].fd, buf, sizeof(buf));
			if (n > 0)
				bytes += (int)n;
		}

		if (workMs)
			std::this_thread::sleep_for(std::chrono::milliseconds(workMs));

		if (selfRemoveFrom)
		{
			RtpSessionSet* set = selfRemoveFrom;
			selfRemoveFrom = nullptr;
			set->Remove(this);
		}
	}

	void OnPeriodic(QWORD) override
	{
		Note();
		++periodics;
	}

	void OnPollError(short revents) override
	{
		lastError = revents;
		++errors;
	}

private:
	// Tous les rappels doivent venir du MEME thread, et pas du thread de test.
	void Note()
	{
		std::thread::id me = std::this_thread::get_id();
		if (!threadSeen.exchange(true))
			seenThread = me;
		else if (seenThread != me)
			sawForeignThread = true;
	}
};

// Un seul thread pour tout le groupe, et chaque evenement va au bon handler.
TEST(RtpSessionSet, OneThreadServesEveryHandlerAndRoutesItsOwnEvents)
{
	// Les handlers AVANT le set : la destruction est inverse, donc le set — et
	// son thread — meurt le premier, meme si un ASSERT sort du test plus tot.
	FakeHandler a, b;
	RtpSessionSet set("test-routing");
	a.timeoutMs = b.timeoutMs = -1;

	set.Add(&a);
	set.Add(&b);
	ASSERT_EQ(set.GetHandlerCount(), 2u);
	ASSERT_TRUE(set.Start());

	a.Feed();
	ASSERT_TRUE(WaitFor([&] { return a.bytes.load() == 1; }));

	// b n'a rien recu, mais son OnPeriodic a bien tourne : c'est le contrat.
	EXPECT_EQ(b.bytes.load(), 0);
	EXPECT_GT(b.periodics.load(), 0);

	b.Feed();
	ASSERT_TRUE(WaitFor([&] { return b.bytes.load() == 1; }));
	EXPECT_EQ(a.bytes.load(), 1);

	// Un seul thread, et ce n'est pas celui du test.
	EXPECT_FALSE(a.sawForeignThread.load());
	EXPECT_FALSE(b.sawForeignThread.load());
	EXPECT_EQ(a.seenThread, b.seenThread);
	EXPECT_NE(a.seenThread, std::this_thread::get_id());

	set.Stop();
	set.Remove(&a);
	set.Remove(&b);
}

// L'echeance retenue est le MINIMUM du groupe : le voisin patient est servi
// a la cadence du plus presse.
TEST(RtpSessionSet, PollTimeoutIsTheMinimumOfTheGroup)
{
	FakeHandler fast, slow;
	RtpSessionSet set("test-timeout");
	fast.timeoutMs = 10;
	slow.timeoutMs = 60000;

	set.Add(&fast);
	set.Add(&slow);
	ASSERT_TRUE(set.Start());

	// Sans le minimum, `slow` attendrait une minute avant son premier tick.
	ASSERT_TRUE(WaitFor([&] { return slow.periodics.load() >= 5; }, 2000));
	EXPECT_GE(fast.periodics.load(), 5);

	set.Stop();
	set.Remove(&fast);
	set.Remove(&slow);
}

// Aucun handler ne reclame d'echeance : le poll est infini, donc le reacteur
// DORT (il ne tourne pas a vide), et Wake() est le seul moyen de le rendre.
TEST(RtpSessionSet, WakeBreaksAnInfinitePoll)
{
	FakeHandler h;
	RtpSessionSet set("test-wake");
	h.timeoutMs = -1;

	set.Add(&h);
	ASSERT_TRUE(set.Start());

	// Rien a faire, rien qui arrive : aucun rappel ne doit avoir lieu.
	std::this_thread::sleep_for(std::chrono::milliseconds(120));
	ASSERT_EQ(h.periodics.load(), 0) << "poll(-1) doit dormir, pas tourner";

	Clock::time_point t0 = Clock::now();
	set.Wake();
	EXPECT_TRUE(WaitFor([&] { return h.periodics.load() >= 1; }, 1000));
	EXPECT_LT(ElapsedMs(t0), 1000);

	set.Stop();
	set.Remove(&h);
}

// Inscription alors que le reacteur dort dans un poll infini : sans le reveil
// pose par Add, le descripteur du nouveau venu ne serait pas dans le jeu poll,
// et son trafic ne serait vu qu'au prochain evenement d'un voisin — jamais, ici.
TEST(RtpSessionSet, AddWakesASleepingReactorSoTheNewcomerIsPolled)
{
	FakeHandler resident, newcomer;
	RtpSessionSet set("test-add-wake");
	resident.timeoutMs = -1;
	newcomer.timeoutMs = -1;

	set.Add(&resident);
	ASSERT_TRUE(set.Start());
	std::this_thread::sleep_for(std::chrono::milliseconds(80));
	ASSERT_EQ(resident.periodics.load(), 0);

	set.Add(&newcomer);
	newcomer.Feed();

	EXPECT_TRUE(WaitFor([&] { return newcomer.bytes.load() == 1; }, 1000));

	set.Stop();
	set.Remove(&resident);
	set.Remove(&newcomer);
}

// LE contrat du §3.4 : au retour de Remove, le reacteur ne touche plus le
// handler. C'est ce qui autorise l'appelant a fermer ses sockets juste apres.
TEST(RtpSessionSet, RemoveIsSynchronousSoTheCallerMayCloseItsSockets)
{
	FakeHandler victim, witness;
	RtpSessionSet set("test-quiesce");
	victim.timeoutMs = 5;
	witness.timeoutMs = 5;

	set.Add(&victim);
	set.Add(&witness);
	ASSERT_TRUE(set.Start());
	ASSERT_TRUE(WaitFor([&] { return victim.periodics.load() >= 3; }));

	set.Remove(&victim);
	EXPECT_EQ(set.GetHandlerCount(), 1u);

	// Le retour de Remove autorise la fermeture. Si le reacteur poll encore ce
	// descripteur, il verra POLLNVAL — donc OnPollError, donc le compteur bouge.
	int frozenPolls	   = victim.polls.load();
	int frozenPeriodics = victim.periodics.load();
	victim.CloseRead();
	victim.Feed();

	std::this_thread::sleep_for(std::chrono::milliseconds(200));

	EXPECT_EQ(victim.polls.load(), frozenPolls);
	EXPECT_EQ(victim.periodics.load(), frozenPeriodics);
	EXPECT_EQ(victim.errors.load(), 0) << "un fd ferme apres Remove ne doit plus etre poll";

	// Et le voisin n'a rien perdu.
	EXPECT_GT(witness.periodics.load(), frozenPeriodics);

	set.Stop();
	set.Remove(&witness);
}

// Retrait DEPUIS un callback : s'attendre soi-meme ne finirait jamais, donc le
// retrait est immediat.
TEST(RtpSessionSet, RemoveFromInsideACallbackDoesNotBlock)
{
	FakeHandler h, witness;
	RtpSessionSet set("test-reentrant");
	h.timeoutMs = -1;
	witness.timeoutMs = 10;
	h.selfRemoveFrom = &set;

	set.Add(&h);
	set.Add(&witness);
	ASSERT_TRUE(set.Start());

	h.Feed();
	ASSERT_TRUE(WaitFor([&] { return set.GetHandlerCount() == 1u; }))
		<< "le handler doit s'etre retire lui-meme";

	// Le reacteur n'est pas bloque : le voisin continue d'etre servi.
	int before = witness.periodics.load();
	EXPECT_TRUE(WaitFor([&] { return witness.periodics.load() > before + 2; }));

	// Et il ne rappelle plus le partant.
	int frozen = h.periodics.load();
	std::this_thread::sleep_for(std::chrono::milliseconds(150));
	EXPECT_EQ(h.periodics.load(), frozen);

	set.Stop();
	set.Remove(&witness);
}

// Un socket mort retire SON handler, et laisse vivre les autres. C'est le
// changement de comportement voulu : aujourd'hui la session devient sourde en
// silence, ici elle sort du groupe.
TEST(RtpSessionSet, ADeadSocketRemovesOnlyItsOwnHandler)
{
	FakeHandler dying, witness;
	RtpSessionSet set("test-pollerr");
	dying.timeoutMs = 10;
	witness.timeoutMs = 10;

	set.Add(&dying);
	set.Add(&witness);
	ASSERT_TRUE(set.Start());
	ASSERT_TRUE(WaitFor([&] { return witness.periodics.load() >= 2; }));

	// Bout ecriture ferme -> POLLHUP sur le bout lecture.
	dying.CloseWrite();

	ASSERT_TRUE(WaitFor([&] { return dying.errors.load() >= 1; }));
	EXPECT_TRUE(dying.lastError.load() & (POLLHUP | POLLERR | POLLNVAL));
	EXPECT_EQ(set.GetHandlerCount(), 1u);

	int before = witness.periodics.load();
	EXPECT_TRUE(WaitFor([&] { return witness.periodics.load() > before + 3; }))
		<< "le groupe survit au handler mort";

	set.Stop();
	set.Remove(&dying);
	set.Remove(&witness);
}

// Inscriptions et retraits pendant que le trafic coule.
TEST(RtpSessionSet, AddAndRemoveUnderTrafficLoseNothing)
{
	FakeHandler steady;
	RtpSessionSet set("test-churn");
	steady.timeoutMs = 5;

	set.Add(&steady);
	ASSERT_TRUE(set.Start());

	std::atomic<bool> feeding{true};
	std::atomic<int>  fed{0};
	std::thread feeder([&] {
		while (feeding)
		{
			steady.Feed();
			++fed;
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
	});

	for (int i = 0; i < 20; ++i)
	{
		FakeHandler transient;
		transient.timeoutMs = 5;
		set.Add(&transient);
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		set.Remove(&transient);	// synchrone : la destruction qui suit est sure
	}

	feeding = false;
	feeder.join();

	EXPECT_EQ(set.GetHandlerCount(), 1u);
	EXPECT_TRUE(WaitFor([&] { return steady.bytes.load() >= fed.load(); }))
		<< "octets lus " << steady.bytes.load() << " pour " << fed.load() << " ecrits";
	EXPECT_FALSE(steady.sawForeignThread.load());

	set.Stop();
	set.Remove(&steady);
}

// Un tour long est trace et mesure : c'est le garde-fou de la tete de ligne.
TEST(RtpSessionSet, ALongTurnIsMeasured)
{
	FakeHandler slowpoke;
	RtpSessionSet set("test-longturn");
	slowpoke.timeoutMs = -1;
	slowpoke.workMs = 80;	// > LongTurnUs

	set.Add(&slowpoke);
	ASSERT_TRUE(set.Start());

	slowpoke.Feed();
	ASSERT_TRUE(WaitFor([&] { return slowpoke.polls.load() >= 1; }));
	EXPECT_TRUE(WaitFor([&] { return set.GetLongestTurnUs() >= RtpSessionSet::LongTurnUs; }));

	set.Stop();
	set.Remove(&slowpoke);
}

// Cas de bord : double inscription refusee, retrait d'un inconnu inoffensif,
// retrait sans reacteur demarre immediat.
TEST(RtpSessionSet, EdgeCasesAreHarmless)
{
	FakeHandler h, unknown;
	RtpSessionSet set("test-edges");

	set.Add(&h);
	set.Add(&h);				// refuse
	EXPECT_EQ(set.GetHandlerCount(), 1u);

	set.Remove(&unknown);			// inconnu : sans effet
	EXPECT_EQ(set.GetHandlerCount(), 1u);

	// Reacteur jamais demarre : le retrait ne doit pas attendre un thread absent.
	Clock::time_point t0 = Clock::now();
	set.Remove(&h);
	EXPECT_LT(ElapsedMs(t0), 500);
	EXPECT_EQ(set.GetHandlerCount(), 0u);

	set.Remove(nullptr);
}

} // namespace
