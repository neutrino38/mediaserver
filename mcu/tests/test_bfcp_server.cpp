/**
 * test_bfcp_server.cpp — BFCPFloorControlServer (bfcp/BFCPFloorControlServer.h) :
 * le serveur de contrôle de parole de la pile BFCP interne, prouvé seul sur un
 * transport factice qui enregistre ce qui part sur le fil.
 *
 * Conception : docs/conception/BFCP-INTERNE/SPEC.md, lot 0. Les scénarios sont
 * ceux que SharedDocMixer joue avec libbfcp aujourd'hui : requête → Pending,
 * octroi → Accepted puis Granted, refus, révocation par une seconde requête,
 * libération, retrait d'un utilisateur, Goodbye, FloorQuery et notification des
 * abonnés, Hello → HelloAck. Plus le contrat de verrouillage : un rappel du
 * Listener peut rappeler le serveur sans interblocage.
 */
#include <gtest/gtest.h>

#include <memory>
#include <set>
#include <vector>

#include "bfcp/BFCPFloorControlServer.h"

namespace {

// Ce qu'un message sorti du serveur contient d'observable.
struct Sent
{
	BFCPMessage::Primitive	primitive;
	int			transactionId	= 0;
	int			userId		= 0;
	int			status		= 0;	// BFCPAttrRequestStatus::Status du FloorRequestStatus
	int			floorRequestId	= 0;
	int			floorId		= 0;	// FloorStatus
	int			nbInfos		= 0;	// FloorRequestInformation d'un FloorStatus
	int			errorCode	= 0;
	std::vector<int>	primitives;		// HelloAck
};

struct FakeTransport : public BFCPTransport
{
	std::vector<Sent>	sent;
	bool			closed	 = false;
	bool			reliable = true;

	bool Send(const BFCPMessage& msg) override
	{
		Sent s;
		s.primitive	= msg.GetPrimitive();
		s.transactionId	= msg.GetTransactionId();
		s.userId	= msg.GetUserId();
		switch (msg.GetPrimitive())
		{
			case BFCPMessage::FloorRequestStatus:
			{
				const BFCPAttrFloorRequestInformation* info = static_cast<const BFCPMsgFloorRequestStatus&>(msg).GetFloorRequestInformation();
				if (info) {
					s.floorRequestId = info->GetFloorRequestId();
					if (info->GetOverallRequestStatus() && info->GetOverallRequestStatus()->GetRequestStatus())
						s.status = info->GetOverallRequestStatus()->GetRequestStatus()->GetStatus();
				}
				break;
			}
			case BFCPMessage::FloorStatus:
			{
				const BFCPMsgFloorStatus& fs = static_cast<const BFCPMsgFloorStatus&>(msg);
				if (fs.HasFloorId())
					s.floorId = fs.GetFloorId();
				s.nbInfos = fs.CountFloorRequestInformations();
				if (s.nbInfos > 0 && fs.GetFloorRequestInformation(0)->GetOverallRequestStatus())
					s.status = fs.GetFloorRequestInformation(0)->GetOverallRequestStatus()->GetRequestStatus()->GetStatus();
				break;
			}
			case BFCPMessage::Error:
				s.errorCode = static_cast<const BFCPMsgError&>(msg).GetErrorCode();
				break;
			case BFCPMessage::HelloAck:
			{
				const BFCPAttrSupportedPrimitives& p = static_cast<const BFCPMsgHelloAck&>(msg).GetSupportedPrimitives();
				for (int i=0; i<p.Count(); i++)
					s.primitives.push_back(p.Get(i));
				break;
			}
			default:
				break;
		}
		sent.push_back(s);
		return true;
	}

	void Close() override		{ closed = true; }
	bool IsReliable() const override { return reliable; }

	int Count(BFCPMessage::Primitive p) const
	{
		int n = 0;
		for (size_t i=0; i<sent.size(); i++)
			if (sent[i].primitive == p)
				n++;
		return n;
	}

	// Les statuts des FloorRequestStatus, dans l'ordre d'émission.
	std::vector<int> Statuses() const
	{
		std::vector<int> out;
		for (size_t i=0; i<sent.size(); i++)
			if (sent[i].primitive == BFCPMessage::FloorRequestStatus)
				out.push_back(sent[i].status);
		return out;
	}

