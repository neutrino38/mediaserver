#include "sentpackethistory.h"

SentPacketHistory::SentPacketHistory()
{
	lastSeq = 0;
	hasLastSeq = false;
	lastRecvTicks = 0;
	hasLastRecvTicks = false;
}

QWORD SentPacketHistory::Unwrap(WORD seq)
{
	return UnwrapSeq16(seq, lastSeq, hasLastSeq);
}

void SentPacketHistory::OnPacketSent(WORD seq, QWORD nowUs, DWORD size)
{
	QWORD unwrapped = Unwrap(seq);
	if (!hasLastSeq || unwrapped > lastSeq)
	{
		lastSeq = unwrapped;
		hasLastSeq = true;
	}
	history[unwrapped] = { nowUs, size, false };
	//Purge par la duree
	while (!history.empty() && history.begin()->second.sentTimeUs + WindowUs < nowUs)
		history.erase(history.begin());
}

std::vector<SentPacketHistory::Result> SentPacketHistory::ProcessFeedback(
	const TransportWideFeedbackField& feedback, DWORD& lost, DWORD& unknown)
{
	std::vector<Result> results;
	lost = 0;
	unknown = 0;

	//Derouler le temps de reference du rapport (compteur 24 bits en pas de
	//64 ms, horloge du recepteur) au plus proche de la derniere arrivee vue
	QWORD baseTicks = (QWORD)feedback.referenceTicks << 8;
	static const QWORD WrapTicks = (QWORD)1 << 32;	//2^24 pas de 64 ms = 2^32 pas de 250 us
	if (hasLastRecvTicks)
	{
		QWORD cycles = lastRecvTicks / WrapTicks;
		QWORD candidate = cycles * WrapTicks + baseTicks;
		QWORD up   = candidate + WrapTicks;
		QWORD down = candidate >= WrapTicks ? candidate - WrapTicks : candidate;
		QWORD best = candidate;
		auto dist = [&](QWORD v) { return v > lastRecvTicks ? v - lastRecvTicks : lastRecvTicks - v; };
		if (dist(up) < dist(best)) best = up;
		if (dist(down) < dist(best)) best = down;
		baseTicks = best;
	}

	QWORD arrivalTicks = baseTicks;
	bool sawReceived = false;
	for (const TransportWideFeedbackField::PacketStatus& p : feedback.packets)
	{
		if (!p.received)
		{
			lost++;
			continue;
		}
		sawReceived = true;
		if (feedback.hasTimestamps)
			arrivalTicks += p.deltaTicks;
		QWORD unwrapped = Unwrap(p.seq);
		std::map<QWORD, Sent>::iterator it = history.find(unwrapped);
		if (it == history.end())
		{
			unknown++;
			continue;
		}
		if (it->second.acked)
			//Retransmission acquittee deux fois : premiere arrivee gagnante
			continue;
		it->second.acked = true;
		results.push_back({ it->second.sentTimeUs,
		                    arrivalTicks * TransportWideFeedbackField::TickUs,
		                    it->second.size });
	}
	if (feedback.hasTimestamps && sawReceived)
	{
		lastRecvTicks = arrivalTicks;
		hasLastRecvTicks = true;
	}
	return results;
}
