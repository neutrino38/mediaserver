/**
 * test_rtp_mid.cpp — l'extension d'en-tête MID (RFC 8843 §15, RFC 8285 §4.2),
 * lot 0 du chantier docs/conception/UNIFIED-PLAN/SPEC.md.
 *
 * Trois choses à prouver :
 *  - un paquet forgé portant un MID one-byte est lu, et l'en-tête two-byte
 *    n'est pas pris pour du one-byte (SPEC §5) ;
 *  - une session lit les extensions de TOUT paquet reçu, transport-cc négocié
 *    ou non : sans cela le MID d'une jambe jointe serait invisible ;
 *  - un paquet émis porte le MID annoncé par les propriétés `mid` et
 *    `urn:ietf:params:rtp-hdrext:sdes:mid`.
 *
 *     ./tests/runtests --gtest_filter='RtpMid*'
 */
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "rtp.h"
#include "rtpsession.h"

namespace {

const char* const kMidUri = "urn:ietf:params:rtp-hdrext:sdes:mid";
const BYTE  kMidExtId    = 4;
const BYTE  kPayloadType = 0;	// PCMU
const DWORD kProbeSSRC   = 0x1234ABCD;

// RTP minimal, X paramétrable, une extension dont le profil et les octets
// d'élément sont donnés tels quels (padding à 4 octets inclus par l'appelant).
std::vector<BYTE> ForgeRtp(WORD profile, const std::vector<BYTE>& element)
{
	std::vector<BYTE> pkt;
	pkt.push_back(element.empty() ? 0x80 : 0x90);
	pkt.push_back(kPayloadType);
	pkt.push_back(0x00); pkt.push_back(0x01);				// seq
	pkt.push_back(0x00); pkt.push_back(0x00); pkt.push_back(0x00); pkt.push_back(0x64);	// ts
	pkt.push_back(0x12); pkt.push_back(0x34); pkt.push_back(0xAB); pkt.push_back(0xCD);	// ssrc
	if (!element.empty())
	{
		pkt.push_back(profile >> 8); pkt.push_back(profile & 0xFF);
		const WORD words = element.size() / 4;
		pkt.push_back(words >> 8); pkt.push_back(words & 0xFF);
		pkt.insert(pkt.end(), element.begin(), element.end());
	}
	for (int i = 0; i < 4; ++i)
		pkt.push_back(0xAA);						// média
	return pkt;
}

// Élément one-byte : id, len-1, puis la valeur, bourré à 4 octets.
std::vector<BYTE> OneByteMid(BYTE id, const std::string& mid)
{
	std::vector<BYTE> e;
	e.push_back((BYTE)(id << 4 | (mid.size() - 1)));
	e.insert(e.end(), mid.begin(), mid.end());
	while (e.size() & 0x03)
		e.push_back(0);
	return e;
}

RTPMap MidMap()
{
	RTPMap map;
	map[kMidExtId] = RTPPacket::HeaderExtension::MID;
	return map;
}

class StubListener : public RTPSession::Listener
{
public:
	void onFPURequested(RTPSession*) override {}
	void onReceiverEstimatedMaxBitrate(RTPSession*, DWORD) override {}
	void onTempMaxMediaStreamBitrateRequest(RTPSession*, DWORD, DWORD) override {}
};

// Socket UDP en loopback jouant le pair.
class Probe
{
public:
	bool Open()
	{
		fd = socket(PF_INET, SOCK_DGRAM, 0);
		if (fd < 0)
			return false;
		sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin_family      = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (bind(fd, (sockaddr*)&addr, sizeof(addr)) != 0)
			return false;
		socklen_t len = sizeof(bound);
		return getsockname(fd, (sockaddr*)&bound, &len) == 0;
	}

	~Probe() { if (fd >= 0) close(fd); }

	int Port() const { return ntohs(bound.sin_port); }

	bool Send(int port, const std::vector<BYTE>& pkt)
	{
		sockaddr_in to;
		memset(&to, 0, sizeof(to));
		to.sin_family      = AF_INET;
		to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		to.sin_port        = htons(port);
		return sendto(fd, pkt.data(), pkt.size(), 0, (sockaddr*)&to, sizeof(to)) == (ssize_t)pkt.size();
	}

	// Attend un RTP à extension, le relit comme le ferait un pair, et rend
	// son MID ou une chaîne vide. Les amorçages NAT sont des en-têtes nus.
	std::string WaitForMid(int timeoutMs)
	{
		while (timeoutMs > 0)
		{
			pollfd pfd = { fd, POLLIN, 0 };
			const int slice = timeoutMs < 50 ? timeoutMs : 50;
			timeoutMs -= slice;
			if (poll(&pfd, 1, slice) <= 0)
				continue;
			BYTE buffer[MTU];
			const ssize_t size = recv(fd, buffer, sizeof(buffer), 0);
			if (size < 17 || RTCPCompoundPacket::IsRTCP(buffer, size))
				continue;
			RTPTimedPacket packet(MediaFrame::Audio, buffer, size);
			if (!packet.IsValid() || !packet.GetX())
				continue;
			packet.ProcessExtensions(MidMap());
			return packet.HasMid() ? packet.GetMid() : "";
		}
		return "";
	}

private:
	int         fd = -1;
	sockaddr_in bound {};
};

class Session
{
public:
	explicit Session(const char* mid)
		: session(MediaFrame::Audio, &listener)
	{
		ok = (session.Init() == 1);
		if (!ok)
			return;
		RTPMap map;
		map[kPayloadType] = 0;
		session.SetSendingRTPMap(map);
		session.SetReceivingRTPMap(map);
		Properties props;
		props["rtcp-mux"] = "1";
		props[kMidUri]    = "4";
		if (mid)
			props["mid"] = mid;
		session.SetProperties(props);
	}

