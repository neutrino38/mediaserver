#ifndef TRANSPORTFEEDBACK_H
#define TRANSPORTFEEDBACK_H

#include <vector>
#include "config.h"
#include "rtp.h"

//RTCP RTPFB fmt 15 (draft-holmer-rmcat-transport-wide-cc-extensions-01) :
//temps d'arrivee rapportes par numero de sequence transport-wide. Module
//partage entre le lot 4 (nous construisons le rapport) et le lot 6 (nous
//consommons celui du pair) — sender_bwe_plan.md, D3. Le corps est a taille
//variable : il occupe tout le paquet apres les deux SSRC, donc UN seul champ
//par paquet.
struct TransportWideFeedbackField : public RTCPRTPFeedback::Field
{
	//Un statut par numero de sequence couvert, contigus depuis baseSeq.
	//deltaTicks n'a de sens que si received : difference d'arrivee avec le
	//paquet recu precedent (le premier est relatif au temps de reference),
	//en pas de 250 us, signee.
	struct PacketStatus
	{
		WORD seq;
		bool received;
		int  deltaTicks;
	};

	static const int   TickUs     = 250;
	static const QWORD BaseTickUs = (QWORD)TickUs << 8;	//64 ms
	static const QWORD TimeWrapUs = BaseTickUs << 24;	//~12,4 jours

	WORD  baseSeq;
	DWORD referenceTicks;	//24 bits, pas de 64 ms, horloge du RECEPTEUR
	BYTE  fbSeq;
	bool  hasTimestamps;	//le format autorise un rapport sans deltas
	std::vector<PacketStatus> packets;

	TransportWideFeedbackField();

	virtual DWORD GetSize();
	virtual DWORD Parse(BYTE* data, DWORD size);
	virtual DWORD Serialize(BYTE* data, DWORD size);

	//Construction (lot 4). SetBase fixe le premier seq couvert et le temps de
	//reference ; AddReceived complete les trous en "perdu" et rend false si le
	//delta ne tient pas dans le format (il faut alors scinder le rapport).
	void SetBase(WORD seq, QWORD arrivalUs);
	bool AddReceived(WORD seq, QWORD arrivalUs);

private:
	QWORD lastArrivalTicks;
	bool Encode(std::vector<WORD>& chunks, std::vector<BYTE>& deltas) const;
};

#endif
