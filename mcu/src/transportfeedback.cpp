#include <cstring>
#include <iterator>
#include "transportfeedback.h"

//Symboles du format : la taille du delta EST le symbole (0 = perdu, 1 = petit
//delta sur 1 octet, 2 = grand delta signe sur 2 octets, 3 = reserve).
enum { SymbolNotReceived = 0, SymbolSmall = 1, SymbolLarge = 2, SymbolReserved = 3 };

static const DWORD HeaderBytes = 8;	//baseSeq(2) + count(2) + refTime(3) + fbSeq(1)
static const DWORD MaxRunLength = 0x1FFF;

TransportWideFeedbackField::TransportWideFeedbackField()
{
	baseSeq = 0;
	referenceTicks = 0;
	fbSeq = 0;
	hasTimestamps = true;
	lastArrivalTicks = 0;
}

void TransportWideFeedbackField::SetBase(WORD seq, QWORD arrivalUs)
{
	baseSeq = seq;
	//Le temps de reference est tronque au pas de 64 ms ; le premier delta
	//porte le reste. Modulo la periode du compteur 24 bits.
	referenceTicks = (DWORD)((arrivalUs % TimeWrapUs) / BaseTickUs);
	lastArrivalTicks = (QWORD)referenceTicks << 8;
	packets.clear();
}

bool TransportWideFeedbackField::AddReceived(WORD seq, QWORD arrivalUs)
{
	//Combler les trous depuis le dernier seq couvert
	WORD next = packets.empty() ? baseSeq : (WORD)(packets.back().seq + 1);
	//Distance en avant (arithmetique 16 bits)
	WORD gap = (WORD)(seq - next);
	//Un retour en arriere ou un saut demesure n'est pas encodable
	if (gap > 0x7FFF)
		return false;
	QWORD arrivalTicks = (arrivalUs % TimeWrapUs) / TickUs;
	long long delta = (long long)arrivalTicks - (long long)lastArrivalTicks;
	if (delta < -32768 || delta > 32767)
		return false;
	for (WORD i = 0; i < gap; ++i)
		packets.push_back({ (WORD)(next + i), false, 0 });
	packets.push_back({ seq, true, (int)delta });
	lastArrivalTicks = arrivalTicks;
	return true;
}

bool TransportWideFeedbackField::Encode(std::vector<WORD>& chunks, std::vector<BYTE>& deltas) const
{
	//Symbole par paquet
	std::vector<BYTE> symbols;
	symbols.reserve(packets.size());
	for (const PacketStatus& p : packets)
	{
		if (!p.received)
			symbols.push_back(SymbolNotReceived);
		else if (p.deltaTicks >= 0 && p.deltaTicks <= 0xFF)
			symbols.push_back(SymbolSmall);
		else if (p.deltaTicks >= -32768 && p.deltaTicks <= 32767)
			symbols.push_back(SymbolLarge);
		else
			return false;
	}
	//Chunks : plage (run-length) au-dela de 7 symboles identiques, sinon
	//vecteur de 7 symboles a 2 bits. Moins compact que l'encodeur temoin
	//(pas de vecteurs 1 bit) mais toujours conforme au decodage.
	size_t pos = 0;
	while (pos < symbols.size())
	{
		size_t run = 1;
		while (pos + run < symbols.size() && symbols[pos + run] == symbols[pos] && run < MaxRunLength)
			run++;
		if (run > 7 || pos + run == symbols.size())
		{
			//Run-length : bit 15 = 0, symbole sur 2 bits, longueur sur 13
			chunks.push_back((WORD)((symbols[pos] << 13) | run));
			pos += run;
		} else {
			//Vecteur 2 bits : bits 15-14 = 11, puis 7 symboles ; les places
			//au-dela de la fin sont remplies de 0, le compte fait foi au decodage
			WORD chunk = 0xC000;
			for (int i = 0; i < 7; ++i)
			{
				BYTE s = (pos + i < symbols.size()) ? symbols[pos + i] : (BYTE)SymbolNotReceived;
				chunk |= s << (12 - 2 * i);
			}
			chunks.push_back(chunk);
			pos += 7;
		}
	}
	//Deltas, dans l'ordre des paquets recus
	for (const PacketStatus& p : packets)
	{
		if (!p.received)
			continue;
		if (p.deltaTicks >= 0 && p.deltaTicks <= 0xFF)
			deltas.push_back((BYTE)p.deltaTicks);
		else
		{
			deltas.push_back((BYTE)(((WORD)p.deltaTicks) >> 8));
			deltas.push_back((BYTE)(((WORD)p.deltaTicks) & 0xFF));
		}
	}
	return true;
}

