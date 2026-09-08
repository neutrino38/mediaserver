/**
 * test_transport_cc_wiring.cpp — le branchement transport-cc de bout en bout
 * (docs/RATE-CONTROL.md).
 *
 * Les suites `TransportFeedbackWire` et `TransportFeedbackGenerator` éprouvent
 * le format et l'accumulateur en isolation, et elles passaient toutes alors que
 * la production n'émettait RIEN : la table des extensions négociées est écrite
 * dans un sens et relue dans l'autre. Aucun test de composant ne pouvait le
 * voir.
 *
 * Ce test-ci part donc d'où part le pair : un datagramme RTP portant
 * l'extension one-byte, envoyé à une vraie `RTPSession` par un socket sonde en
 * loopback. Ce qu'il vérifie est ce que le pair attend — un RTCP RTPFB fmt 15
 * en retour.
 */
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "log.h"
#include "rtp.h"
#include "rtpsession.h"
#include "transportfeedback.h"

namespace {

const char* const kTransportCCUri =
	"http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01";

const BYTE  kExtId       = 3;		// l'id que le pair a négocié
const BYTE  kPayloadType = 0;		// PCMU, comme le harnais de latching
const BYTE  kCodec       = 0;
const DWORD kProbeSSRC   = 0x1234ABCD;

class StubListener : public RTPSession::Listener
{
public:
	void onFPURequested(RTPSession*) override {}
	void onReceiverEstimatedMaxBitrate(RTPSession*, DWORD) override {}
	void onTempMaxMediaStreamBitrateRequest(RTPSession*, DWORD, DWORD) override {}
};

// Socket UDP en loopback jouant le pair : il émet du RTP porteur de l'extension
// et guette le rapport d'arrivée que la session lui doit.
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
		addr.sin_port        = 0;
		if (bind(fd, (sockaddr*)&addr, sizeof(addr)) != 0)
			return false;
		socklen_t len = sizeof(bound);
		return getsockname(fd, (sockaddr*)&bound, &len) == 0;
	}

	~Probe() { if (fd >= 0) close(fd); }

	int Port() const { return ntohs(bound.sin_port); }

	// RTP minimal mais valide, X=1, une extension one-byte portant `seq`.
	bool SendRtp(int port, WORD rtpSeq, WORD transportSeq, bool withExtension)
	{
		BYTE packet[32];
		memset(packet, 0, sizeof(packet));
		packet[0] = withExtension ? 0x90 : 0x80;	// V=2, X
		packet[1] = kPayloadType;
		set2(packet, 2, rtpSeq);
		set4(packet, 4, 0x11223344);			// timestamp
		set4(packet, 8, kProbeSSRC);
		DWORD len = 12;
		if (withExtension)
		{
			set2(packet, len, 0xBEDE);		// profil one-byte
			set2(packet, len + 2, 1);		// 1 mot d'extension
			packet[len + 4] = (BYTE)(kExtId << 4 | 0x01);	// id, len=2 octets
			set2(packet, len + 5, transportSeq);
			packet[len + 7] = 0;			// padding
			len += 8;
		}
		// Charge utile : la session jette un paquet dont l'en-tête déborde
		memset(packet + len, 0xAA, 4);
		len += 4;

		sockaddr_in to;
		memset(&to, 0, sizeof(to));
		to.sin_family      = AF_INET;
		to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		to.sin_port        = htons(port);
		return sendto(fd, packet, len, 0, (sockaddr*)&to, sizeof(to)) == (ssize_t)len;
	}

	// Attend un RTP porteur de l'extension one-byte et rend l'id lu, ou 0.
	BYTE WaitForRtpExtensionId(int timeoutMs)
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
			// Les paquets d'amorcage NAT sont des en-tetes nus, sans extension
			if (size < 17 || RTCPCompoundPacket::IsRTCP(buffer, size))
				continue;
			if (!(buffer[0] & 0x10) || get2(buffer, 12) != 0xBEDE)
				continue;
			return (BYTE)(buffer[16] >> 4);
		}
		return 0;
	}

	// Attend un RTCP RTPFB fmt 15. Rend false au bout de timeoutMs. Les paquets
	// d'amorçage NAT et les rapports d'émission traversent sans gêner.
	bool WaitForTransportFeedback(int timeoutMs, TransportWideFeedbackField& field, DWORD& mediaSSRC)
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
			if (size <= 0 || !RTCPCompoundPacket::IsRTCP(buffer, size))
				continue;

			RTCPCompoundPacket* rtcp = RTCPCompoundPacket::Parse(buffer, size);
			if (!rtcp)
				continue;
			bool found = false;
			for (DWORD i = 0; i < rtcp->GetPacketCount() && !found; ++i)
			{
				RTCPPacket* packet = rtcp->GetPacket(i);
				if (packet->GetType() != RTCPPacket::RTPFeedback)
					continue;
				RTCPRTPFeedback* fb = (RTCPRTPFeedback*)packet;
				if (fb->GetFeedbackType() != RTCPRTPFeedback::TransportWideFeedbackMessage)
					continue;
				if (fb->GetFieldCount() != 1)
					continue;
				field = *(TransportWideFeedbackField*)fb->GetField(0);
				mediaSSRC = fb->GetMediaSSRC();
				found = true;
			}
			delete rtcp;
			if (found)
				return true;
		}
		return false;
	}

