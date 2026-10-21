/**
 * test_amr_rtp.cpp — l'AMR-WB au bord du fil (RFC 4867).
 *
 * Les codecs AMR lisent et écrivent le format stockage ; le fil porte un CMR et une
 * table des matières, en octet-aligned ou en bandwidth-efficient selon le fmtp
 * négocié pour le PT. `RTPSession` traduit dans les deux sens. Ces tests jouent une
 * vraie session en loopback face à un socket UDP brut : ce qui est vérifié, ce sont
 * les octets sur le fil et ceux que reçoit le consommateur.
 */
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <set>
#include <vector>

#include "log.h"
#include "medkit/codecs.h"
#include "rtp.h"
#include "rtpsession.h"

namespace {

const BYTE  kPt   = 96;
const DWORD kSsrc = 0x1234ABCD;

// Mode 0 de l'AMR-WB (132 bits), tout à 1, Q=1 : au format stockage, puis tel
// que chacun des deux modes le met sur le fil (calcul détaillé dans
// libmedikit tests/test_amr_payload.cpp).
std::vector<BYTE> Storage()
{
	std::vector<BYTE> v(1, 0x04);
	v.insert(v.end(), 16, 0xFF);
	v.push_back(0xF0);
	return v;
}

std::vector<BYTE> BandwidthEfficient()
{
	std::vector<BYTE> v = { 0xF0, 0x7F };
	v.insert(v.end(), 15, 0xFF);
	v.push_back(0xFC);
	return v;
}

std::vector<BYTE> OctetAligned()
{
	std::vector<BYTE> v = { 0xF0, 0x04 };
	v.insert(v.end(), 16, 0xFF);
	v.push_back(0xF0);
	return v;
}

std::vector<BYTE> ForgeRtp(WORD seq, const std::vector<BYTE>& payload)
{
	std::vector<BYTE> pkt = { 0x80, kPt, (BYTE)(seq >> 8), (BYTE)seq,
	                          0x00, 0x00, 0x01, 0x40,
	                          0x12, 0x34, 0xAB, 0xCD };
	pkt.insert(pkt.end(), payload.begin(), payload.end());
	return pkt;
}

class StubListener : public RTPSession::Listener
{
public:
	void onFPURequested(RTPSession*) override {}
	void onReceiverEstimatedMaxBitrate(RTPSession*, DWORD) override {}
	void onTempMaxMediaStreamBitrateRequest(RTPSession*, DWORD, DWORD) override {}
};

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

	// La charge utile du premier paquet RTP reçu, ou un vecteur vide.
	std::vector<BYTE> WaitForPayload(int timeoutMs)
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
			if (size < 12 || RTCPCompoundPacket::IsRTCP(buffer, size))
				continue;
			RTPTimedPacket packet(MediaFrame::Audio, buffer, size);
			if (!packet.IsValid() || packet.GetMediaLength() == 0)
				continue;
			return std::vector<BYTE>(packet.GetMediaData(), packet.GetMediaData() + packet.GetMediaLength());
		}
		return std::vector<BYTE>();
	}

private:
	int         fd = -1;
	sockaddr_in bound {};
};

class Session
{
public:
	explicit Session(bool octetAligned)
		: session(MediaFrame::Audio, &listener)
	{
		ok = (session.Init() == 1);
		if (!ok)
			return;
		RTPMap map;
		map[kPt] = AudioCodec::AMRWB;
		session.SetSendingRTPMap(map);
		session.SetReceivingRTPMap(map);
		if (octetAligned)
			session.SetAMROctetAligned(std::set<BYTE>{ kPt });
	}

	~Session() { if (ok) session.End(); }

	void SendStorage(const std::vector<BYTE>& storage)
	{
		DWORD codec = AudioCodec::AMRWB;
		DWORD type  = kPt;
		RTPPacket packet(MediaFrame::Audio, codec, type);
		memcpy(packet.GetMediaData(), storage.data(), storage.size());
		packet.SetMediaLength(storage.size());
		session.SendPacket(packet);
	}

	std::vector<BYTE> WaitForStorage(int tries)
	{
		for (int i = 0; i < tries; ++i)
		{
			RTPPacket* packet = session.GetPacket(kSsrc, 100);
			if (!packet)
				continue;
			std::vector<BYTE> out(packet->GetMediaData(), packet->GetMediaData() + packet->GetMediaLength());
			delete packet;
			return out;
		}
		return std::vector<BYTE>();
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

// Sans octet-align négocié : le défaut de la RFC, bandwidth-efficient.
TEST(AmrRtp, EmissionBandwidthEfficientParDefaut)
{
	Probe probe;
	Session sess(false);
	REQUIRE_LOOPBACK(probe, sess);
	char ip[] = "127.0.0.1";
	ASSERT_EQ(1, sess.session.SetRemotePort(ip, probe.Port()));

	sess.SendStorage(Storage());
	EXPECT_EQ(probe.WaitForPayload(1000), BandwidthEfficient());
}

TEST(AmrRtp, EmissionOctetAligned)
{
	Probe probe;
	Session sess(true);
	REQUIRE_LOOPBACK(probe, sess);
	char ip[] = "127.0.0.1";
	ASSERT_EQ(1, sess.session.SetRemotePort(ip, probe.Port()));

	sess.SendStorage(Storage());
	EXPECT_EQ(probe.WaitForPayload(1000), OctetAligned());
}

TEST(AmrRtp, ReceptionBandwidthEfficient)
{
	Probe probe;
	Session sess(false);
	REQUIRE_LOOPBACK(probe, sess);
	char ip[] = "127.0.0.1";
	ASSERT_EQ(1, sess.session.SetRemotePort(ip, probe.Port()));

	ASSERT_TRUE(probe.Send(sess.session.GetLocalPort(), ForgeRtp(1, BandwidthEfficient())));
	EXPECT_EQ(sess.WaitForStorage(20), Storage());
}

TEST(AmrRtp, ReceptionOctetAligned)
{
	Probe probe;
	Session sess(true);
	REQUIRE_LOOPBACK(probe, sess);
	char ip[] = "127.0.0.1";
	ASSERT_EQ(1, sess.session.SetRemotePort(ip, probe.Port()));

	ASSERT_TRUE(probe.Send(sess.session.GetLocalPort(), ForgeRtp(1, OctetAligned())));
	EXPECT_EQ(sess.WaitForStorage(20), Storage());
}

// Une charge illisible n'atteint pas le décodeur ; la suivante, si.
TEST(AmrRtp, UneChargeInvalideEstJetee)
{
	Probe probe;
	Session sess(true);
	REQUIRE_LOOPBACK(probe, sess);
	char ip[] = "127.0.0.1";
	ASSERT_EQ(1, sess.session.SetRemotePort(ip, probe.Port()));

	ASSERT_TRUE(probe.Send(sess.session.GetLocalPort(), ForgeRtp(1, { 0xF0, 0x54, 0x00 })));
	ASSERT_TRUE(probe.Send(sess.session.GetLocalPort(), ForgeRtp(2, OctetAligned())));
	EXPECT_EQ(sess.WaitForStorage(20), Storage());
}
