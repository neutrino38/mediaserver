/**
 * test_rtp_rtcp.cpp — parsing RTP/RTCP sur capture réelle (rtp.h).
 *
 * Utilise une FIXTURE pcap versionnée (`tests/fixtures/rtp_rtcp.pcap`, extraite
 * de la capture `record.pcap` du dépôt en ne gardant que le trafic RTP/RTCP) et
 * rejoue chaque datagramme UDP à travers les parseurs du mcu :
 *   - `RTPPacket(media, data, size)` (décodage d'en-tête RTP) ;
 *   - `RTCPCompoundPacket::IsRTCP` / `::Parse` (paquets composés RTCP).
 *
 * On vérifie, sur des paquets réels : version RTP = 2, cohérence par SSRC
 * (plusieurs paquets, numéros de séquence qui progressent), et côté RTCP la
 * présence de rapports (SenderReport/ReceiverReport) correctement décodés.
 *
 * Le mediaserver n'avait aucun test de sa pile RTP/RTCP ; c'en est le premier
 * filet, adossé à du trafic capturé plutôt qu'à des trames synthétiques.
 *
 * Le chemin de la fixture est injecté via -DTEST_PCAP_FILE (surchargeable :
 * make check TEST_PCAP=/chemin). Absente, le test est SKIPPÉ.
 */
#include <gtest/gtest.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

#include "rtp.h"

#ifndef TEST_PCAP_FILE
#define TEST_PCAP_FILE "tests/fixtures/rtp_rtcp.pcap"
#endif

namespace {

// --- Lecteur pcap minimal (format classique, link-type Ethernet) -------------
class PcapReader
{
public:
	bool Open(const char* path)
	{
		f = fopen(path, "rb");
		if (!f)
			return false;
		BYTE gh[24];
		if (fread(gh, 1, 24, f) != 24)
			return false;
		uint32_t magic;
		memcpy(&magic, gh, 4);
		if (magic == 0xa1b2c3d4)
			swap = false;
		else if (magic == 0xd4c3b2a1)
			swap = true;
		else
			return false; // pas un pcap classique (pcapng ?)
		linktype = Read32(gh + 20);
		return true;
	}

	~PcapReader() { if (f) fclose(f); }

	uint32_t LinkType() const { return linktype; }

	// Lit le prochain paquet dans `out`. false en fin de fichier.
	bool Next(std::vector<BYTE>& out)
	{
		BYTE rh[16];
		if (fread(rh, 1, 16, f) != 16)
			return false;
		uint32_t inclLen = Read32(rh + 8);
		if (inclLen == 0 || inclLen > 262144)
			return false;
		out.resize(inclLen);
		return fread(out.data(), 1, inclLen, f) == inclLen;
	}

private:
	uint32_t Read32(const BYTE* p) const
	{
		uint32_t v;
		memcpy(&v, p, 4);
		if (swap)
			v = __builtin_bswap32(v);
		return v;
	}

	FILE* f = nullptr;
	bool swap = false;
	uint32_t linktype = 0;
};

// Extrait le payload UDP d'une trame Ethernet/IPv4. false si ce n'est pas de l'UDP.
bool ExtractUdpPayload(const BYTE* frame, DWORD len, const BYTE*& payload, DWORD& plen)
{
	if (len < 14)
		return false;
	DWORD off = 12;
	WORD ethertype = (frame[off] << 8) | frame[off + 1];
	off += 2;
	// VLAN 802.1Q : saute la balise (jusqu'à deux niveaux).
	for (int i = 0; i < 2 && ethertype == 0x8100; ++i)
	{
		if (len < off + 4)
			return false;
		ethertype = (frame[off + 2] << 8) | frame[off + 3];
		off += 4;
	}
	if (ethertype != 0x0800) // IPv4 seulement
		return false;
	if (len < off + 20)
		return false;
	const BYTE* ip = frame + off;
	if ((ip[0] >> 4) != 4)
		return false;
	DWORD ihl = (ip[0] & 0x0F) * 4;
	if (ihl < 20 || len < off + ihl + 8)
		return false;
	if (ip[9] != 17) // UDP
		return false;
	const BYTE* udp = ip + ihl;
	DWORD udpLen = (udp[4] << 8) | udp[5];
	if (udpLen < 8)
		return false;
	payload = udp + 8;
	DWORD declared = udpLen - 8;
	DWORD remaining = len - (off + ihl + 8);
	plen = declared < remaining ? declared : remaining; // tronqué à la capture
	return plen > 0;
}

} // namespace

