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

	// Enregistrement ORDONNÉ (par ordre d'arrivée) de chaque flux RTP, par SSRC :
	// on garde seq/timestamp/payload-type pour vérifier la cohérence d'un vrai flux.
	struct RtpRec { WORD seq; DWORD ts; BYTE pt; };
	std::map<DWORD, std::vector<RtpRec>> streamBySsrc;

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
			// Le filtre « version==2 » attrape quelques faux positifs (paquets non-RTP
			// dont le 1er octet commence par 0b10) : si l'en-tête calculé (CC/extension)
			// déborde du datagramme, ce n'est pas du RTP bien formé -> compté à part.
			if (rtp.GetRTPHeaderLen() > plen)
			{
				++otherCount;
				continue;
			}
			// Décodage d'en-tête RTP bien formé : contrôles par paquet.
			EXPECT_EQ(rtp.GetVersion(), 2);
			EXPECT_LT(rtp.GetType(), 128u);                  // payload type sur 7 bits
			EXPECT_GE(rtp.GetRTPHeaderLen(), 12u);           // en-tête RTP fixe minimal
			streamBySsrc[rtp.GetSSRC()].push_back(
				{rtp.GetSeqNum(), rtp.GetTimestamp(), (BYTE)rtp.GetType()});
			++rtpCount;
		}
		else
			++otherCount;
	}

	// --- Assertions globales sur la capture ---------------------------------
	printf("[rtp/rtcp] RTP=%u RTCP=%u (sous-paquets=%u, SR=%u, RR=%u) autres=%u SSRC=%zu\n",
	       rtpCount, rtcpCount, rtcpSubPackets, srCount, rrCount, otherCount, streamBySsrc.size());

	EXPECT_GT(rtpCount, 100u)  << "trop peu de paquets RTP décodés";
	EXPECT_GE(rtcpCount, 5u)   << "trop peu de paquets composés RTCP décodés";
	EXPECT_GT(srCount + rrCount, 0u) << "aucun rapport RTCP (SR/RR) décodé";
	ASSERT_FALSE(streamBySsrc.empty());

	// --- Cohérence du flux RTP le plus actif --------------------------------
	// On analyse le SSRC le plus fourni : un vrai flux RTP doit avoir un payload
	// type constant, des numéros de séquence qui avancent (avec gestion du
	// bouclage 16 bits) et des timestamps non décroissants dans la grande majorité.
	DWORD busiest = 0; size_t best = 0;
	for (auto& kv : streamBySsrc)
		if (kv.second.size() > best) { best = kv.second.size(); busiest = kv.first; }
	const std::vector<RtpRec>& s = streamBySsrc[busiest];
	ASSERT_GT(s.size(), 50u) << "flux RTP dominant trop court pour être analysé";

	// (a) payload type unique sur tout le flux
	for (const RtpRec& r : s)
		EXPECT_EQ(r.pt, s[0].pt) << "payload type incohérent dans le flux SSRC dominant";

	// (b) séquences en avant + timestamps non décroissants (majoritaires)
	unsigned seqForward = 0, tsNonDecr = 0;
	for (size_t i = 1; i < s.size(); ++i)
	{
		WORD d = (WORD)(s[i].seq - s[i - 1].seq); // arithmétique 16 bits (bouclage)
		if (d >= 1 && d < 1000)
			++seqForward;
		if ((DWORD)(s[i].ts - s[i - 1].ts) < 0x80000000u) // pas de recul (mod 2^32)
			++tsNonDecr;
	}
	size_t steps = s.size() - 1;
	EXPECT_GT(seqForward, steps * 9 / 10)
		<< "les numéros de séquence RTP ne progressent pas régulièrement";
	EXPECT_GT(tsNonDecr, steps * 9 / 10)
		<< "les timestamps RTP reculent trop souvent";
}

