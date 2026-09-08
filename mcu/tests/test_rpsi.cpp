/**
 * test_rpsi.cpp — l'acquittement RPSI (RFC 4585 §6.3.3), lot 2 de
 * vp8_golden_frame_plan.md : le format de fil du champ
 * ReferencePictureSelectionField et l'émission de bout en bout par
 * RTPSession::SendReferencePictureSelectionIndication.
 *
 * Le format s'éprouve aux OCTETS près (l'aller-retour Parse/Serialize seul ne
 * prouve rien, les deux mains sont les nôtres), et le câblage s'éprouve comme
 * dans test_transport_cc_wiring.cpp : une sonde UDP en loopback joue le pair,
 * envoie du RTP VP8, et guette le PSFB fmt 3 que la session lui doit.
 */
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <vector>

#include "log.h"
#include "medkit/codecs.h"
#include "rtp.h"
#include "rtpsession.h"

namespace {

const BYTE  kPayloadType = 96;			// PT VP8 déclaré par le pair
const BYTE  kCodec       = VideoCodec::VP8;
const DWORD kProbeSSRC   = 0x5678CDEF;

class StubListener : public RTPSession::Listener
{
public:
	void onFPURequested(RTPSession*) override {}
	void onReceiverEstimatedMaxBitrate(RTPSession*, DWORD) override {}
	void onTempMaxMediaStreamBitrateRequest(RTPSession*, DWORD, DWORD) override {}
};

struct RpsiCapture
{
	DWORD mediaSSRC = 0;
	BYTE  padding   = 0xFF;
	BYTE  type      = 0xFF;
	std::vector<BYTE> bitString;
	std::vector<BYTE> rawFci;	// les octets du fil, hors parseur maison
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
		addr.sin_port        = 0;
		if (bind(fd, (sockaddr*)&addr, sizeof(addr)) != 0)
			return false;
		socklen_t len = sizeof(bound);
		return getsockname(fd, (sockaddr*)&bound, &len) == 0;
	}

	~Probe() { if (fd >= 0) close(fd); }

	int Port() const { return ntohs(bound.sin_port); }

	// RTP minimal accepté par la session : c'est lui qui pose recSSRC et
	// recCodec, sans lesquels il n'y a rien à acquitter.
	bool SendRtp(int port, WORD rtpSeq)
	{
		BYTE packet[20];
		memset(packet, 0, sizeof(packet));
		packet[0] = 0x80;			// V=2
		packet[1] = kPayloadType;
		set2(packet, 2, rtpSeq);
		set4(packet, 4, 0x11223344);		// timestamp
		set4(packet, 8, kProbeSSRC);
		memset(packet + 12, 0xAA, 4);
		sockaddr_in to;
		memset(&to, 0, sizeof(to));
		to.sin_family      = AF_INET;
		to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		to.sin_port        = htons(port);
		return sendto(fd, packet, 16, 0, (sockaddr*)&to, sizeof(to)) == 16;
	}

	// Attend un RTCP PSFB fmt 3 (RPSI). Rend false au bout de timeoutMs.
	bool WaitForRpsi(int timeoutMs, RpsiCapture& out)
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

			// Les octets bruts du FCI, indépendamment de notre parseur :
			// en-tête PSFB = 0x83 (V=2, fmt=3), PT=206, puis longueur,
			// 2 SSRC, et le FCI jusqu'à la fin du paquet RTCP.
			for (ssize_t i = 0; i + 12 <= size; i += 4)
				if (buffer[i] == 0x83 && buffer[i + 1] == 206)
				{
					DWORD words = get2(buffer, i + 2);
					DWORD end   = i + 4 * (words + 1);
					if (end <= (DWORD)size && end >= (DWORD)i + 12)
						out.rawFci.assign(buffer + i + 12, buffer + end);
					break;
				}