	const Sent* Last(BFCPMessage::Primitive p) const
	{
		for (size_t i=sent.size(); i>0; i--)
			if (sent[i-1].primitive == p)
				return &sent[i-1];
		return nullptr;
	}
};

struct Event
{
	enum Kind { Request, Granted, Released, Connected } kind;
	int floorRequestId = 0;
	int userId = 0;		// demandeur (Request) ou bénéficiaire (Granted/Released) ou connecté
};

struct FakeListener : public BFCPFloorControlServer::Listener
{
	std::vector<Event>		events;
	// Réentrance : octroyer depuis onFloorRequest, comme un chair automatique.
	BFCPFloorControlServer*		grantFrom = nullptr;

	void onFloorRequest(int floorRequestId, int userId, int, std::set<int>) override
	{
		events.push_back({Event::Request, floorRequestId, userId});
		if (grantFrom)
			grantFrom->GrantFloorRequest(floorRequestId);
	}
	void onFloorGranted(int floorRequestId, int beneficiaryId, std::set<int>) override
	{
		events.push_back({Event::Granted, floorRequestId, beneficiaryId});
	}
	void onFloorReleased(int floorRequestId, int beneficiaryId, std::set<int>) override
	{
		events.push_back({Event::Released, floorRequestId, beneficiaryId});
	}
	void onUserConnected(int userId) override
	{
		events.push_back({Event::Connected, 0, userId});
	}

	int Count(Event::Kind k) const
	{
		int n = 0;
		for (size_t i=0; i<events.size(); i++)
			if (events[i].kind == k)
				n++;
		return n;
	}
	const Event* Last(Event::Kind k) const
	{
		for (size_t i=events.size(); i>0; i--)
			if (events[i-1].kind == k)
				return &events[i-1];
		return nullptr;
	}
};

const int CONF	= 7;
const int FLOOR	= 1;
const int ALICE	= 10;
const int BOB	= 20;

class BfcpServer : public ::testing::Test
{
protected:
	FakeListener		listener;
	BFCPFloorControlServer	server{CONF, &listener};
	FakeTransport		alice;
	FakeTransport		bob;

	void SetUp() override
	{
		ASSERT_TRUE(server.AddFloor(FLOOR));
		ASSERT_TRUE(server.AddUser(ALICE));
		ASSERT_TRUE(server.AddUser(BOB));
		ASSERT_TRUE(server.UserConnected(ALICE, &alice));
		ASSERT_TRUE(server.UserConnected(BOB, &bob));
	}

	// Une FloorRequest du user sur son transport ; rend le floorRequestId vu par le chair.
	int Request(int userId, FakeTransport& from, int transactionId)
	{
		BFCPMsgFloorRequest req(transactionId, CONF, userId);
		req.AddFloorId(FLOOR);
		int before = listener.Count(Event::Request);
		server.MessageReceived(&req, &from);
		if (listener.Count(Event::Request) == before)
			return 0;
		return listener.Last(Event::Request)->floorRequestId;
	}

	void Release(int userId, FakeTransport& from, int floorRequestId, int transactionId)
	{
		BFCPMsgFloorRelease rel(transactionId, CONF, userId);
		rel.SetFloorRequestId(floorRequestId);
		server.MessageReceived(&rel, &from);
	}
};

} // namespace


// ---- Requête et octroi ------------------------------------------------------

TEST_F(BfcpServer, UneRequeteRecoitPendingEtRemonteAuChair)
{
	int id = Request(ALICE, alice, 100);
	ASSERT_NE(0, id);

	ASSERT_EQ(1u, alice.sent.size());
	EXPECT_EQ(BFCPMessage::FloorRequestStatus, alice.sent[0].primitive);
	EXPECT_EQ(BFCPAttrRequestStatus::Pending, alice.sent[0].status);
	EXPECT_EQ(100, alice.sent[0].transactionId) << "la réponse porte le transaction id de la requête";
	EXPECT_EQ(id, alice.sent[0].floorRequestId);
	EXPECT_EQ(ALICE, listener.Last(Event::Request)->userId);
	EXPECT_TRUE(bob.sent.empty());
}

