/**
 * test_rtp_renegotiation.cpp — le trou de l'offre/réponse en réception RTP.
 *
 * Une renégociation (re-INVITE, UPDATE) renumérote couramment les payload types
 * dynamiques : Linphone déplace OPUS de 96 à 98, VP8 de 96 à 107. Nous appliquons
 * la nouvelle numérotation dès que nous RÉPONDONS ; l'offreur, lui, ne bascule
 * qu'en RECEVANT cette réponse (RFC 3264 §8). Entre les deux — un aller-retour SIP,
 * plus le temps qu'un B2BUA met à obtenir la réponse de l'autre jambe — ses paquets
 * portent encore l'ANCIEN numéro.
 *
 * Ces paquets étaient jetés. Inaudible en audio, VISIBLE en vidéo : le décodeur perd
 * tout ce qui suit la dernière intra reçue, et l'image reste figée jusqu'à la
 * suivante. Traffic du 2026-08-13 : ~450 ms de vidéo perdue à chaque re-INVITE, et
 * deux cents lignes d'erreur pour le dire.
 *
 * `RTPSession` garde donc la map de réception précédente quelques secondes et s'y
 * rattrape — sous UNE garde, qui est tout l'objet de la seconde suite : le codec de
 * l'ancien numéro doit être ENCORE négocié. Ce qui est réparé est une
 * renumérotation, rien d'autre ; un codec réellement retiré de la négociation reste
 * un rebut.
 *
 * Observation : `onNewStream`, le seul signal PUBLIC qu'un paquet a été accepté —
 * il n'est émis qu'après la résolution du codec, donc jamais pour un paquet jeté.
 * Chaque phase émet donc son propre SSRC. Rien d'interne n'est inspecté (même
 * parti pris que test_rtp_latching.cpp).
 */
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "log.h"
#include "medkit/codecs.h"
#include "rtp.h"
#include "rtpsession.h"

namespace {

// Numérotation d'avant et d'après la renégociation, et les codecs en jeu.
const BYTE kOldType = 96;
const BYTE kNewType = 107;

// SSRC : un par phase, puisque c'est l'arrivée d'un SSRC INCONNU sur un paquet
// accepté qui fait signe. Le premier sert d'amorce (il devient le flux par défaut,
// ce qui ne passe pas par le listener) ; les suivants sont les mesures.
const DWORD kPrimingSsrc = 0x11111111;
const DWORD kProbeSsrc   = 0x22222222;
const DWORD kProbe2Ssrc  = 0x33333333;

// Listener minimal. `onNewStream` N'APPELLE PAS l'implémentation de base : celle-ci
// ré-aiguille le flux par défaut, un effet de bord dont le test n'a que faire.
class CountingListener : public RTPSession::Listener
{
public:
	void onFPURequested(RTPSession*) override {}
	void onReceiverEstimatedMaxBitrate(RTPSession*, DWORD) override {}
	void onTempMaxMediaStreamBitrateRequest(RTPSession*, DWORD, DWORD) override {}
	void onNewStream(RTPSession*, DWORD, bool receiving) override
	{
		if (receiving)
			newStreams++;
	}

	int newStreams = 0;
};

// Socket UDP en loopback jouant le pair qui émet vers la session.
class Peer
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
		addr.sin_port        = 0;   // port éphémère
		return bind(fd, (sockaddr*)&addr, sizeof(addr)) == 0;
	}

	~Peer() { if (fd >= 0) close(fd); }

	// RTP minimal mais valide : V=2, 12 octets d'en-tête, une charge utile quelconque.
	bool Send(int port, BYTE payloadType, DWORD ssrc, WORD seq)
	{
		BYTE packet[20];
		memset(packet, 0, sizeof(packet));
		packet[0] = 0x80;
		packet[1] = (BYTE)(payloadType & 0x7f);
		packet[2] = (BYTE)(seq >> 8); packet[3] = (BYTE)seq;
		packet[8]  = (BYTE)(ssrc >> 24); packet[9]  = (BYTE)(ssrc >> 16);
		packet[10] = (BYTE)(ssrc >> 8);  packet[11] = (BYTE)ssrc;

		sockaddr_in to;
		memset(&to, 0, sizeof(to));
		to.sin_family      = AF_INET;
		to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		to.sin_port        = htons(port);
		return sendto(fd, packet, sizeof(packet), 0, (sockaddr*)&to, sizeof(to)) == (ssize_t)sizeof(packet);
	}