			RTCPCompoundPacket* rtcp = RTCPCompoundPacket::Parse(buffer, size);
			if (!rtcp)
				continue;
			bool found = false;
			for (DWORD i = 0; i < rtcp->GetPacketCount() && !found; ++i)
			{
				RTCPPacket* packet = rtcp->GetPacket(i);
				if (packet->GetType() != RTCPPacket::PayloadFeedback)
					continue;
				RTCPPayloadFeedback* fb = (RTCPPayloadFeedback*)packet;
				if (fb->GetFeedbackType() != RTCPPayloadFeedback::ReferencePictureSelectionIndication)
					continue;
				if (fb->GetFieldCount() != 1)
					continue;
				RTCPPayloadFeedback::ReferencePictureSelectionField* field =
					(RTCPPayloadFeedback::ReferencePictureSelectionField*)fb->GetField(0);
				out.mediaSSRC = fb->GetMediaSSRC();
				out.padding   = field->padding;
				out.type      = field->type;
				out.bitString.assign(field->payload, field->payload + field->length);
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

class Session
{
public:
	Session() : session(MediaFrame::Video, &listener)
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
		session.SetProperties(props);
	}

	~Session() { if (ok) session.End(); }

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

// La session apprend le flux du pair par son premier paquet RTP, sur son
// propre thread : tant qu'il n'est pas traité, l'émetteur rend 0 (rien à
// acquitter) — on réessaie jusqu'à ce que le RPSI parte vraiment.
bool AckOnceStreamKnown(RTPSession& session, WORD pictureId, int timeoutMs)
{
	while (timeoutMs > 0)
	{
		if (session.SendReferencePictureSelectionIndication(0, pictureId) > 0)
			return true;
		usleep(20000);
		timeoutMs -= 20;
	}
	return false;
}

const int kExpectTimeoutMs = 2000;
const int kDenyTimeoutMs   = 500;

} // namespace

TEST(RpsiField, SerialisationExacteBitString16Bits)
{
	const BYTE bits[] = { 0x81, 0x23 };
	RTCPPayloadFeedback::ReferencePictureSelectionField field(kPayloadType, bits, 2);
	ASSERT_EQ(field.GetSize(), 4u);	// FCI aligné 32 bits sans bourrage
	BYTE buffer[8];
	memset(buffer, 0xEE, sizeof(buffer));
	ASSERT_EQ(field.Serialize(buffer, sizeof(buffer)), 4u);
	EXPECT_EQ(buffer[0], 0x00);	// PB=0
	EXPECT_EQ(buffer[1], kPayloadType);
	EXPECT_EQ(buffer[2], 0x81);
	EXPECT_EQ(buffer[3], 0x23);
}

TEST(RpsiField, SerialisationExacteBitString8Bits)
{
	const BYTE bits[] = { 0x45 };
	RTCPPayloadFeedback::ReferencePictureSelectionField field(kPayloadType, bits, 1);
	ASSERT_EQ(field.GetSize(), 4u);	// 1 octet de bourrage pour l'alignement
	BYTE buffer[8];
	memset(buffer, 0xEE, sizeof(buffer));
	ASSERT_EQ(field.Serialize(buffer, sizeof(buffer)), 4u);
	EXPECT_EQ(buffer[0], 0x08);	// PB=8 bits
	EXPECT_EQ(buffer[1], kPayloadType);
	EXPECT_EQ(buffer[2], 0x45);
	EXPECT_EQ(buffer[3], 0x00);	// bourrage nul
}

TEST(RpsiField, AllerRetour)
{
	const BYTE bits[] = { 0x81, 0x23 };
	RTCPPayloadFeedback::ReferencePictureSelectionField field(kPayloadType, bits, 2);
	BYTE buffer[8];
	DWORD len = field.Serialize(buffer, sizeof(buffer));
	ASSERT_GT(len, 0u);
	RTCPPayloadFeedback::ReferencePictureSelectionField back;
	ASSERT_EQ(back.Parse(buffer, len), len);
	EXPECT_EQ(back.padding, field.padding);
	EXPECT_EQ(back.type, field.type);
	ASSERT_EQ(back.length, field.length);
	EXPECT_EQ(memcmp(back.payload, field.payload, back.length), 0);
}

TEST(RpsiField, BourrageInvalideRefuse)
{
	RTCPPayloadFeedback::ReferencePictureSelectionField field;
	//PB non multiple de 8 : un bit string RPSI est fait d'octets entiers
	BYTE fractionnaire[] = { 0x04, 0x60, 0x81, 0x23 };
	EXPECT_EQ(field.Parse(fractionnaire, sizeof(fractionnaire)), 0u);
	//PB plus grand que le champ
	RTCPPayloadFeedback::ReferencePictureSelectionField field2;
	BYTE deborde[] = { 0x20, 0x60, 0x81, 0x23 };	// 32 bits de bourrage sur 2 octets
	EXPECT_EQ(field2.Parse(deborde, sizeof(deborde)), 0u);
	//Trop court pour l'en-tête
	RTCPPayloadFeedback::ReferencePictureSelectionField field3;
	BYTE court[] = { 0x00 };
	EXPECT_EQ(field3.Parse(court, sizeof(court)), 0u);
}

// LE test du lot : le pair envoie du VP8, on acquitte une trame de référence,
// il doit recevoir un PSFB fmt 3 dont le bit string est le PictureID intact.
TEST(RpsiWiring, LaSessionAcquitteAuPair)
{
	Probe probe;
	Session sess;
	REQUIRE_LOOPBACK(probe, sess);

	char ip[] = "127.0.0.1";
	ASSERT_EQ(1, sess.session.SetRemotePort(ip, probe.Port()));
	ASSERT_TRUE(probe.SendRtp(sess.session.GetLocalPort(), 1));
	ASSERT_TRUE(AckOnceStreamKnown(sess.session, 0x8123, 2000))
		<< "le paquet RTP de la sonde n'a pas cree le flux entrant";

	RpsiCapture got;
	ASSERT_TRUE(probe.WaitForRpsi(kExpectTimeoutMs, got))
		<< "aucun PSFB fmt 3 recu : l'emetteur ne sera jamais acquitte";

	EXPECT_EQ(got.mediaSSRC, kProbeSSRC);
	EXPECT_EQ(got.type, kPayloadType) << "le FCI doit nommer le PT du flux acquitte";
	EXPECT_EQ(got.padding, 0);
	ASSERT_EQ(got.bitString.size(), 2u);
	EXPECT_EQ(got.bitString[0], 0x81);
	EXPECT_EQ(got.bitString[1], 0x23);

	//Les octets du fil, hors parseur maison : PB, PT, PictureID réseau
	ASSERT_EQ(got.rawFci.size(), 4u);
	EXPECT_EQ(got.rawFci[0], 0x00);
	EXPECT_EQ(got.rawFci[1], kPayloadType);
	EXPECT_EQ(got.rawFci[2], 0x81);
	EXPECT_EQ(got.rawFci[3], 0x23);
}

TEST(RpsiWiring, PictureIdCourtSurUnOctet)
{
	Probe probe;
	Session sess;
	REQUIRE_LOOPBACK(probe, sess);

	char ip[] = "127.0.0.1";
	ASSERT_EQ(1, sess.session.SetRemotePort(ip, probe.Port()));
	ASSERT_TRUE(probe.SendRtp(sess.session.GetLocalPort(), 1));
	ASSERT_TRUE(AckOnceStreamKnown(sess.session, 0x0045, 2000));

	RpsiCapture got;
	ASSERT_TRUE(probe.WaitForRpsi(kExpectTimeoutMs, got));
	EXPECT_EQ(got.padding, 8);
	ASSERT_EQ(got.bitString.size(), 1u);
	EXPECT_EQ(got.bitString[0], 0x45);
	ASSERT_EQ(got.rawFci.size(), 4u);
	EXPECT_EQ(got.rawFci[0], 0x08);
	EXPECT_EQ(got.rawFci[3], 0x00);
}

// Garde-fou : sans flux entrant il n'y a rien à acquitter, rien ne part.
TEST(RpsiWiring, SansFluxRecuRienNEstEmis)
{
	Probe probe;
	Session sess;
	REQUIRE_LOOPBACK(probe, sess);

	char ip[] = "127.0.0.1";
	ASSERT_EQ(1, sess.session.SetRemotePort(ip, probe.Port()));

	EXPECT_EQ(sess.session.SendReferencePictureSelectionIndication(0, 0x8123), 0);

	RpsiCapture got;
	EXPECT_FALSE(probe.WaitForRpsi(kDenyTimeoutMs, got));
}