TEST_F(BfcpServer, LOctroiEmetAcceptedPuisGrantedAuDemandeur)
{
	int id = Request(ALICE, alice, 100);
	ASSERT_TRUE(server.GrantFloorRequest(id));

	std::vector<int> expected = {BFCPAttrRequestStatus::Pending, BFCPAttrRequestStatus::Accepted, BFCPAttrRequestStatus::Granted};
	EXPECT_EQ(expected, alice.Statuses());
	// Notification serveur sur transport fiable : transaction id 0 (RFC 4582 §8.3).
	EXPECT_EQ(0, alice.sent[1].transactionId);
	EXPECT_EQ(0, alice.sent[2].transactionId);

	ASSERT_EQ(1, listener.Count(Event::Granted));
	EXPECT_EQ(id, listener.Last(Event::Granted)->floorRequestId);
	EXPECT_EQ(ALICE, listener.Last(Event::Granted)->userId);
	EXPECT_EQ(id, server.GetGrantedFloorRequestId(FLOOR));
	EXPECT_TRUE(bob.sent.empty()) << "personne n'a interrogé le floor : pas de FloorStatus";
}

TEST_F(BfcpServer, UnOctroiDeuxFoisEstRefuse)
{
	int id = Request(ALICE, alice, 100);
	ASSERT_TRUE(server.GrantFloorRequest(id));
	EXPECT_FALSE(server.GrantFloorRequest(id));
	EXPECT_FALSE(server.GrantFloorRequest(9999));
}

TEST_F(BfcpServer, LeRefusEmetDeniedEtOublieLaRequete)
{
	int id = Request(ALICE, alice, 100);
	ASSERT_TRUE(server.DenyFloorRequest(id, "non"));

	EXPECT_EQ(BFCPAttrRequestStatus::Denied, alice.Last(BFCPMessage::FloorRequestStatus)->status);
	EXPECT_EQ(0, listener.Count(Event::Granted));
	EXPECT_EQ(0, listener.Count(Event::Released));
	EXPECT_FALSE(server.GrantFloorRequest(id)) << "une requête refusée n'existe plus";
	EXPECT_EQ(0, server.GetGrantedFloorRequestId(FLOOR));
}

// ---- Révocation --------------------------------------------------------------

TEST_F(BfcpServer, OctroyerAUnSecondRevoqueLePremier)
{
	int a = Request(ALICE, alice, 100);
	ASSERT_TRUE(server.GrantFloorRequest(a));
	int b = Request(BOB, bob, 200);
	ASSERT_TRUE(server.GrantFloorRequest(b));

	EXPECT_EQ(BFCPAttrRequestStatus::Revoked, alice.Last(BFCPMessage::FloorRequestStatus)->status);
	EXPECT_EQ(BFCPAttrRequestStatus::Granted, bob.Last(BFCPMessage::FloorRequestStatus)->status);

	ASSERT_EQ(1, listener.Count(Event::Released));
	EXPECT_EQ(a, listener.Last(Event::Released)->floorRequestId);
	EXPECT_EQ(ALICE, listener.Last(Event::Released)->userId);
	EXPECT_EQ(b, server.GetGrantedFloorRequestId(FLOOR));
}

TEST_F(BfcpServer, LaRevocationParLeChairEmetRevoked)
{
	int id = Request(ALICE, alice, 100);
	ASSERT_TRUE(server.GrantFloorRequest(id));
	ASSERT_TRUE(server.RevokeFloorRequest(id, "stop"));

	EXPECT_EQ(BFCPAttrRequestStatus::Revoked, alice.Last(BFCPMessage::FloorRequestStatus)->status);
	EXPECT_EQ(1, listener.Count(Event::Released));
	EXPECT_EQ(0, server.GetGrantedFloorRequestId(FLOOR));
	EXPECT_FALSE(server.RevokeFloorRequest(id)) << "révoquer deux fois échoue";
}

TEST_F(BfcpServer, RevoquerUneRequeteNonOctroyeeEchoue)
{
	int id = Request(ALICE, alice, 100);
	EXPECT_FALSE(server.RevokeFloorRequest(id));
	EXPECT_EQ(0, listener.Count(Event::Released));
}