TEST(RtpRtcp, ParseRealCapture)
{
	PcapReader pcap;
	if (!pcap.Open(TEST_PCAP_FILE))
		GTEST_SKIP() << "Fixture pcap absente/illisible : " << TEST_PCAP_FILE;
	ASSERT_EQ(pcap.LinkType(), 1u) << "link-type attendu : Ethernet (1)";

	unsigned rtpCount = 0, rtcpCount = 0, otherCount = 0;
	unsigned srCount = 0, rrCount = 0, rtcpSubPackets = 0;
	std::map<DWORD, std::pair<DWORD, DWORD>> seqBySsrc; // ssrc -> (seqMin, seqMax)
	std::map<DWORD, unsigned> pktBySsrc;

	std::vector<BYTE> frame;
	while (pcap.Next(frame))
	{
		const BYTE* payload = nullptr;
		DWORD plen = 0;
		if (!ExtractUdpPayload(frame.data(), frame.size(), payload, plen))
			continue;

		// RTCP d'abord (types 200-206), sinon RTP (version 2), sinon bruit.
		if (RTCPCompoundPacket::IsRTCP((BYTE*)payload, plen))
		{
			RTCPCompoundPacket* rtcp = RTCPCompoundPacket::Parse((BYTE*)payload, plen);
			ASSERT_NE(rtcp, nullptr) << "IsRTCP vrai mais Parse a échoué";
			EXPECT_GT(rtcp->GetPacketCount(), 0u);
			for (DWORD i = 0; i < rtcp->GetPacketCount(); ++i)
			{
				RTCPPacket* p = rtcp->GetPacket(i);
				ASSERT_NE(p, nullptr);
				++rtcpSubPackets;
				if (p->GetType() == RTCPPacket::SenderReport)
					++srCount;
				else if (p->GetType() == RTCPPacket::ReceiverReport)
					++rrCount;
			}
			delete rtcp;
			++rtcpCount;
		}
		else if (plen >= 12 && (payload[0] >> 6) == 2 && plen <= 1700)
		{
			RTPPacket rtp(MediaFrame::Audio, (BYTE*)payload, plen);
			EXPECT_EQ(rtp.GetVersion(), 2);
			EXPECT_LT(rtp.GetType(), 128u); // payload type sur 7 bits
			DWORD ssrc = rtp.GetSSRC();
			WORD seq = rtp.GetSeqNum();
			auto it = seqBySsrc.find(ssrc);
			if (it == seqBySsrc.end())
				seqBySsrc[ssrc] = {seq, seq};
			else
			{
				if (seq < it->second.first) it->second.first = seq;
				if (seq > it->second.second) it->second.second = seq;
			}
			++pktBySsrc[ssrc];
			++rtpCount;
		}
		else
			++otherCount;
	}

	// --- Assertions globales sur la capture ---------------------------------
	printf("[rtp/rtcp] RTP=%u RTCP=%u (sous-paquets=%u, SR=%u, RR=%u) autres=%u SSRC=%zu\n",
	       rtpCount, rtcpCount, rtcpSubPackets, srCount, rrCount, otherCount, seqBySsrc.size());

	EXPECT_GT(rtpCount, 100u)  << "trop peu de paquets RTP décodés";
	EXPECT_GE(rtcpCount, 5u)   << "trop peu de paquets composés RTCP décodés";
	EXPECT_GT(srCount + rrCount, 0u) << "aucun rapport RTCP (SR/RR) décodé";
	ASSERT_FALSE(seqBySsrc.empty());

	// Le SSRC le plus actif doit couvrir une plage de séquence non triviale
	// (les paquets progressent, ce n'est pas une trame figée).
	DWORD busiest = 0; unsigned best = 0;
	for (auto& kv : pktBySsrc)
		if (kv.second > best) { best = kv.second; busiest = kv.first; }
	EXPECT_GT(best, 10u);
	auto& span = seqBySsrc[busiest];
	EXPECT_GT(span.second, span.first) << "les numéros de séquence RTP ne progressent pas";
}