	~Session() { if (ok) session.End(); }

	void SendMedia()
	{
		DWORD codec = 0;
		DWORD type  = kPayloadType;
		RTPPacket packet(MediaFrame::Audio, codec, type);
		memset(packet.GetMediaData(), 0xAA, 4);
		packet.SetMediaLength(4);
		session.SendPacket(packet);
	}

	bool         ok = false;
	StubListener listener;
	RTPSession   session;
};

#define REQUIRE_LOOPBACK(probe, sess)                                          \
	do {                                                                   \
		if (!(probe).Open())                                           \
			GTEST_SKIP() << "socket loopback indisponible";        \
		if (!(sess).ok)                                                \
			GTEST_SKIP() << "impossible de binder une paire de ports RTP"; \
	} while (0)

} // namespace

// ---------------------------------------------------------------------------
// Le paquet
// ---------------------------------------------------------------------------

TEST(RtpMid, UnMidOneByteEstLu)
{
	std::vector<BYTE> pkt = ForgeRtp(0xBEDE, OneByteMid(kMidExtId, "v0"));
	RTPTimedPacket packet(MediaFrame::Video, pkt.data(), pkt.size());
	ASSERT_TRUE(packet.IsValid());
	packet.ProcessExtensions(MidMap());
	ASSERT_TRUE(packet.HasMid());
	EXPECT_STREQ("v0", packet.GetMid());
	EXPECT_EQ(4u, packet.GetMediaLength()) << "le media suit l'extension, intact";
}

TEST(RtpMid, UnMidDeSeizeOctetsEstLuEntier)
{
	const std::string mid = "0123456789abcdef";
	std::vector<BYTE> pkt = ForgeRtp(0xBEDE, OneByteMid(kMidExtId, mid));
	RTPTimedPacket packet(MediaFrame::Video, pkt.data(), pkt.size());
	ASSERT_TRUE(packet.IsValid());
	packet.ProcessExtensions(MidMap());
	ASSERT_TRUE(packet.HasMid());
	EXPECT_EQ(mid, packet.GetMid());
}

// Les mêmes octets sous le profil two-byte (RFC 8285 §4.3, 0x100x) : lus en
// one-byte, ils donneraient id 4, longueur 2, valeur "v0". Ils ne le sont pas.
TEST(RtpMid, UnEnTeteTwoByteNEstPasPrisPourDuOneByte)
{
	std::vector<BYTE> pkt = ForgeRtp(0x1000, OneByteMid(kMidExtId, "v0"));
	RTPTimedPacket packet(MediaFrame::Video, pkt.data(), pkt.size());
	ASSERT_TRUE(packet.IsValid());
	packet.ProcessExtensions(MidMap());
	EXPECT_FALSE(packet.HasMid());
	EXPECT_EQ(4u, packet.GetMediaLength());
}

TEST(RtpMid, UnIdNonNegocieNeDonnePasDeMid)
{
	std::vector<BYTE> pkt = ForgeRtp(0xBEDE, OneByteMid(kMidExtId + 1, "v0"));
	RTPTimedPacket packet(MediaFrame::Video, pkt.data(), pkt.size());
	ASSERT_TRUE(packet.IsValid());
	packet.ProcessExtensions(MidMap());
	EXPECT_FALSE(packet.HasMid());
}

// ---------------------------------------------------------------------------
// La session
// ---------------------------------------------------------------------------

// Sans transport-cc négocié, le MID d'un paquet reçu doit quand même être lu :
// le paquet rendu au consommateur est celui que le réacteur a analysé.
TEST(RtpMid, LaSessionLitLeMidSansTransportCC)
{
	Probe probe;
	Session sess(NULL);
	REQUIRE_LOOPBACK(probe, sess);

	char ip[] = "127.0.0.1";
	ASSERT_EQ(1, sess.session.SetRemotePort(ip, probe.Port()));
	ASSERT_TRUE(probe.Send(sess.session.GetLocalPort(), ForgeRtp(0xBEDE, OneByteMid(kMidExtId, "v1"))));

	RTPPacket* packet = NULL;
	for (int i = 0; i < 20 && !packet; ++i)
		packet = sess.session.GetPacket(kProbeSSRC, 100);
	ASSERT_TRUE(packet != NULL) << "le paquet n'est jamais arrive au consommateur";
	EXPECT_TRUE(packet->HasMid()) << "extension non lue : ProcessExtensions n'a pas tourne";
	EXPECT_STREQ("v1", packet->GetMid());
	delete packet;
}

TEST(RtpMid, NosPaquetsPortentLeMidAnnonce)
{
	Probe probe;
	Session sess("v2");
	REQUIRE_LOOPBACK(probe, sess);

	char ip[] = "127.0.0.1";
	ASSERT_EQ(1, sess.session.SetRemotePort(ip, probe.Port()));

	for (int i = 0; i < 10; ++i)
	{
		sess.SendMedia();
		const std::string mid = probe.WaitForMid(100);
		if (!mid.empty())
		{
			EXPECT_EQ("v2", mid);
			return;
		}
	}
	FAIL() << "aucun paquet sortant ne porte le MID";
}

// Sans propriété `mid`, l'id négocié seul n'écrit rien : il n'y a rien à dire.
TEST(RtpMid, SansMidAucuneExtensionEcrite)
{
	Probe probe;
	Session sess(NULL);
	REQUIRE_LOOPBACK(probe, sess);

	char ip[] = "127.0.0.1";
	ASSERT_EQ(1, sess.session.SetRemotePort(ip, probe.Port()));

	for (int i = 0; i < 5; ++i)
	{
		sess.SendMedia();
		EXPECT_EQ("", probe.WaitForMid(100));
	}
}