// ---- Libération --------------------------------------------------------------

TEST_F(BfcpServer, LeDetenteurQuiLibereRecoitReleased)
{
	int id = Request(ALICE, alice, 100);
	ASSERT_TRUE(server.GrantFloorRequest(id));
	Release(ALICE, alice, id, 101);

	const Sent* last = alice.Last(BFCPMessage::FloorRequestStatus);
	EXPECT_EQ(BFCPAttrRequestStatus::Released, last->status);
	EXPECT_EQ(101, last->transactionId) << "la réponse porte le transaction id du FloorRelease";
	ASSERT_EQ(1, listener.Count(Event::Released));
	EXPECT_EQ(id, listener.Last(Event::Released)->floorRequestId);
	EXPECT_EQ(0, server.GetGrantedFloorRequestId(FLOOR));
}

TEST_F(BfcpServer, LibererUneRequetePendanteLAnnuleSansPrevenirLeChair)
{
	int id = Request(ALICE, alice, 100);
	Release(ALICE, alice, id, 101);

	EXPECT_EQ(BFCPAttrRequestStatus::Cancelled, alice.Last(BFCPMessage::FloorRequestStatus)->status);
	EXPECT_EQ(0, listener.Count(Event::Released)) << "rien n'était tenu, rien n'est relâché";
	EXPECT_FALSE(server.GrantFloorRequest(id));
}

TEST_F(BfcpServer, UnTiersNePeutPasLibererLaRequeteDUnAutre)
{
	int id = Request(ALICE, alice, 100);
	ASSERT_TRUE(server.GrantFloorRequest(id));
	Release(BOB, bob, id, 200);

	ASSERT_EQ(1, bob.Count(BFCPMessage::Error));
	EXPECT_EQ(BFCPAttrErrorCode::UnauthorizedOperation, bob.Last(BFCPMessage::Error)->errorCode);
	EXPECT_EQ(id, server.GetGrantedFloorRequestId(FLOOR));
}

TEST_F(BfcpServer, LibererUneRequeteInconnueRendUneErreur)
{
	Release(ALICE, alice, 4242, 101);
	ASSERT_EQ(1, alice.Count(BFCPMessage::Error));
	EXPECT_EQ(BFCPAttrErrorCode::FloorRequestIdDoesNotExist, alice.Last(BFCPMessage::Error)->errorCode);
}

// ---- Départ d'un utilisateur -------------------------------------------------

TEST_F(BfcpServer, RetirerLeDetenteurRevoqueEtDitGoodbye)
{
	int id = Request(ALICE, alice, 100);
	ASSERT_TRUE(server.GrantFloorRequest(id));
	ASSERT_TRUE(server.RemoveUser(ALICE));

	EXPECT_EQ(BFCPAttrRequestStatus::Revoked, alice.Last(BFCPMessage::FloorRequestStatus)->status);
	EXPECT_EQ(1, alice.Count(BFCPMessage::Goodbye));
	EXPECT_TRUE(alice.closed);
	ASSERT_EQ(1, listener.Count(Event::Released));
	EXPECT_EQ(ALICE, listener.Last(Event::Released)->userId);
	EXPECT_EQ(0, server.GetGrantedFloorRequestId(FLOOR));
	EXPECT_FALSE(server.RemoveUser(ALICE)) << "retirer deux fois échoue";
}

TEST_F(BfcpServer, UnGoodbyeDuClientEstAcquitteEtLeRetire)
{
	int id = Request(ALICE, alice, 100);
	ASSERT_TRUE(server.GrantFloorRequest(id));

	BFCPMessage bye(BFCPMessage::Goodbye, 102, CONF, ALICE);
	server.MessageReceived(&bye, &alice);

	const Sent* ack = alice.Last(BFCPMessage::GoodbyeAck);
	ASSERT_NE(nullptr, ack);
	EXPECT_EQ(102, ack->transactionId);
	EXPECT_EQ(0, alice.Count(BFCPMessage::Goodbye)) << "on n'envoie pas Goodbye à qui vient de le dire";
	EXPECT_TRUE(alice.closed);
	EXPECT_EQ(1, listener.Count(Event::Released));
	EXPECT_EQ(0, server.GetGrantedFloorRequestId(FLOOR));

	// L'utilisateur n'existe plus : un message de sa part est une erreur.
	Request(ALICE, alice, 103);
	EXPECT_EQ(BFCPAttrErrorCode::UserDoesNotExist, alice.Last(BFCPMessage::Error)->errorCode);
}