private:
	int fd = -1;
};

// Session vidéo en réception, map de départ {96 -> VP8}.
class VideoSession
{
public:
	VideoSession() : session(MediaFrame::Video, &listener)
	{
		ok = (session.Init() == 1);
		if (!ok)
			return;

		Renegotiate(kOldType, VideoCodec::VP8);
	}

	~VideoSession() { if (ok) session.End(); }

	// Ce que fait StartReceiving à chaque offre/réponse : une nouvelle map de
	// réception, et une seule ligne — c'est là que la précédente devient le repli.
	void Renegotiate(BYTE type, BYTE codec)
	{
		RTPMap map;
		map[type] = codec;
		session.SetReceivingRTPMap(map);
	}

	int Port() { return session.GetLocalPort(); }

	// Émet `payloadType`/`ssrc` et laisse le thread de réception le traiter. Rend
	// le nombre de nouveaux flux annoncés depuis l'appel — 1 = paquet accepté,
	// 0 = paquet jeté. La boucle est une attente, pas une cadence : elle sort dès
	// que le compte bouge.
	int SendAndCount(Peer& peer, BYTE payloadType, DWORD ssrc, WORD seq)
	{
		const int before = listener.newStreams;
		EXPECT_TRUE(peer.Send(Port(), payloadType, ssrc, seq));

		for (int waited = 0; waited < 500 && listener.newStreams == before; waited += 10)
			usleep(10 * 1000);

		return listener.newStreams - before;
	}

	bool ok = false;
	CountingListener listener;
	RTPSession session;
};

TEST(RtpRenegotiation, SalvagesAPacketStillSentWithThePreviousPayloadType)
{
	Peer peer;
	ASSERT_TRUE(peer.Open());

	VideoSession s;
	ASSERT_TRUE(s.ok);

	// Le pair émet, tout va bien : ce premier SSRC devient le flux par défaut.
	s.SendAndCount(peer, kOldType, kPrimingSsrc, 1);

	// Renégociation : MÊME codec, NOUVEAU numéro. Le pair n'a pas encore vu la
	// réponse et continue sur l'ancien.
	s.Renegotiate(kNewType, VideoCodec::VP8);

	EXPECT_EQ(1, s.SendAndCount(peer, kOldType, kProbeSsrc, 2))
		<< "un paquet portant l'ancien numéro d'un codec toujours négocié doit "
		   "être rattrapé, pas jeté : c'est la vidéo de la seconde qui suit le "
		   "re-INVITE";

	// …et la nouvelle numérotation marche évidemment aussi.
	EXPECT_EQ(1, s.SendAndCount(peer, kNewType, kProbe2Ssrc, 3));
}

TEST(RtpRenegotiation, DropsAPayloadTypeWhoseCodecTheRenegotiationRemoved)
{
	Peer peer;
	ASSERT_TRUE(peer.Open());

	VideoSession s;
	ASSERT_TRUE(s.ok);

	s.SendAndCount(peer, kOldType, kPrimingSsrc, 1);

	// Renégociation qui abandonne VP8 pour H.264. Le numéro 96 est connu de la map
	// précédente, mais ce qu'il désigne n'est plus négocié du tout.
	s.Renegotiate(kNewType, VideoCodec::H264);

	EXPECT_EQ(0, s.SendAndCount(peer, kOldType, kProbeSsrc, 2))
		<< "un codec retiré de la négociation ne doit PAS repasser par la porte du "
		   "rattrapage : le pair d'en face vient de le refuser";

	EXPECT_EQ(1, s.SendAndCount(peer, kNewType, kProbe2Ssrc, 3))
		<< "le codec que la renégociation a retenu, lui, passe";
}

// Non testé ici : l'EXPIRATION du repli (RTP_MAP_FALLBACK_MS, 5 s). Elle demanderait
// soit d'attendre cinq secondes dans la suite, soit une horloge injectable dans
// RTPSession — l'un est trop lent, l'autre est une refonte. La borne est un garde-fou
// contre un pair qui ne bascule jamais, pas le comportement que ces tests protègent.

}  // namespace
