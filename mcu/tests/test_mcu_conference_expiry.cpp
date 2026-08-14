/**
 * test_mcu_conference_expiry.cpp — expiration des conférences de l'API MCU par
 * inactivité du contrôleur, même politique que côté JSR-309
 * (`jsr309_session_expiry_plan.md` §7, mécanique commune dans
 * `eventqueuesweeper.h`).
 *
 * Côté MCU, le `queueId` est porté par la CONFÉRENCE (`MCU::ConferenceEntry`) :
 * la portée du nettoyage suit donc le découpage des files choisi par le
 * contrôleur — une file par conférence les isole, une file partagée les emporte
 * ensemble. Ces tests figent :
 *   - file non lue → conférence détruite, tag libéré, file détruite ;
 *   - `EventQueueDelete` explicite → délai de grâce ARMÉ, pas de destruction
 *     immédiate, puis destruction à l'échéance ;
 *   - conférence sans file (queueId 0, cas MOTELI) jamais balayée ;
 *   - délai 0 = désarmé (comportement historique) ;
 *   - `End()` joint le balayeur.
 *
 * La couche « vitalité de la file » elle-même (Attach/Detach/IsPolled,
 * GetIdleQueues) est testée dans test_jsr309_session_expiry.cpp : elle est
 * commune aux deux API.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "mcu.h"
#include "xmlstreaminghandler.h"

namespace {

typedef std::chrono::milliseconds Ms;

class DummyMcuEvent : public XmlEvent
{
public:
	virtual xmlrpc_value* GetXmlValue(xmlrpc_env *env) { return NULL; }
};

static bool QueueExists(XmlStreamingHandler& handler, int id)
{
	DummyMcuEvent *event = new DummyMcuEvent();

	if (handler.AddEvent((DWORD)id,event))
		return true;

	//File inconnue : AddEvent ne détruit pas l'événement
	delete event;
	return false;
}

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

// Nominal : personne ne lit la file → conférence et file détruites, et le tag
// est libéré (sans quoi il resterait à bloquer la map des tags, par laquelle
// passent tous les événements de participant).
TEST(McuConferenceExpiry, SweeperDeletesConferencesOfUnpolledQueue)
{
	XmlStreamingHandler handler;
	MCU mcu;

	//1 s de grâce → balayage toutes les secondes
	mcu.Init(&handler,1);

	int queueId = mcu.CreateEventQueue();
	ASSERT_GT(queueId,0);

	int confId = mcu.CreateConference(L"expirable",queueId);
	ASSERT_GT(confId,0);

	std::shared_ptr<MultiConf> conf;
	ASSERT_TRUE(mcu.GetConferenceRef(confId,conf));
	conf.reset();

	EXPECT_TRUE(WaitFor(6000,[&] {
		std::shared_ptr<MultiConf> c;
		return mcu.GetConferenceRef(confId,c) == 0;
	}));

	//Tag libéré : le contrôleur peut recréer une conférence du même nom
	EXPECT_EQ(mcu.GetConferenceId(L"expirable"),0);

	//La file part avec les conférences qu'elle portait
	EXPECT_FALSE(QueueExists(handler,queueId));

	mcu.End();
}

// Une référence « en vol » (handler XML-RPC concurrent) survit à l'expiration :
// la conférence quitte la map et est terminée, mais l'objet vit tant que la
// référence existe.
TEST(McuConferenceExpiry, InFlightReferenceSurvivesExpiry)
{
	XmlStreamingHandler handler;
	MCU mcu;

	mcu.Init(&handler,1);

	int queueId = mcu.CreateEventQueue();
	int confId = mcu.CreateConference(L"tenue",queueId);

	std::shared_ptr<MultiConf> held;
	ASSERT_TRUE(mcu.GetConferenceRef(confId,held));

	EXPECT_TRUE(WaitFor(6000,[&] {
		std::shared_ptr<MultiConf> c;
		return mcu.GetConferenceRef(confId,c) == 0;
	}));

	ASSERT_TRUE((bool)held);
	EXPECT_EQ(held->GetTag(),std::wstring(L"tenue"));

	mcu.End();
	held.reset();
}

// EventQueueDelete explicite : la conférence N'EST PAS détruite sur le coup —
// le balayeur arme le délai de grâce (précision mainteneur 2026-08-11), pour
// laisser au contrôleur une chance de se reconnecter.
TEST(McuConferenceExpiry, DeleteEventQueueArmsGracePeriodInsteadOfDeleting)
{
	XmlStreamingHandler handler;
	MCU mcu;

	//Expiration désarmée : rien ne doit détruire la conférence
	mcu.Init(&handler,0);

	int queueId = mcu.CreateEventQueue();
	int confId = mcu.CreateConference(L"grace",queueId);

	ASSERT_TRUE(mcu.DeleteEventQueue(queueId));

	EXPECT_FALSE(QueueExists(handler,queueId));

	std::shared_ptr<MultiConf> conf;
	EXPECT_TRUE(mcu.GetConferenceRef(confId,conf));
	conf.reset();

	mcu.End();
}

// ... et à l'échéance du délai, la conférence part.
TEST(McuConferenceExpiry, OrphanConferenceDiesWhenGracePeriodElapses)
{
	XmlStreamingHandler handler;
	MCU mcu;

	mcu.Init(&handler,1);

	int queueId = mcu.CreateEventQueue();
	int confId = mcu.CreateConference(L"orpheline",queueId);

	ASSERT_TRUE(mcu.DeleteEventQueue(queueId));

	//Un tour arme le délai, un autre l'échoit
	EXPECT_TRUE(WaitFor(8000,[&] {
		std::shared_ptr<MultiConf> c;
		return mcu.GetConferenceRef(confId,c) == 0;
	}));

	mcu.End();
}

// Conférence sans file (queueId 0) : jamais balayée. C'est le cas du chemin
// MOTELI/RabbitMQ, où eventListenerId vaut 0 par défaut — son nettoyage reste
// à concevoir (§7.5), il ne doit surtout pas être fait par ce balayeur.
TEST(McuConferenceExpiry, ConferenceWithoutQueueIsNeverSwept)
{
	XmlStreamingHandler handler;
	MCU mcu;

	mcu.Init(&handler,1);

	int confId = mcu.CreateConference(L"sans-file",0);
	ASSERT_GT(confId,0);

	std::this_thread::sleep_for(Ms(2500));

	std::shared_ptr<MultiConf> conf;
	EXPECT_TRUE(mcu.GetConferenceRef(confId,conf));
	conf.reset();

	mcu.End();
}

// Délai 0 = mécanisme désarmé : comportement historique strictement inchangé.
TEST(McuConferenceExpiry, ZeroGraceDisablesTheSweeper)
{
	XmlStreamingHandler handler;
	MCU mcu;

	mcu.Init(&handler,0);

	int queueId = mcu.CreateEventQueue();
	int confId = mcu.CreateConference(L"immortelle",queueId);

	std::this_thread::sleep_for(Ms(1500));

	std::shared_ptr<MultiConf> conf;
	EXPECT_TRUE(mcu.GetConferenceRef(confId,conf));
	EXPECT_TRUE(QueueExists(handler,queueId));
	conf.reset();

	mcu.End();
}

// Arrêt propre : End() joint le balayeur, sans interblocage, même avec des
// conférences en attente d'expiration (End extrait sous verrou et termine
// dehors, ce que l'ancien End ne faisait pas).
TEST(McuConferenceExpiry, EndJoinsSweeperWithPendingConferences)
{
	XmlStreamingHandler handler;
	MCU mcu;

	mcu.Init(&handler,1);

	int queueId = mcu.CreateEventQueue();
	mcu.CreateConference(L"en-attente",queueId);

	//Arrêt AVANT le premier tour de balayage
	mcu.End();

	//Les tags partent avec les conférences
	EXPECT_EQ(mcu.GetConferenceId(L"en-attente"),0);
}

}	// namespace
