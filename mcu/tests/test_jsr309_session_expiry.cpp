/**
 * test_jsr309_session_expiry.cpp — expiration des MediaSession JSR309 par
 * inactivité, « solution (c) : vitalité par event queue »
 * (jsr309_session_expiry_plan.md §7).
 *
 * Le principe testé : le long-poll du client sur sa file d'événements EST sa
 * preuve de vie. Plus personne ne polle une file pendant le délai de grâce →
 * les MediaSession liées à cette file (entry.queueId) sont détruites, puis la
 * file elle-même.
 *
 * Ce que ces tests figent :
 *   - XmlEventQueue : une file neuve est « pollée » (délai de grâce depuis sa
 *     CRÉATION), un poller attaché la protège indéfiniment, son détachement
 *     relance le compte à rebours ;
 *   - XmlStreamingHandler::GetIdleQueues ne rend que des ids (aucun pointeur
 *     de file ne survit au verrou) et recense les files abandonnées ;
 *   - JSR309Manager : le balayeur détruit sessions PUIS file, la cascade
 *     EventQueueDelete fait le même ménage immédiatement, et un délai de 0
 *     désarme tout (comportement historique).
 */
#include <gtest/gtest.h>

#include <chrono>
#include <thread>

//JSR309Manager vit dans src/jsr309 (hors include/), comme le voit main.cpp
#include "../src/jsr309/JSR309Manager.h"
#include "xmlstreaminghandler.h"