TEST_F(BfcpServer, LaDeconnexionDuTransportRevoqueLeFloor)
{
	int id = Request(ALICE, alice, 100);
	ASSERT_TRUE(server.GrantFloorRequest(id));
	server.UserDisconnected(ALICE, &alice);

	EXPECT_EQ(1, listener.Count(Event::Released));
	EXPECT_EQ(0, server.GetGrantedFloorRequestId(FLOOR));
	EXPECT_FALSE(alice.closed) << "le transport s'est fermé tout seul, le serveur ne le referme pas";
}

TEST_F(BfcpServer, LaDeconnexionDUnAncienTransportEstIgnoree)
{
	FakeTransport alice2;
	BFCPMsgHello hello(1, CONF, ALICE);
	server.MessageReceived(&hello, &alice2);		// alice2 reprend ALICE
	EXPECT_TRUE(alice.closed);

	int id = Request(ALICE, alice2, 100);
	ASSERT_TRUE(server.GrantFloorRequest(id));
	server.UserDisconnected(ALICE, &alice);		// l'ancien transport tombe : sans effet
	EXPECT_EQ(id, server.GetGrantedFloorRequestId(FLOOR));
	EXPECT_EQ(0, listener.Count(Event::Released));
}

// ---- Transport et identité ---------------------------------------------------

TEST_F(BfcpServer, UnUtilisateurInconnuRecoitUneErreurSurSonTransport)
{
	FakeTransport stranger;
	BFCPMsgFloorRequest req(1, CONF, 99);
	req.AddFloorId(FLOOR);
	server.MessageReceived(&req, &stranger);

	ASSERT_EQ(1u, stranger.sent.size());
	EXPECT_EQ(BFCPMessage::Error, stranger.sent[0].primitive);
	EXPECT_EQ(BFCPAttrErrorCode::UserDoesNotExist, stranger.sent[0].errorCode);
	EXPECT_EQ(99, stranger.sent[0].userId);
	EXPECT_EQ(0, listener.Count(Event::Request));
}

TEST_F(BfcpServer, UneAutreConferenceEstRefusee)
{
	BFCPMsgHello hello(1, CONF + 1, ALICE);
	server.MessageReceived(&hello, &alice);
	ASSERT_EQ(1, alice.Count(BFCPMessage::Error));
	EXPECT_EQ(BFCPAttrErrorCode::ConferenceDoesNotExist, alice.Last(BFCPMessage::Error)->errorCode);
}

TEST_F(BfcpServer, ParlerDepuisUnAutreTransportSansHelloEstRefuse)
{
	FakeTransport intruder;
	BFCPMsgFloorRequest req(1, CONF, ALICE);
	req.AddFloorId(FLOOR);
	server.MessageReceived(&req, &intruder);

	ASSERT_EQ(1, intruder.Count(BFCPMessage::Error));
	EXPECT_EQ(BFCPAttrErrorCode::UnauthorizedOperation, intruder.Last(BFCPMessage::Error)->errorCode);
	EXPECT_FALSE(alice.closed);
	EXPECT_EQ(0, listener.Count(Event::Request));
}

TEST_F(BfcpServer, UnHelloDepuisUnAutreTransportReprendLUtilisateur)
{
	int id = Request(ALICE, alice, 100);
	ASSERT_TRUE(server.GrantFloorRequest(id));

	FakeTransport alice2;
	BFCPMsgHello hello(1, CONF, ALICE);
	server.MessageReceived(&hello, &alice2);

	EXPECT_TRUE(alice.closed) << "l'ancien transport est fermé";
	EXPECT_EQ(1, alice2.Count(BFCPMessage::HelloAck));
	EXPECT_EQ(1, listener.Count(Event::Released)) << "la parole tenue par l'ancienne connexion tombe";
	EXPECT_EQ(0, server.GetGrantedFloorRequestId(FLOOR));

	int id2 = Request(ALICE, alice2, 101);
	EXPECT_NE(0, id2);
}

