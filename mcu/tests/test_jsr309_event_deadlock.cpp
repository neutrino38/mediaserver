/**
 * test_jsr309_event_deadlock.cpp — publication d'un événement depuis le CHEMIN
 * DES PAQUETS, pendant un détachement concurrent.
 *
 * Lot 0 de `jsr309_transcode_sans_thread.md` (§4.2). Le cycle, préexistant :
 *
 *   thread de démultiplexage : Port(source).mutex tenu par `Multiplex`
 *                              → Joinable::Update() d'un listener
 *                              → PostEvent → JSR309Manager::PostEvent
 *                              → MediaSession::GetEventContext
 *   thread XML-RPC           : MediaSession::mutex tenu par EndpointDettach
 *                              → Port::Detach → RemoveListener
 *                              → Port(source).mutex
 *
 * Deux verrous, deux ordres opposés. Il ne se voyait pas parce que le seul
 * chemin qui publie sous le verrou du port demande `useExtFIR`, faux par
 * défaut ; le chantier « transcodeurs sans thread » y fait passer, en plus,
 * les demandes de FPU du décodeur. La réponse : les contextes d'événement ont
 * un verrou à eux, plus fin, que personne ne prend avant celui du port.
 *
 * Le test reproduit le cycle avec les vraies classes : un vrai
 * `RTPMultiplexer` comme source (c'est son mutex qui fait barrière), un vrai
 * `Endpoint` de session comme puits, et le vrai `EndpointDettach`.
 *
 * ATTENTION : une régression ici est un interblocage. Le test le borne dans le
 * temps et TUE le processus plutôt que de laisser la suite pendre.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>

#include "../src/jsr309/JSR309Manager.h"
#include "../src/jsr309/Endpoint.h"
#include "../src/jsr309/RTPEndpoint.h"
#include "../src/jsr309/RTPMultiplexer.h"

namespace {

typedef std::chrono::milliseconds Ms;

// Source de paquets, calquée sur RTPEndpoint : c'est un RTPMultiplexer (donc
// son `Multiplex` tient le mutex pendant tout l'appel aux listeners) et son
// `Update()` publie un événement au lieu de faire suivre en amont — exactement
// ce que fait RTPEndpoint::Update quand `useExtFIR` est vrai.
class EventPostingSource : public RTPMultiplexer
{
public:
	void Update() override
	{
		PostEvent(new ::ExternalFIRRequestedEvent());
		posted = true;
	}

	std::atomic<bool> posted { false };
};

// Écouteur qui, sous le verrou du port source, demande une intra à la source —
// le geste de VideoTranscoder::RequestSourceFPU et du décodeur sur perte.
// Il attend d'abord que le détacheur soit ENGAGÉ, pour que les deux verrous
// soient bien pris dans les deux ordres opposés.
class FpuRequestingListener : public Joinable::Listener
{
public:
	FpuRequestingListener(EventPostingSource& source) : source(source) {}

	void onRTPPacket(RTPPacket& packet) override
	{
		//Signale que le verrou du port est tenu
		{
			std::lock_guard<std::mutex> lock(mutex);
			inside = true;
		}
		cond.notify_all();

		//Laisse le thread « XML-RPC » entrer dans EndpointDettach et venir
		//buter sur ce verrou-ci.
		std::this_thread::sleep_for(Ms(200));

		//Et c'est ici que se referme le cycle, si cycle il y a.
		source.Update();
	}

	void onResetStream() override {}
	void onEndStream() override {}

	//Attend l'entrée dans onRTPPacket
	bool WaitInside(int timeoutMs)
	{
		std::unique_lock<std::mutex> lock(mutex);
		return cond.wait_for(lock, Ms(timeoutMs), [this] { return inside; });
	}

private:
	EventPostingSource&	source;
	std::mutex		mutex;
	std::condition_variable	cond;
	bool			inside = false;
};

// Une régression rend les deux threads injoignables : mieux vaut mourir en le
// disant que faire pendre `make check` sans un mot.
static void DieOnDeadlock(const char* what)
{
	fprintf(stderr,
		"\nINTERBLOCAGE : %s n'a pas rendu la main.\n"
		"Port(source).mutex -> MediaSession::mutex a ete repris quelque part "
		"(cf. jsr309_transcode_sans_thread.md §4.2).\n", what);
	fflush(stderr);
	std::abort();
}

TEST(JSR309EventDeadlock, PostingFromThePacketPathDoesNotBlockOnTheSessionMutex)
{
	XmlStreamingHandler handler;
	JSR309Manager manager;
	//0 : pas de balayage par expiration, il n'a rien à voir avec ce test
	ASSERT_TRUE(manager.Init(&handler, 0));

	int sessionId = manager.CreateMediaSession(L"deadlock", 0);
	ASSERT_GT(sessionId, 0);

	std::shared_ptr<MediaSession> session;
	ASSERT_TRUE(manager.GetMediaSessionRef(sessionId, session));

	//Puits : un endpoint audio de la session. Sa création lui donne son
	//contexte d'événement — celui que la source publiera.
	int endpointId = session->EndpointCreate(L"sink", true, false, false);
	ASSERT_GT(endpointId, 0);

	std::shared_ptr<Endpoint> endpoint = session->GetEndpoint(endpointId);
	ASSERT_TRUE((bool)endpoint);

	std::shared_ptr<Joinable> sinkPort = endpoint->GetJoinable(MediaFrame::Audio);
	ASSERT_TRUE((bool)sinkPort);
	int eventContextId = sinkPort->GetEventContextId();
	ASSERT_GT(eventContextId, 0);

	//Source : elle publie dans le contexte du puits, comme un RTPEndpoint amont
	auto source = std::make_shared<EventPostingSource>();
	source->SetEventHandler(sessionId, &manager);
	source->SetEventContextId(eventContextId);

	//Câblage réel : le port du puits s'inscrit chez la source. C'est ce lien
	//que EndpointDettach défera, en tenant le mutex de session.
	ASSERT_EQ(1, endpoint->Attach(MediaFrame::Audio, MediaFrame::VIDEO_MAIN, source));

	FpuRequestingListener listener(*source);
	source->AddListener(&listener);

	//Thread « démultiplexage » : publie un paquet, donc tient le mutex du port
	std::atomic<bool> multiplexed { false };
	std::thread demux([&]() {
		RTPPacket packet(MediaFrame::Audio, AudioCodec::PCMU);
		packet.SetSeqNum(1);
		packet.SetTimestamp(160);
		packet.SetMediaLength(0);
		source->Multiplex(packet);
		multiplexed = true;
	});

	ASSERT_TRUE(listener.WaitInside(2000)) << "le paquet n'a jamais atteint l'ecouteur";

	//Thread « XML-RPC » : prend le mutex de session et vient buter sur celui du
	//port. C'est l'ordre INVERSE de celui du thread de démultiplexage.
	std::atomic<bool> detached { false };
	std::thread control([&]() {
		session->EndpointDettach(endpointId, MediaFrame::Audio);
		detached = true;
	});

	//Les deux doivent rendre la main. Sinon, c'est le cycle.
	for (int waited = 0; !(multiplexed && detached); waited += 25)
	{
		if (waited > 5000)
			DieOnDeadlock("Multiplex/EndpointDettach concurrents");
		std::this_thread::sleep_for(Ms(25));
	}

	demux.join();
	control.join();

	EXPECT_TRUE(source->posted) << "l'evenement doit avoir ete publie, pas seulement evite";

	source->RemoveListener(&listener);
	manager.DeleteMediaSession(sessionId);
	manager.End();
}

}  // namespace