DWORD TransportWideFeedbackField::GetSize()
{
	std::vector<WORD> chunks;
	std::vector<BYTE> deltas;
	if (packets.empty() || !Encode(chunks, deltas))
		return 0;
	DWORD size = HeaderBytes + 2 * chunks.size() + deltas.size();
	//L'en-tete RTCP + les deux SSRC font 12 octets : le champ doit etre
	//multiple de 4 pour que le paquet tombe sur un mot
	return (size + 3) & ~3;
}

DWORD TransportWideFeedbackField::Serialize(BYTE* data, DWORD size)
{
	std::vector<WORD> chunks;
	std::vector<BYTE> deltas;
	if (packets.empty() || !Encode(chunks, deltas))
		return 0;
	DWORD needed = (DWORD)(HeaderBytes + 2 * chunks.size() + deltas.size());
	DWORD padded = (needed + 3) & ~3;
	if (size < padded)
		return 0;
	set2(data, 0, baseSeq);
	set2(data, 2, (WORD)packets.size());
	set3(data, 4, referenceTicks & 0x00FFFFFF);
	data[7] = fbSeq;
	DWORD len = HeaderBytes;
	for (WORD chunk : chunks)
	{
		set2(data, len, chunk);
		len += 2;
	}
	for (BYTE b : deltas)
		data[len++] = b;
	while (len < padded)
		data[len++] = 0;
	return len;
}

DWORD TransportWideFeedbackField::Parse(BYTE* data, DWORD size)
{
	if (size < HeaderBytes + 2)
		return 0;
	baseSeq = get2(data, 0);
	WORD statusCount = get2(data, 2);
	referenceTicks = get3(data, 4);
	fbSeq = data[7];
	//Un rapport vide n'est pas permis (temoin transport_feedback.cc)
	if (!statusCount)
		return 0;
	packets.clear();
	packets.reserve(statusCount);
	hasTimestamps = true;

	//Chunks -> un symbole (= taille de delta) par paquet
	std::vector<BYTE> symbols;
	symbols.reserve(statusCount);
	DWORD len = HeaderBytes;
	while (symbols.size() < statusCount)
	{
		if (len + 2 > size)
			return 0;
		WORD chunk = get2(data, len);
		len += 2;
		DWORD remaining = statusCount - symbols.size();
		if (!(chunk & 0x8000))
		{
			//Run-length
			BYTE symbol = (chunk >> 13) & 0x03;
			DWORD run = chunk & MaxRunLength;
			if (run > remaining)
				run = remaining;
			symbols.insert(symbols.end(), run, symbol);
		} else if (!(chunk & 0x4000)) {
			//Vecteur 1 bit : 14 symboles recu-petit-delta / perdu
			for (int i = 13; i >= 0 && symbols.size() < statusCount; --i)
				symbols.push_back((chunk >> i) & 0x01 ? SymbolSmall : SymbolNotReceived);
		} else {
			//Vecteur 2 bits : 7 symboles
			for (int i = 12; i >= 0 && symbols.size() < statusCount; i -= 2)
				symbols.push_back((chunk >> i) & 0x03);
		}
	}

	//Total d'octets de delta annonces par les symboles
	DWORD deltaBytes = 0;
	for (BYTE s : symbols)
		deltaBytes += (s == SymbolSmall) ? 1 : (s == SymbolLarge) ? 2 : (s == SymbolReserved) ? 3 : 0;

	if (len + deltaBytes <= size)
	{
		//Rapport avec deltas
		WORD seq = baseSeq;
		for (BYTE s : symbols)
		{
			switch (s)
			{
				case SymbolNotReceived:
					packets.push_back({ seq, false, 0 });
					break;
				case SymbolSmall:
					packets.push_back({ seq, true, (int)data[len] });
					len += 1;
					break;
				case SymbolLarge:
					packets.push_back({ seq, true, (short)get2(data, len) });
					len += 2;
					break;
				default:
					//Symbole reserve : rapport invalide (temoin, delta_size 3)
					packets.clear();
					return 0;
			}
			seq++;
		}
	} else {
		//Rapport sans deltas : les symboles disent seulement recu/perdu
		hasTimestamps = false;
		WORD seq = baseSeq;
		for (BYTE s : symbols)
		{
			packets.push_back({ seq, s != SymbolNotReceived, 0 });
			seq++;
		}
	}
	//Consommer le bourrage de fin : le champ occupe tout le paquet
	return size;
}