TEST_F(BfcpServer, UnPremierMessageSansHelloAttacheLeTransport)
{
	ASSERT_TRUE(server.AddUser(30));
	FakeTransport carol;
	BFCPMsgFloorRequest req(1, CONF, 30);
	req.AddFloorId(FLOOR);
	server.MessageReceived(&req, &carol);

	EXPECT_EQ(1, carol.Count(BFCPMessage::FloorRequestStatus)) << "la RFC n'impose pas Hello d'abord";
	EXPECT_EQ(1, listener.Count(Event::Request));
}

// ---- Hello -------------------------------------------------------------------

TEST_F(BfcpServer, HelloRecoitHelloAckAvecLesPrimitivesEtPrevientLeChair)
{
	BFCPMsgHello hello(55, CONF, ALICE);
	server.MessageReceived(&hello, &alice);

	ASSERT_EQ(1u, alice.sent.size()) << "sur TCP, pas de Hello serveur en retour";
	const Sent& ack = alice.sent[0];
	EXPECT_EQ(BFCPMessage::HelloAck, ack.primitive);
	EXPECT_EQ(55, ack.transactionId);
	std::set<int> prims(ack.primitives.begin(), ack.primitives.end());
	EXPECT_TRUE(prims.count(BFCPMessage::FloorRequest));
	EXPECT_TRUE(prims.count(BFCPMessage::FloorRelease));
	EXPECT_TRUE(prims.count(BFCPMessage::FloorQuery));
	EXPECT_TRUE(prims.count(BFCPMessage::Goodbye));
	EXPECT_FALSE(prims.count(BFCPMessage::ChairAction));

	ASSERT_EQ(1, listener.Count(Event::Connected));
	EXPECT_EQ(ALICE, listener.Last(Event::Connected)->userId);
}

TEST_F(BfcpServer, SurTransportNonFiableLeServeurRepondEtSalueEnRetour)
{
	alice.reliable = false;
	BFCPMsgHello hello(55, CONF, ALICE);
	server.MessageReceived(&hello, &alice);

	ASSERT_EQ(2u, alice.sent.size());
	EXPECT_EQ(BFCPMessage::HelloAck, alice.sent[0].primitive);
	EXPECT_EQ(BFCPMessage::Hello, alice.sent[1].primitive);
	EXPECT_NE(0, alice.sent[1].transactionId) << "une transaction serveur, à acquitter";

	// Les notifications portent alors un transaction id non nul.
	int id = Request(ALICE, alice, 100);
	ASSERT_TRUE(server.GrantFloorRequest(id));
	const Sent* granted = alice.Last(BFCPMessage::FloorRequestStatus);
	EXPECT_EQ(BFCPAttrRequestStatus::Granted, granted->status);
	EXPECT_NE(0, granted->transactionId);
}

TEST_F(BfcpServer, LesAcquittementsSontAcceptesEnSilence)
{
	BFCPMessage ack1(BFCPMessage::FloorRequestStatusAck, 3, CONF, ALICE);
	BFCPMessage ack2(BFCPMessage::FloorStatusAck, 4, CONF, ALICE);
	BFCPMessage ack3(BFCPMessage::HelloAck, 5, CONF, ALICE);
	server.MessageReceived(&ack1, &alice);
	server.MessageReceived(&ack2, &alice);
	server.MessageReceived(&ack3, &alice);
	EXPECT_TRUE(alice.sent.empty());
}

TEST_F(BfcpServer, UnePrimitiveNonPorteeRecoitUnknownPrimitive)
{
	BFCPMessage chair(BFCPMessage::ChairAction, 9, CONF, ALICE);
	server.MessageReceived(&chair, &alice);
	ASSERT_EQ(1, alice.Count(BFCPMessage::Error));
	EXPECT_EQ(BFCPAttrErrorCode::UnknownPrimitive, alice.Last(BFCPMessage::Error)->errorCode);
	EXPECT_EQ(9, alice.Last(BFCPMessage::Error)->transactionId);
}