// ===========================================================================
// Tests RTP AUTONOMES (sans pcap) : round-trip de l'en-tête RTP dans l'esprit
// des autres suites (build -> GetData/GetSize -> re-parse). Ils exercent
// directement RTPPacket sans dépendre de la capture.
// ===========================================================================
TEST(Rtp, HeaderRoundTrip)
{
	BYTE media[64];
	for (int i = 0; i < 64; ++i) media[i] = (BYTE)(0x30 + i);

	DWORD codec = 0; // variable (pas une constante pointeur nul) -> lève l'ambiguïté
	RTPPacket in(MediaFrame::Audio, codec, (DWORD)96);
	in.SetSeqNum(0x1234);
	in.SetTimestamp(0xDEADBEEF);
	in.SetSSRC(0xCAFEBABE);
	in.SetMark(true);
	ASSERT_TRUE(in.SetPayload(media, sizeof(media)));

	// Sérialisation sur le fil = en-tête (12 o) + payload.
	EXPECT_EQ(in.GetRTPHeaderLen(), 12u);
	EXPECT_EQ(in.GetSize(), 12u + sizeof(media));

	// Re-parse depuis les octets bruts.
	RTPPacket out(MediaFrame::Audio, in.GetData(), in.GetSize());
	EXPECT_EQ(out.GetVersion(), 2);
	EXPECT_EQ(out.GetType(), 96u);
	EXPECT_EQ(out.GetSeqNum(), 0x1234);
	EXPECT_EQ(out.GetTimestamp(), 0xDEADBEEFu);
	EXPECT_EQ(out.GetSSRC(), 0xCAFEBABEu);
	EXPECT_TRUE(out.GetMark());
	ASSERT_EQ(out.GetMediaLength(), sizeof(media));
	EXPECT_EQ(0, memcmp(out.GetMediaData(), media, sizeof(media)));

	// Accès statiques sur le buffer brut (utilisés par le démultiplexage RTP).
	EXPECT_EQ(RTPPacket::GetType(in.GetData()), 96);
	EXPECT_EQ(RTPPacket::GetSSRC(in.GetData()), 0xCAFEBABEu);
}

TEST(Rtp, SeqAndTimestampWrap)
{
	// Valeurs limites : bouclage 16 bits de la séquence et 32 bits du timestamp.
	DWORD codec = 0;
	for (auto pt : {(DWORD)0, (DWORD)8, (DWORD)127})
	{
		RTPPacket in(MediaFrame::Video, codec, pt);
		in.SetSeqNum(0xFFFF);
		in.SetTimestamp(0xFFFFFFFF);
		in.SetMark(false);
		BYTE b = 0xAB;
		ASSERT_TRUE(in.SetPayload(&b, 1));

		RTPPacket out(MediaFrame::Video, in.GetData(), in.GetSize());
		EXPECT_EQ(out.GetType(), pt);
		EXPECT_EQ(out.GetSeqNum(), 0xFFFF);
		EXPECT_EQ(out.GetTimestamp(), 0xFFFFFFFFu);
		EXPECT_FALSE(out.GetMark());
		ASSERT_EQ(out.GetMediaLength(), 1u);
		EXPECT_EQ(out.GetMediaData()[0], 0xAB);
	}
}

// ===========================================================================
// Tests ADVERSES : paquets RTP/RTCP volontairement cassés. On vérifie que les
// parseurs ne crashent pas (utile sous ASAN) et exposent de quoi détecter la
// malformation. Les octets fautifs restent DANS le buffer fourni pour un
// comportement déterministe (pas de lecture au-delà des données passées).
// ===========================================================================

namespace {

// En-tête RTP minimal bien formé (12 octets) : V=2, P=0, X=0, CC=0.
std::vector<BYTE> MakeRtpHeader(BYTE pt, WORD seq, DWORD ts, DWORD ssrc)
{
	std::vector<BYTE> b(12, 0);
	b[0] = 0x80;                       // V=2
	b[1] = (BYTE)(pt & 0x7F);          // M=0, PT
	b[2] = (BYTE)(seq >> 8);  b[3] = (BYTE)seq;
	b[4] = (BYTE)(ts >> 24);  b[5] = (BYTE)(ts >> 16);
	b[6] = (BYTE)(ts >> 8);   b[7] = (BYTE)ts;
	b[8] = (BYTE)(ssrc >> 24); b[9]  = (BYTE)(ssrc >> 16);
	b[10] = (BYTE)(ssrc >> 8); b[11] = (BYTE)ssrc;
	return b;
}

} // namespace

// En-tête d'extension annonçant une taille démesurée : l'en-tête RTP calculé
// déborde du datagramme -> détectable (GetRTPHeaderLen > taille), sans crash.
TEST(RtpAdversarial, ExtensionLengthOverflow)
{
	std::vector<BYTE> b = MakeRtpHeader(96, 1, 2, 3);
	b[0] |= 0x10;                 // X=1
	b.push_back(0xBE); b.push_back(0xDE); // ext type
	b.push_back(0xFF); b.push_back(0xFF); // ext len = 0xFFFF mots (32 bits)

	RTPPacket rtp(MediaFrame::Audio, b.data(), b.size());
	EXPECT_EQ(rtp.GetVersion(), 2);
	EXPECT_TRUE(rtp.GetX());
	EXPECT_GT(rtp.GetRTPHeaderLen(), (DWORD)b.size())
		<< "en-tête d'extension surdimensionné non détecté";
}