namespace {

typedef std::chrono::milliseconds Ms;

// Événement minimal : sert uniquement à savoir si une file existe encore
// (AddEvent rend 0 sur file inconnue, sans détruire l'événement).
class DummyEvent : public XmlEvent
{
public:
	virtual xmlrpc_value* GetXmlValue(xmlrpc_env *env) { return NULL; }
};

// La file <id> existe-t-elle encore dans le handler ?
static bool QueueExists(XmlStreamingHandler& handler, int id)
{
	DummyEvent *event = new DummyEvent();

	if (handler.AddEvent((DWORD)id,event))
		//La file a pris possession de l'événement
		return true;

	//File inconnue : AddEvent ne détruit pas l'événement (défaut historique)
	delete event;
	return false;
}

// Attend, jusqu'à timeoutMs, que pred() devienne vrai. Rend le verdict final.
template <class Pred>
static bool WaitFor(int timeoutMs, Pred pred)
{
	for (int waited = 0; waited < timeoutMs; waited += 25)
	{
		if (pred()) return true;
		std::this_thread::sleep_for(Ms(25));
	}
	return pred();
}

//----------------------------------------------------------------------------
// XmlEventQueue : la vitalité elle-même
//----------------------------------------------------------------------------

// Une file neuve n'est pas immédiatement expirable : le client a le délai de
// grâce pour venir poller ce qu'il vient de créer.
TEST(JSR309QueueLiveness, FreshQueueIsWithinGracePeriod)
{
	XmlEventQueue queue;

	EXPECT_TRUE(queue.IsPolled(Ms(1000)));
}

// ... mais une file jamais pollée finit par expirer (sonde de supervision
// morte, appel avorté avant le média : les « files nues » du §7.2).
TEST(JSR309QueueLiveness, NeverPolledQueueExpires)
{
	XmlEventQueue queue;

	std::this_thread::sleep_for(Ms(120));

	EXPECT_FALSE(queue.IsPolled(Ms(50)));
}

// Un poller attaché protège la file quelle que soit la durée écoulée : c'est
// exactement le long-poll qui dort 30 s entre deux keep-alive.
TEST(JSR309QueueLiveness, AttachedPollerProtectsIndefinitely)
{
	XmlEventQueue queue;

	queue.AttachPoller();
	std::this_thread::sleep_for(Ms(120));

	EXPECT_TRUE(queue.IsPolled(Ms(10)));
}

// Le détachement relance le compte à rebours ; une reconnexion (ré-attache)
// l'annule.
TEST(JSR309QueueLiveness, DetachStartsGracePeriodAndReattachCancelsIt)
{
	XmlEventQueue queue;

	queue.AttachPoller();
	queue.DetachPoller();

	//Juste après le détachement : encore dans la grâce
	EXPECT_TRUE(queue.IsPolled(Ms(1000)));

	std::this_thread::sleep_for(Ms(120));
	EXPECT_FALSE(queue.IsPolled(Ms(50)));

	//Reconnexion du client : la file redevient vivante
	queue.AttachPoller();
	EXPECT_TRUE(queue.IsPolled(Ms(50)));
}

// Deux pollers (client qui se reconnecte avant que l'ancien socket ne meure) :
// le départ du premier ne doit pas rendre la file expirable.
TEST(JSR309QueueLiveness, LastPollerWins)
{
	XmlEventQueue queue;

	queue.AttachPoller();
	queue.AttachPoller();
	queue.DetachPoller();

	std::this_thread::sleep_for(Ms(120));
	EXPECT_TRUE(queue.IsPolled(Ms(50)));	//il en reste un

	queue.DetachPoller();
	std::this_thread::sleep_for(Ms(120));
	EXPECT_FALSE(queue.IsPolled(Ms(50)));
}

//----------------------------------------------------------------------------
// XmlStreamingHandler : recensement des files abandonnées
//----------------------------------------------------------------------------

TEST(JSR309IdleQueues, ReportsAbandonedQueuesOnly)
{
	XmlStreamingHandler handler;

	int q1 = handler.CreateEventQueue();
	int q2 = handler.CreateEventQueue();
	ASSERT_GT(q1,0);
	ASSERT_GT(q2,0);

	//Délai de grâce large : les deux files viennent de naître
	EXPECT_TRUE(handler.GetIdleQueues(Ms(60000)).empty());

	std::this_thread::sleep_for(Ms(120));

	//Délai de grâce dépassé : les deux sont recensées
	std::vector<DWORD> idles = handler.GetIdleQueues(Ms(50));
	EXPECT_EQ(idles.size(),2u);

	//Une file détruite disparaît du recensement
	ASSERT_TRUE(handler.DestroyEventQueue((DWORD)q1));
	idles = handler.GetIdleQueues(Ms(50));
	ASSERT_EQ(idles.size(),1u);
	EXPECT_EQ((int)idles[0],q2);
}

//----------------------------------------------------------------------------
// JSR309Manager : le balayeur
//----------------------------------------------------------------------------

// Nominal : personne ne polle la file → session ET file détruites.
TEST(JSR309SessionExpiry, SweeperDeletesSessionsOfUnpolledQueue)
{
	XmlStreamingHandler handler;
	JSR309Manager manager;

	//1 s de grâce → balayage toutes les secondes (min(grâce, 10 s))
	manager.Init(&handler,1);

	int queueId = manager.CreateEventQueue();
	ASSERT_GT(queueId,0);

	int sessionId = manager.CreateMediaSession(L"expirable",queueId);
	ASSERT_GT(sessionId,0);

	std::shared_ptr<MediaSession> sess;
	ASSERT_TRUE(manager.GetMediaSessionRef(sessionId,sess));
	sess.reset();

	//Deux tours de balayage suffisent (grâce 1 s, période 1 s)
	EXPECT_TRUE(WaitFor(6000,[&] {
		std::shared_ptr<MediaSession> s;
		return manager.GetMediaSessionRef(sessionId,s) == 0;
	}));

	//La file part avec les sessions qu'elle portait
	EXPECT_FALSE(QueueExists(handler,queueId));

	manager.End();
}

// Un handler tenant déjà une référence : la session est retirée de la map et
// terminée, mais l'objet survit tant que la référence existe (shared_ptr).
TEST(JSR309SessionExpiry, InFlightReferenceSurvivesExpiry)
{
	XmlStreamingHandler handler;
	JSR309Manager manager;

	manager.Init(&handler,1);

	int queueId = manager.CreateEventQueue();
	int sessionId = manager.CreateMediaSession(L"tenue",queueId);

	//Référence « en vol », comme un handler XML-RPC concurrent
	std::shared_ptr<MediaSession> held;
	ASSERT_TRUE(manager.GetMediaSessionRef(sessionId,held));

	EXPECT_TRUE(WaitFor(6000,[&] {
		std::shared_ptr<MediaSession> s;
		return manager.GetMediaSessionRef(sessionId,s) == 0;
	}));

	//L'objet est toujours là et utilisable (End est idempotent)
	ASSERT_TRUE((bool)held);
	EXPECT_EQ(held->GetTag(),std::wstring(L"tenue"));

	manager.End();
	held.reset();
}

// EventQueueDelete explicite : la session N'EST PAS détruite sur le coup — le
// balayeur arme le délai de grâce, pour laisser au client une chance de revenir
// (précision mainteneur 2026-08-11).
TEST(JSR309SessionExpiry, DeleteEventQueueArmsGracePeriodInsteadOfDeleting)
{
	XmlStreamingHandler handler;
	JSR309Manager manager;

	//Expiration désarmée : rien ne doit détruire la session
	manager.Init(&handler,0);

	int queueId = manager.CreateEventQueue();
	int sessionId = manager.CreateMediaSession(L"grace",queueId);

	ASSERT_TRUE(manager.DeleteEventQueue(queueId));

	//La file est partie, la session est TOUJOURS là
	EXPECT_FALSE(QueueExists(handler,queueId));

	std::shared_ptr<MediaSession> sess;
	EXPECT_TRUE(manager.GetMediaSessionRef(sessionId,sess));
	sess.reset();

	manager.End();
}

// ... et à l'échéance du délai, la session part (balayeur armé).
TEST(JSR309SessionExpiry, OrphanSessionDiesWhenGracePeriodElapses)
{
	XmlStreamingHandler handler;
	JSR309Manager manager;

	manager.Init(&handler,1);

	int queueId = manager.CreateEventQueue();
	int sessionId = manager.CreateMediaSession(L"orpheline",queueId);

	//Le client s'en va proprement mais oublie sa session
	ASSERT_TRUE(manager.DeleteEventQueue(queueId));

	//Un tour arme le délai, un autre l'échoit : deux périodes au moins
	EXPECT_TRUE(WaitFor(8000,[&] {
		std::shared_ptr<MediaSession> s;
		return manager.GetMediaSessionRef(sessionId,s) == 0;
	}));

	manager.End();
}

// Session sans file (queueId 0) : elle n'est rattachée à rien, donc le balayeur
// ne doit JAMAIS la toucher — c'est le trou documenté (§7.6), et c'est aussi ce
// qui protège le chemin MOTELI, où eventListenerId vaut 0 par défaut.
TEST(JSR309SessionExpiry, SessionWithoutQueueIsNeverSwept)
{
	XmlStreamingHandler handler;
	JSR309Manager manager;

	manager.Init(&handler,1);

	int sessionId = manager.CreateMediaSession(L"sans-file",0);
	ASSERT_GT(sessionId,0);

	//Plusieurs tours de balayage
	std::this_thread::sleep_for(Ms(2500));

	std::shared_ptr<MediaSession> sess;
	EXPECT_TRUE(manager.GetMediaSessionRef(sessionId,sess));
	sess.reset();

	manager.End();
}

// Délai 0 = mécanisme désarmé : comportement historique strictement inchangé
// (aucune session, aucune file jamais détruite d'office).
TEST(JSR309SessionExpiry, ZeroGraceDisablesTheSweeper)
{
	XmlStreamingHandler handler;
	JSR309Manager manager;

	manager.Init(&handler,0);

	int queueId = manager.CreateEventQueue();
	int sessionId = manager.CreateMediaSession(L"immortelle",queueId);

	std::this_thread::sleep_for(Ms(1500));

	std::shared_ptr<MediaSession> sess;
	EXPECT_TRUE(manager.GetMediaSessionRef(sessionId,sess));
	EXPECT_TRUE(QueueExists(handler,queueId));
	sess.reset();

	manager.End();
}

// Arrêt propre : End() joint le balayeur (pas d'interblocage, pas de double
// End de session) même avec des sessions en attente d'expiration.
TEST(JSR309SessionExpiry, EndJoinsSweeperWithPendingSessions)
{
	XmlStreamingHandler handler;
	JSR309Manager manager;

	manager.Init(&handler,1);

	int queueId = manager.CreateEventQueue();
	manager.CreateMediaSession(L"en-attente",queueId);

	//Arrêt AVANT le premier tour de balayage
	manager.End();

	//La session a été terminée par End, pas par le balayeur
	SUCCEED();
}

}	// namespace