// ---- FloorQuery et abonnement -----------------------------------------------

TEST_F(BfcpServer, FloorQueryRepondFloorStatusPuisNotifieLesChangements)
{
	BFCPMsgFloorQuery query(70, CONF, BOB);
	query.AddFloorId(FLOOR);
	server.MessageReceived(&query, &bob);

	ASSERT_EQ(1u, bob.sent.size());
	EXPECT_EQ(BFCPMessage::FloorStatus, bob.sent[0].primitive);
	EXPECT_EQ(70, bob.sent[0].transactionId);
	EXPECT_EQ(FLOOR, bob.sent[0].floorId);
	EXPECT_EQ(0, bob.sent[0].nbInfos) << "personne ne demande le floor";

	int id = Request(ALICE, alice, 100);
	ASSERT_EQ(1, bob.Count(BFCPMessage::FloorStatus)) << "une requête Pending ne notifie pas encore";

	ASSERT_TRUE(server.GrantFloorRequest(id));
	const Sent* fs = bob.Last(BFCPMessage::FloorStatus);
	ASSERT_EQ(2, bob.Count(BFCPMessage::FloorStatus));
	EXPECT_EQ(1, fs->nbInfos);
	EXPECT_EQ(BFCPAttrRequestStatus::Granted, fs->status);
	EXPECT_EQ(0, fs->transactionId) << "notification : transaction id 0 sur TCP";

	Release(ALICE, alice, id, 101);
	fs = bob.Last(BFCPMessage::FloorStatus);
	ASSERT_EQ(3, bob.Count(BFCPMessage::FloorStatus));
	EXPECT_EQ(BFCPAttrRequestStatus::Released, fs->status);
	EXPECT_TRUE(alice.Count(BFCPMessage::FloorStatus) == 0) << "Alice n'a pas interrogé le floor";
}

TEST_F(BfcpServer, FloorQueryVideDesabonne)
{
	BFCPMsgFloorQuery query(70, CONF, BOB);
	query.AddFloorId(FLOOR);
	server.MessageReceived(&query, &bob);
	BFCPMsgFloorQuery none(71, CONF, BOB);
	server.MessageReceived(&none, &bob);
	ASSERT_EQ(2, bob.Count(BFCPMessage::FloorStatus));
	EXPECT_EQ(0, bob.sent[1].floorId) << "réponse vide, sans FLOOR-ID";

	int id = Request(ALICE, alice, 100);
	ASSERT_TRUE(server.GrantFloorRequest(id));
	EXPECT_EQ(2, bob.Count(BFCPMessage::FloorStatus)) << "plus abonné, plus notifié";
}

TEST_F(BfcpServer, FloorQueryDUnFloorInconnuRendInvalidFloorId)
{
	BFCPMsgFloorQuery query(70, CONF, BOB);
	query.AddFloorId(FLOOR + 5);
	server.MessageReceived(&query, &bob);
	ASSERT_EQ(1, bob.Count(BFCPMessage::Error));
	EXPECT_EQ(BFCPAttrErrorCode::InvalidFloorId, bob.Last(BFCPMessage::Error)->errorCode);
}

TEST_F(BfcpServer, NotifyFloorStatusPousseLEtatDuFloorAUnUtilisateur)
{
	int id = Request(ALICE, alice, 100);
	ASSERT_TRUE(server.GrantFloorRequest(id));

	ASSERT_TRUE(server.NotifyFloorStatus(BOB, FLOOR));
	const Sent* fs = bob.Last(BFCPMessage::FloorStatus);
	ASSERT_NE(nullptr, fs);
	EXPECT_EQ(FLOOR, fs->floorId);
	EXPECT_EQ(1, fs->nbInfos);
	EXPECT_EQ(BFCPAttrRequestStatus::Granted, fs->status);

	EXPECT_FALSE(server.NotifyFloorStatus(99, FLOOR));
	EXPECT_FALSE(server.NotifyFloorStatus(BOB, FLOOR + 5));
}

// ---- Erreurs de requête ------------------------------------------------------