// CSRC count = 15 dans un paquet trop court : l'en-tête calculé (12 + 60) dépasse
// le datagramme -> détectable, pas de crash (seul l'octet 0 est lu pour cc).
TEST(RtpAdversarial, CsrcCountOverflow)
{
	std::vector<BYTE> b = MakeRtpHeader(8, 1, 2, 3);
	b[0] = 0x8F;                 // V=2, CC=15

	RTPPacket rtp(MediaFrame::Audio, b.data(), b.size());
	EXPECT_EQ(rtp.GetCC(), 15);
	EXPECT_GT(rtp.GetRTPHeaderLen(), (DWORD)b.size());
}

// Mauvaise version : détectée par GetVersion (le classifieur du test capture
// s'appuie dessus pour écarter le paquet).
TEST(RtpAdversarial, WrongVersionDetectable)
{
	std::vector<BYTE> b = MakeRtpHeader(96, 1, 2, 3);
	b[0] = 0x40;                 // V=1

	RTPPacket rtp(MediaFrame::Audio, b.data(), b.size());
	EXPECT_NE(rtp.GetVersion(), 2);
}

// En-tête exact sans payload : cas limite bien formé (longueur média = 0).
TEST(RtpAdversarial, HeaderOnlyNoPayload)
{
	std::vector<BYTE> b = MakeRtpHeader(0, 100, 200, 300);
	RTPPacket rtp(MediaFrame::Audio, b.data(), b.size());
	EXPECT_EQ(rtp.GetVersion(), 2);
	EXPECT_EQ(rtp.GetRTPHeaderLen(), 12u);
	EXPECT_EQ(rtp.GetMediaLength(), 0u);
}

// --- RTCP ------------------------------------------------------------------

TEST(RtcpAdversarial, IsRtcpRejectsTooShort)
{
	BYTE b[3] = {0x80, 200, 0};  // < sizeof(rtcp_common_t) (4 octets)
	EXPECT_FALSE(RTCPCompoundPacket::IsRTCP(b, sizeof(b)));
}

TEST(RtcpAdversarial, IsRtcpRejectsWrongVersion)
{
	BYTE b[8] = {0};
	b[0] = 0x40; b[1] = 200;     // V=1, pt=SR
	EXPECT_FALSE(RTCPCompoundPacket::IsRTCP(b, sizeof(b)));
}

TEST(RtcpAdversarial, IsRtcpRejectsPtOutOfRange)
{
	BYTE b[8] = {0};
	b[0] = 0x80;
	b[1] = 100;  EXPECT_FALSE(RTCPCompoundPacket::IsRTCP(b, sizeof(b))); // < 200
	b[1] = 207;  EXPECT_FALSE(RTCPCompoundPacket::IsRTCP(b, sizeof(b))); // > 206
}

TEST(RtcpAdversarial, ParseNonRtcpReturnsNull)
{
	BYTE b[8] = {0};
	b[0] = 0x80; b[1] = 96;      // ressemble à du RTP, pas du RTCP
	EXPECT_EQ(RTCPCompoundPacket::Parse(b, sizeof(b)), nullptr);
}

// En-tête RTCP valide (V=2, pt=SR) mais champ length démesuré : Parse doit
// renvoyer NULL ("Wrong rtcp packet size") sans lire au-delà du buffer.
TEST(RtcpAdversarial, ParseRejectsLengthOverflow)
{
	BYTE b[8] = {0};
	b[0] = 0x80; b[1] = 200;     // V=2, SenderReport
	b[2] = 0xFF; b[3] = 0xFF;    // length = 0xFFFF mots -> ~256 Ko annoncés
	ASSERT_TRUE(RTCPCompoundPacket::IsRTCP(b, sizeof(b)));
	RTCPCompoundPacket* c = RTCPCompoundPacket::Parse(b, sizeof(b));
	EXPECT_EQ(c, nullptr) << "longueur RTCP débordante non rejetée";
	delete c; // no-op si nullptr
}
