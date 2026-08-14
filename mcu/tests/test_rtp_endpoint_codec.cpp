/**
 * test_rtp_endpoint_codec.cpp — un endpoint n'émet que ce qu'il sait étiqueter.
 *
 * `RTPEndpoint::onRTPPacket` bascule le type de charge utile quand le codec du
 * paquet change, par `RTPSession::SetSendingCodec`. Celui-ci ÉCHOUE si le codec
 * n'est pas dans la rtpMap de sortie négociée, et laisse alors le PT PRÉCÉDENT
 * dans l'en-tête. Son verdict était ignoré : le paquet partait quand même, des
 * octets d'un codec sous l'étiquette d'un autre. Le pair ne voit aucune erreur —
 * il lit le PT, croit savoir ce qu'il décode, et décode du bruit.
 *
 * Et `codec` était mis à jour AVANT l'appel, donc dès le paquet suivant le bloc
 * entier était sauté : l'échec était journalisé UNE fois puis plus jamais, sans
 * retentative après une renégociation, et sans trace des paquets suivants.
 *
 * CE QUI EST OBSERVÉ, c'est ce que voit le pair : un socket sonde en loopback
 * reçoit-il le média, ou non. Ni `sendType` ni `codec` ne sont consultés — ils
 * sont privés, et le PT sur le fil est de toute façon le seul fait qui compte.
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
#include "../src/jsr309/RTPEndpoint.h"

namespace {

// Charge utile reconnaissable : distingue le média émis par SendPacket des
// paquets d'amorçage NAT (en-tête nu, sans charge utile).
const BYTE kMagic[] = { 0xC0, 0xFF, 0xEE, 0x42, 0x13, 0x37 };

// La rtpMap de sortie ne porte QUE H.264. VP8 est le codec qu'un plan de contrôle
// peut demander à tort — c'est exactement la situation d'une sélection cross-leg
// qui choisit un codec de l'intersection alors que la carte d'émission de la patte
// est épinglée sur un seul type.
const BYTE kPayloadTypeH264 = 99;
const DWORD kCodecH264      = VideoCodec::H264;
const DWORD kCodecVP8       = VideoCodec::VP8;

class ProbeSocket
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

	~ProbeSocket() { if (fd >= 0) close(fd); }

	// Attend un datagramme portant kMagic. Rend le type de charge utile lu dans
	// l'en-tête RTP, ou -1 si rien n'arrive avant expiration.
	int WaitForMagic(int timeoutMs)
	{
		pollfd pfd = { fd, POLLIN, 0 };
		while (timeoutMs > 0)
		{
			const int slice = timeoutMs < 50 ? timeoutMs : 50;
			const int ret = poll(&pfd, 1, slice);
			timeoutMs -= slice;
			if (ret <= 0)
				continue;

			BYTE buffer[MTU];
			const ssize_t size = recv(fd, buffer, sizeof(buffer), 0);
			if (size < (ssize_t)(12 + sizeof(kMagic)))
				continue;	// en-tête nu : amorçage NAT, on l'ignore
			if (memcmp(buffer + 12, kMagic, sizeof(kMagic)) != 0)
				continue;

			return buffer[1] & 0x7f;
		}
		return -1;
	}

	int Port() const { return ntohs(bound.sin_port); }

private:
	int         fd = -1;
	sockaddr_in bound {};
};

// Endpoint vidéo prêt à émettre vers la sonde, sa carte de sortie ne portant que
// H.264.
class SendingEndpoint
{
public:
	explicit SendingEndpoint(int probePort) : endpoint(MediaFrame::Video)
	{
		// Le succès ne se lit PAS dans la valeur de retour : `RTPEndpoint::Init`
		// rend 0 quand tout va bien et `false` — donc 0 aussi — quand l'endpoint
		// était déjà initialisé. Aucun appelant ne peut les distinguer. On vérifie
		// donc le fait qui compte : une paire de ports RTP a bien été bindée.
		endpoint.Init();
		ok = (endpoint.GetLocalPort() > 0);
		if (!ok)
			return;

		RTPMap map;
		map[kPayloadTypeH264] = kCodecH264;
		endpoint.SetSendingRTPMap(map);
		endpoint.SetReceivingRTPMap(map);

		char ip[] = "127.0.0.1";
		endpoint.SetRemotePort(ip, probePort);
		endpoint.StartSending();
	}

	~SendingEndpoint() { if (ok) endpoint.End(); }

	// Publie un paquet média comme le ferait une source attachée (transcodeur,
	// mixeur, player) : c'est le chemin Joinable::Listener.
	void Publish(DWORD codec, WORD seq, DWORD ts)
	{
		DWORD c = codec;
		RTPPacket packet(MediaFrame::Video, c);
		packet.SetSeqNum(seq);
		packet.SetTimestamp(ts);
		memcpy(packet.GetMediaData(), kMagic, sizeof(kMagic));
		packet.SetMediaLength(sizeof(kMagic));
		endpoint.onRTPPacket(packet);
	}

	bool        ok = false;
	RTPEndpoint endpoint;
};

const int kExpectTimeoutMs = 1500;
const int kDenyTimeoutMs   = 400;

#define REQUIRE_LOOPBACK(probe, ep)                                            \
	do {                                                                   \
		if (!(probe).Open())                                           \
			GTEST_SKIP() << "socket loopback indisponible";        \
		if (!(ep).ok)                                                  \
			GTEST_SKIP() << "impossible de binder une paire de ports RTP"; \
	} while (0)

} // namespace

TEST(RTPEndpointCodec, UnCodecDeLaCarteEstEmisAvecSonType)
{
	// Le cas nominal, sans quoi le test négatif ne prouverait rien : un codec
	// présent dans la carte sort, et sort sous SON type.
	ProbeSocket probe;
	ASSERT_TRUE(probe.Open()) << "socket loopback indisponible";

	SendingEndpoint ep(probe.Port());
	if (!ep.ok)
		GTEST_SKIP() << "impossible de binder une paire de ports RTP";

	ep.Publish(kCodecH264, 1, 90000);

	EXPECT_EQ(probe.WaitForMagic(kExpectTimeoutMs), (int)kPayloadTypeH264);
}

TEST(RTPEndpointCodec, UnCodecHorsDeLaCarteNEstPasEmis)
{
	// LE test de régression. Avant correction, ce paquet VP8 partait sous le PT 99
	// — celui de H.264 — et le pair décodait du VP8 comme du H.264.
	ProbeSocket probe;
	ASSERT_TRUE(probe.Open()) << "socket loopback indisponible";

	SendingEndpoint ep(probe.Port());
	if (!ep.ok)
		GTEST_SKIP() << "impossible de binder une paire de ports RTP";

	// D'abord un paquet légitime : c'est lui qui installe le PT 99 dans l'en-tête
	// d'émission, donc l'étiquette que le paquet VP8 usurperait.
	ep.Publish(kCodecH264, 1, 90000);
	ASSERT_EQ(probe.WaitForMagic(kExpectTimeoutMs), (int)kPayloadTypeH264);

	ep.Publish(kCodecVP8, 2, 93000);

	EXPECT_EQ(probe.WaitForMagic(kDenyTimeoutMs), -1)
		<< "un paquet hors carte doit etre jete, pas emis sous l'etiquette du precedent";
}

TEST(RTPEndpointCodec, UnCodecHorsDeLaCarteEstRejeteAChaquePaquet)
{
	// Le second défaut : `codec` était mis à jour avant l'appel, donc le bloc
	// entier sautait dès le paquet suivant et TOUS les paquets d'après partaient,
	// mal étiquetés et sans trace. Le refus doit tenir sur toute la rafale.
	ProbeSocket probe;
	ASSERT_TRUE(probe.Open()) << "socket loopback indisponible";

	SendingEndpoint ep(probe.Port());
	if (!ep.ok)
		GTEST_SKIP() << "impossible de binder une paire de ports RTP";

	ep.Publish(kCodecH264, 1, 90000);
	ASSERT_EQ(probe.WaitForMagic(kExpectTimeoutMs), (int)kPayloadTypeH264);

	for (int i = 0; i < 20; i++)
		ep.Publish(kCodecVP8, (WORD)(2 + i), 93000 + i * 3000);

	EXPECT_EQ(probe.WaitForMagic(kDenyTimeoutMs), -1)
		<< "le refus doit valoir pour la rafale entiere, pas seulement le 1er paquet";
}

TEST(RTPEndpointCodec, UneRenegociationQuiAjouteLeTypeDebloqueLEmission)
{
	// Corollaire de la borne de retentative : ne pas retenter du tout aurait
	// condamné le flux jusqu'au raccrochage. Après ajout du type à la carte,
	// l'émission doit reprendre.
	ProbeSocket probe;
	ASSERT_TRUE(probe.Open()) << "socket loopback indisponible";

	SendingEndpoint ep(probe.Port());
	if (!ep.ok)
		GTEST_SKIP() << "impossible de binder une paire de ports RTP";

	ep.Publish(kCodecVP8, 1, 90000);
	ASSERT_EQ(probe.WaitForMagic(kDenyTimeoutMs), -1) << "VP8 n'est pas dans la carte";

	// Renégociation : le pair a accepté VP8 sur le type 107.
	const BYTE kPayloadTypeVP8 = 107;
	RTPMap map;
	map[kPayloadTypeH264] = kCodecH264;
	map[kPayloadTypeVP8]  = kCodecVP8;
	ep.endpoint.SetSendingRTPMap(map);

	// La retentative est bornée à une par seconde : laisser la fenêtre s'écouler,
	// sinon le paquet suivant serait jeté sans même redemander à la carte.
	usleep(1100 * 1000);

	ep.Publish(kCodecVP8, 2, 93000);

	EXPECT_EQ(probe.WaitForMagic(kExpectTimeoutMs), (int)kPayloadTypeVP8)
		<< "apres renegociation, VP8 doit sortir sous SON type";
}