TEST_F(BfcpServer, DemanderUnFloorInconnuRendInvalidFloorId)
{
	BFCPMsgFloorRequest req(1, CONF, ALICE);
	req.AddFloorId(FLOOR + 5);
	server.MessageReceived(&req, &alice);
	ASSERT_EQ(1, alice.Count(BFCPMessage::Error));
	EXPECT_EQ(BFCPAttrErrorCode::InvalidFloorId, alice.Last(BFCPMessage::Error)->errorCode);
	EXPECT_EQ(0, listener.Count(Event::Request));
}

TEST_F(BfcpServer, UnBeneficiaireInconnuRendUserDoesNotExist)
{
	BFCPMsgFloorRequest req(1, CONF, ALICE);
	req.AddFloorId(FLOOR);
	req.SetBeneficiaryId(99);
	server.MessageReceived(&req, &alice);
	ASSERT_EQ(1, alice.Count(BFCPMessage::Error));
	EXPECT_EQ(BFCPAttrErrorCode::UserDoesNotExist, alice.Last(BFCPMessage::Error)->errorCode);
}

// ---- Chair et réentrance -----------------------------------------------------

TEST_F(BfcpServer, LeChairQuiRappelleLeServeurDepuisOnFloorRequestNeBloquePas)
{
	listener.grantFrom = &server;
	int id = Request(ALICE, alice, 100);
	ASSERT_NE(0, id);

	std::vector<int> expected = {BFCPAttrRequestStatus::Pending, BFCPAttrRequestStatus::Accepted, BFCPAttrRequestStatus::Granted};
	EXPECT_EQ(expected, alice.Statuses());
	EXPECT_EQ(1, listener.Count(Event::Granted));
	EXPECT_EQ(id, server.GetGrantedFloorRequestId(FLOOR));
}

TEST_F(BfcpServer, LaRequeteDUnChairEstOctroyeeSansDemander)
{
	ASSERT_TRUE(server.SetChair(BOB));
	int before = listener.Count(Event::Request);
	BFCPMsgFloorRequest req(1, CONF, BOB);
	req.AddFloorId(FLOOR);
	server.MessageReceived(&req, &bob);

	EXPECT_EQ(before, listener.Count(Event::Request)) << "le chair ne se demande pas la permission";
	EXPECT_EQ(1, listener.Count(Event::Granted));
	EXPECT_EQ(BFCPAttrRequestStatus::Granted, bob.Last(BFCPMessage::FloorRequestStatus)->status);
	EXPECT_FALSE(server.SetChair(99));
}

// ---- Fin de conférence -------------------------------------------------------

TEST_F(BfcpServer, EndRevoqueToutDitGoodbyeEtFermeLesTransports)
{
	int a = Request(ALICE, alice, 100);
	ASSERT_TRUE(server.GrantFloorRequest(a));
	Request(BOB, bob, 200);

	server.End();

	EXPECT_EQ(BFCPAttrRequestStatus::Revoked, alice.Last(BFCPMessage::FloorRequestStatus)->status);
	EXPECT_EQ(BFCPAttrRequestStatus::Denied, bob.Last(BFCPMessage::FloorRequestStatus)->status);
	EXPECT_EQ(1, alice.Count(BFCPMessage::Goodbye));
	EXPECT_EQ(1, bob.Count(BFCPMessage::Goodbye));
	EXPECT_TRUE(alice.closed);
	EXPECT_TRUE(bob.closed);
	EXPECT_EQ(0, listener.Count(Event::Released)) << "la conférence finit : le chair n'est plus prévenu";

	EXPECT_FALSE(server.AddUser(40));
	EXPECT_EQ(0, server.GetGrantedFloorRequestId(FLOOR));
	server.End();	// idempotent
}

TEST_F(BfcpServer, LesIdentifiantsHorsPlageSontRefuses)
{
	EXPECT_FALSE(server.AddUser(0));
	EXPECT_FALSE(server.AddUser(70000));
	EXPECT_FALSE(server.AddUser(ALICE)) << "déjà présent";
	EXPECT_FALSE(server.AddFloor(0));
	EXPECT_FALSE(server.AddFloor(FLOOR)) << "déjà présent";
}