private:
	int         fd = -1;
	sockaddr_in bound {};
};

// Session prête à recevoir et à répondre : RTCP multiplexé sur le port RTP pour
// que le rapport revienne au socket unique de la sonde.
class Session
{
public:
	explicit Session(bool negotiateTransportCC)
		: session(MediaFrame::Audio, &listener)
	{
		ok = (session.Init() == 1);
		if (!ok)
			return;

		RTPMap map;
		map[kPayloadType] = kCodec;
		session.SetSendingRTPMap(map);
		session.SetReceivingRTPMap(map);

		Properties props;
		props["rtcp-mux"] = "1";
		if (negotiateTransportCC)
			props[kTransportCCUri] = "3";
		session.SetProperties(props);
	}

	~Session() { if (ok) session.End(); }

	// Un paquet media minimal vers la cible courante.
	void SendMedia()
	{
		DWORD codec = kCodec;
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

const int kExpectTimeoutMs = 2000;
const int kDenyTimeoutMs   = 800;

} // namespace

// LE test de ce lot : le pair envoie des paquets numérotés, il doit recevoir un
// rapport d'arrivée. C'est tout ce que son estimateur émetteur attend de nous, et
// sans lui il recule jusqu'à son plancher.
TEST(TransportCCWiring, LaSessionRapporteLesArriveesAuPair)
{
	Probe probe;
	Session sess(true);
	REQUIRE_LOOPBACK(probe, sess);

	char ip[] = "127.0.0.1";
	ASSERT_EQ(1, sess.session.SetRemotePort(ip, probe.Port()));

	ASSERT_TRUE(probe.SendRtp(sess.session.GetLocalPort(), 1, 100, true));
	ASSERT_TRUE(probe.SendRtp(sess.session.GetLocalPort(), 2, 101, true));

	TransportWideFeedbackField field;
	DWORD mediaSSRC = 0;
	ASSERT_TRUE(probe.WaitForTransportFeedback(kExpectTimeoutMs, field, mediaSSRC))
		<< "aucun rapport fmt 15 recu : le pair est aveugle sur ce qu'il nous envoie";

	EXPECT_EQ(kProbeSSRC, mediaSSRC) << "le rapport doit nommer le flux rapporte";
	EXPECT_EQ(100, field.baseSeq);
	ASSERT_FALSE(field.packets.empty());
	EXPECT_TRUE(field.packets[0].received);
}

// Le garde-fou : sans extension négociée, la session ne doit rien émettre — un
// rapport spontané parlerait d'un compteur que personne n'écrit.
TEST(TransportCCWiring, SansNegociationAucunRapport)
{
	Probe probe;
	Session sess(false);
	REQUIRE_LOOPBACK(probe, sess);

	char ip[] = "127.0.0.1";
	ASSERT_EQ(1, sess.session.SetRemotePort(ip, probe.Port()));

	// Le pair porte quand même l'extension : c'est notre absence de négociation
	// qui doit trancher, pas ce que le paquet contient.
	ASSERT_TRUE(probe.SendRtp(sess.session.GetLocalPort(), 1, 100, true));

	TransportWideFeedbackField field;
	DWORD mediaSSRC = 0;
	EXPECT_FALSE(probe.WaitForTransportFeedback(kDenyTimeoutMs, field, mediaSSRC));
}

// L'autre moitie du cablage : nos paquets sortants portent l'extension avec l'id
// que le pair a negocie. Sans elle, son recepteur ne peut rien nous rapporter —
// et il cesse pourtant d'emettre du REMB des que la negociation a eu lieu.
TEST(TransportCCWiring, NosPaquetsPortentLExtensionNegociee)
{
	Probe probe;
	Session sess(true);
	REQUIRE_LOOPBACK(probe, sess);

	char ip[] = "127.0.0.1";
	ASSERT_EQ(1, sess.session.SetRemotePort(ip, probe.Port()));

	for (int i = 0; i < 10; ++i)
	{
		sess.SendMedia();
		const BYTE id = probe.WaitForRtpExtensionId(100);
		if (id)
		{
			EXPECT_EQ(kExtId, id) << "id d'extension ecrit different du negocie";
			return;
		}
	}
	FAIL() << "aucun paquet sortant ne porte l'extension transport-wide";
}