TransportWideFeedbackGenerator::TransportWideFeedbackGenerator()
{
	lastSeq = 0;
	hasLastSeq = false;
	windowStart = 0;
	hasWindowStart = false;
	lastSendUs = 0;
	intervalUs = DefaultIntervalUs;
	mediaSSRC = 0;
	fbSeq = 0;
}

void TransportWideFeedbackGenerator::OnPacketReceived(DWORD ssrc, WORD seq, QWORD nowUs)
{
	mediaSSRC = ssrc;
	QWORD unwrapped = UnwrapSeq16(seq, lastSeq, hasLastSeq);
	if (!hasLastSeq || unwrapped > lastSeq)
	{
		lastSeq = unwrapped;
		hasLastSeq = true;
	}
	//Premiere arrivee gagnante : un doublon ne redate pas le paquet
	if (arrivals.find(unwrapped) == arrivals.end())
		arrivals[unwrapped] = nowUs;
	//Un paquet reordonne rouvre la fenetre : le prochain rapport repart de lui,
	//quitte a redire des statuts deja envoyes (le pair ignore les doublons)
	if (!hasWindowStart || unwrapped < windowStart)
	{
		windowStart = unwrapped;
		hasWindowStart = true;
	}
	//Purge par la duree : au-dela de la fenetre, un reordonnancement n'est plus
	//rapportable et l'arrivee ne sert plus a rien
	while (!arrivals.empty() && arrivals.begin()->second + BackWindowUs < nowUs)
	{
		QWORD old = arrivals.begin()->first;
		arrivals.erase(arrivals.begin());
		if (hasWindowStart && windowStart <= old)
			windowStart = old + 1;
	}
}

DWORD TransportWideFeedbackGenerator::GetPendingCount() const
{
	if (!hasWindowStart)
		return 0;
	return (DWORD)std::distance(arrivals.lower_bound(windowStart), arrivals.end());
}

bool TransportWideFeedbackGenerator::HasPending() const
{
	return hasWindowStart && arrivals.lower_bound(windowStart) != arrivals.end();
}

bool TransportWideFeedbackGenerator::ShouldSend(QWORD nowUs) const
{
	return HasPending() && nowUs >= lastSendUs + intervalUs;
}

bool TransportWideFeedbackGenerator::BuildFeedback(TransportWideFeedbackField& field, QWORD nowUs)
{
	if (!hasWindowStart)
		return false;
	std::map<QWORD,QWORD>::const_iterator it = arrivals.lower_bound(windowStart);
	if (it == arrivals.end())
		return false;

	const QWORD base = it->first;
	field.SetBase((WORD)base, it->second);
	field.fbSeq = fbSeq++;

	QWORD covered = base;
	bool any = false;
	for (; it != arrivals.end(); ++it)
	{
		//Ce que ce paquet ajoute au rapport, trous compris : c'est la distance
		//au premier seq couvert qui borne, pas le nombre de paquets recus
		if (it->first - base + 1 > MaxStatusPerReport)
			break;
		//Un delta qui ne tient pas dans le format arrete le rapport ici
		if (!field.AddReceived((WORD)it->first, it->second))
			break;
		covered = it->first;
		any = true;
	}

	//Le premier paquet lui-meme refuse : il n'est pas rapportable, on l'ecarte
	//plutot que de bloquer la fenetre sur lui a chaque tour
	if (!any)
	{
		windowStart = base + 1;
		return false;
	}

	windowStart = covered + 1;
	lastSendUs = nowUs;
	return true;
}

void TransportWideFeedbackGenerator::SetSendBitrate(DWORD bitrate)
{
	//Les rapports occupent au plus 5 % du lien (temoin :143-165) : sous le
	//debit ou meme un rapport par MaxIntervalUs depasserait, on s'en tient au
	//rapport le plus espace.
	QWORD twccBps = (QWORD)bitrate / 20;
	if (!twccBps)
	{
		intervalUs = MaxIntervalUs;
		return;
	}
	QWORD interval = ((QWORD)ReportBytes * 8 * 1000000) / twccBps;
	if (interval < MinIntervalUs)
		interval = MinIntervalUs;
	if (interval > MaxIntervalUs)
		interval = MaxIntervalUs;
	intervalUs = interval;
}
