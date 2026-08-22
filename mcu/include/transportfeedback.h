#ifndef TRANSPORTFEEDBACK_H
#define TRANSPORTFEEDBACK_H

#include <map>
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

//Deroulage d'un compteur 16 bits vers 64 bits, au plus proche du dernier vu :
//une avance de moins de 32768 est une avance, sinon c'est un retour. Partage
//par l'historique d'emission et par le generateur de rapports — les deux
//comptent le meme numero de sequence transport-wide.
inline QWORD UnwrapSeq16(WORD seq, QWORD lastSeq, bool hasLastSeq)
{
	if (!hasLastSeq)
		return seq;
	QWORD cycles = lastSeq & ~0xFFFFULL;
	QWORD candidate = cycles | seq;
	WORD last = (WORD)(lastSeq & 0xFFFF);
	WORD forward = (WORD)(seq - last);
	if (forward < 0x8000)
	{
		if (candidate < lastSeq)
			candidate += 0x10000;
	} else {
		if (candidate >= lastSeq && candidate >= 0x10000)
			candidate -= 0x10000;
	}
	return candidate;
}

//Accumulateur des arrivees et fabricant du rapport fmt 15 (lot 4). Symetrique
//de SentPacketHistory : celui-ci retient ce que NOUS recevons, pour le
//rapporter au pair qui nous l'envoie — c'est ce rapport qui nourrit son
//estimateur emetteur. Confine au thread de reception de la session.
class TransportWideFeedbackGenerator
{
public:
	//Cadence du temoin, transport_sequence_number_feedback_generator.cc:37-40
	static constexpr QWORD MinIntervalUs     =  50000;
	static constexpr QWORD MaxIntervalUs     = 250000;
	static constexpr QWORD DefaultIntervalUs = 100000;
	//Une arrivee reste en memoire ce temps la : un paquet reordonne arrive
	//apres son rapport, et il faut pouvoir le rapporter quand meme (idem :37)
	static constexpr QWORD BackWindowUs      = 500000;
	//Taille moyenne d'un rapport sur le fil, IP+UDP+SRTP compris (idem :150-153)
	static constexpr DWORD ReportBytes       = 20 + 8 + 10 + 30;
	//Un rapport doit tenir dans un datagramme : au-dela, il se scinde. Les
	//trous comptent, donc c'est bien un nombre de STATUTS, pas de paquets.
	static constexpr DWORD MaxStatusPerReport = 400;

	TransportWideFeedbackGenerator();

	//A chaque paquet RTP portant l'extension transport-wide
	void OnPacketReceived(DWORD ssrc, WORD seq, QWORD nowUs);

	//Reste-t-il des arrivees non rapportees ?
	bool HasPending() const;
	//... et l'intervalle de cadence est-il ecoule ?
	bool ShouldSend(QWORD nowUs) const;

	//Remplit le champ avec les arrivees en attente. Rend false s'il n'y a rien
	//a dire. Ce qui ne tient pas dans ce rapport part au suivant.
	bool BuildFeedback(TransportWideFeedbackField& field, QWORD nowUs);

	//Debit d'emission connu (bps) : les rapports visent 5 % du lien
	//(temoin :143-165). Sans appel, l'intervalle vaut DefaultIntervalUs.
	void SetSendBitrate(DWORD bitrate);

	DWORD GetMediaSSRC() const { return mediaSSRC; }
	QWORD GetIntervalUs() const { return intervalUs; }
	DWORD GetPendingCount() const;

private:
	std::map<QWORD,QWORD> arrivals;	//seq deroule -> arrivee (us, horloge locale)
	QWORD lastSeq;
	bool  hasLastSeq;
	QWORD windowStart;		//premier seq deroule restant a rapporter
	bool  hasWindowStart;
	QWORD lastSendUs;
	QWORD intervalUs;
	DWORD mediaSSRC;
	BYTE  fbSeq;
};

#endif
