#ifndef SENTPACKETHISTORY_H
#define SENTPACKETHISTORY_H

#include <map>
#include <vector>
#include "config.h"
#include "transportfeedback.h"

//Historique des paquets emis, indexe par numero de sequence transport-wide
//deroule, borne par la duree (60 s, temoin transport_feedback_adapter.cc:39).
//Apparie un rapport transport-cc du pair avec les instants d'emission locaux
//et rend des resultats prets pour l'estimateur (sender_bwe_plan.md, 6.1).
class SentPacketHistory
{
public:
	struct Result
	{
		QWORD sentTimeUs;	//horloge locale
		QWORD recvTimeUs;	//horloge du pair, deroulee, comparable entre rapports
		DWORD size;		//octets, vue transport
	};

	static const QWORD WindowUs = 60ULL * 1000000;

	SentPacketHistory();

	//A chaque emission : seq est la valeur ecrite dans l'extension (16 bits)
	void OnPacketSent(WORD seq, QWORD nowUs, DWORD size);

	//A chaque rapport recu : rend les paquets ACQUITTES, dans l'ordre du
	//rapport, avec leur instant d'arrivee deroule ; compte a part les paquets
	//declares perdus et les seq inconnus de l'historique. Un seq deja
	//acquitte est ignore (premiere arrivee gagnante : les retransmissions
	//repartent avec le meme numero, cf. ReSendPacket).
	std::vector<Result> ProcessFeedback(const TransportWideFeedbackField& feedback,
	                                    DWORD& lost, DWORD& unknown);

	DWORD GetSize() const { return history.size(); }

private:
	struct Sent
	{
		QWORD sentTimeUs;
		DWORD size;
		bool  acked;
	};

	//Deroulage 16 bits -> 64 bits, au plus proche du dernier vu
	QWORD Unwrap(WORD seq);

	std::map<QWORD, Sent> history;
	QWORD lastSeq;		//dernier seq deroule emis
	bool  hasLastSeq;
	QWORD lastRecvTicks;	//derniere arrivee deroulee (pas de 250 us)
	bool  hasLastRecvTicks;
};

#endif
