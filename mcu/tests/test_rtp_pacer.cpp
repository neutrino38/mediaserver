/**
 * test_rtp_pacer.cpp — le lisseur RTP étale vraiment, et il étale AUSSI après
 * un silence.
 *
 * `RTPSmoother` datait chaque paquet par un offset depuis le début de son image,
 * et remettait son horloge à zéro au bit de marque. Conséquence : après une
 * pause plus longue que le budget de l'image, le temps écoulé dépassait tous les
 * offsets, donc AUCUNE attente n'était honorée et l'image entière partait en
 * rafale. C'est précisément ce que l'estimateur émetteur mesurait à la place du
 * réseau (séance du 2026-08-20 : le débit acquitté variait de 2,7x à l'intérieur
 * d'une même seconde, et dépassait le débit émis dans 39 % des échantillons).
 *
 * Le pacer à budget remplace cela par un curseur continu : chaque paquet porte
 * son temps de passage sur le fil, la dette se reporte d'une image à l'autre, et
 * le curseur ne traîne jamais dans le passé.
 *
 * On ne teste aucun état interne : on mesure ce que voit le pair, par un socket
 * sonde en loopback — même philosophie que test_rtp_latching.cpp.
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
#include "rtp.h"
#include "rtpsession.h"
#include "RTPSmoother.h"
#include "tools.h"
#include "video.h"

namespace {

const VideoCodec::Type kCodec = VideoCodec::H264;
const BYTE  kPayloadType = 100;

class StubListener : public RTPSession::Listener
{
public:
	void onFPURequested(RTPSession*) override {}
	void onReceiverEstimatedMaxBitrate(RTPSession*, DWORD) override {}
	void onTempMaxMediaStreamBitrateRequest(RTPSession*, DWORD, DWORD) override {}
};

// Socket UDP en loopback : joue le pair et HORODATE chaque arrivée.
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

	// Collecte les instants d'arrivée (us) des datagrammes portant une charge
	// utile, pendant au plus `windowMs`. S'arrête tôt si `expected` sont arrivés ;
	// `expected == 0` veut dire « vide la ligne pendant toute la fenêtre » (PIÈGE :
	// une comparaison `size() < expected` non signée sortirait aussitôt, et le
	// silence que le test croit observer n'aurait jamais lieu).
	std::vector<QWORD> Collect(int windowMs, size_t expected)
	{
		std::vector<QWORD> arrivals;
		pollfd pfd = { fd, POLLIN, 0 };
		while (windowMs > 0 && (!expected || arrivals.size() < expected))
		{
			const int slice = windowMs < 10 ? windowMs : 10;
			const int ret = poll(&pfd, 1, slice);
			windowMs -= slice;
			if (ret <= 0)
				continue;
			BYTE buffer[MTU];
			const ssize_t size = recv(fd, buffer, sizeof(buffer), 0);
			//En-tête nu = amorçage NAT, pas du média
			if (size > 12)
				arrivals.push_back(getTime());
		}
		return arrivals;
	}

	int Port() const { return ntohs(bound.sin_port); }

private:
	int         fd = -1;
	sockaddr_in bound {};
};

// Image vidéo de `packets` paquets de `bytes` octets, déjà packetisée.
VideoFrame* MakeFrame(size_t packets, size_t bytes)
{
	VideoFrame* frame = new VideoFrame(kCodec, packets * bytes + 16);
	std::vector<BYTE> payload(packets * bytes, 0x5A);
	frame->SetLength(0);
	frame->AppendMedia(payload.data(), payload.size());
	frame->ClearRTPPacketizationInfo();
	for (size_t i = 0; i < packets; i++)
		frame->AddRtpPacket(i * bytes, bytes, NULL, 0, i + 1 == packets);
	frame->SetTimestamp(0);
	return frame;
}

// Session bindée, table d'émission posée, cible = la sonde.
class Sender
{
public:
	explicit Sender(ProbeSocket& probe)
		: session(MediaFrame::Video, &listener)
	{
		ok = (session.Init() == 1);
		if (!ok)
			return;
		RTPMap map;
		map[kPayloadType] = (DWORD)kCodec;
		session.SetSendingRTPMap(map);
		session.SetReceivingRTPMap(map);
		session.SetRemotePort((char*)"127.0.0.1", probe.Port());
		ok = (smoother.Init(&session) == 1);
	}

	~Sender()
	{
		smoother.End();
		if (ok)
			session.End();
	}

	bool Ok() const { return ok; }
	RTPSmoother& Smoother() { return smoother; }

private:
	StubListener listener;
	RTPSession   session;
	RTPSmoother  smoother;
	bool         ok = false;
};

// Étalement observé : dernier moins premier, en ms.
QWORD SpreadMs(const std::vector<QWORD>& arrivals)
{
	if (arrivals.size() < 2)
		return 0;
	return (arrivals.back() - arrivals.front()) / 1000;
}

} // namespace

// Contrat de base : une image de 10 paquets à qui on donne 100 ms de budget
// s'étale sur ce budget, elle ne part pas d'un bloc.
TEST(RtpPacer, UneImageSEtaleSurSonBudget)
{
	ProbeSocket probe;
	ASSERT_TRUE(probe.Open());
	Sender sender(probe);
	ASSERT_TRUE(sender.Ok());

	//Laisse passer la rafale d'amorçage NAT de SetRemotePort
	probe.Collect(120, 0);

	VideoFrame* frame = MakeFrame(10, 400);
	ASSERT_EQ(sender.Smoother().SendFrame(frame, 100), 1);
	delete frame;

	std::vector<QWORD> arrivals = probe.Collect(400, 10);
	ASSERT_EQ(arrivals.size(), 10u) << "tous les paquets de l'image doivent sortir";

	const QWORD spread = SpreadMs(arrivals);
	//Budget 100 ms : on attend l'ordre de grandeur, pas la milliseconde.
	EXPECT_GE(spread, 50u) << "image partie en rafale (" << spread << " ms)";
	EXPECT_LE(spread, 200u) << "étalement bien au-delà du budget (" << spread << " ms)";
}

// LE défaut corrigé : après un silence plus long que le budget, l'image suivante
// doit rester lissée. L'ancien lisseur n'attendait plus du tout (le temps écoulé
// depuis la dernière marque dépassait tous les offsets) et la vidait d'un coup.
TEST(RtpPacer, ApresUnSilenceLImageResteLissee)
{
	ProbeSocket probe;
	ASSERT_TRUE(probe.Open());
	Sender sender(probe);
	ASSERT_TRUE(sender.Ok());

	probe.Collect(120, 0);

	//Première image : elle pose l'horloge du lisseur.
	VideoFrame* first = MakeFrame(4, 400);
	ASSERT_EQ(sender.Smoother().SendFrame(first, 40), 1);
	delete first;
	ASSERT_EQ(probe.Collect(300, 4).size(), 4u);

	//Silence franchement plus long que le budget d'une image.
	probe.Collect(400, 0);

	//Seconde image, même budget : elle doit s'étaler comme la première.
	VideoFrame* second = MakeFrame(10, 400);
	ASSERT_EQ(sender.Smoother().SendFrame(second, 100), 1);
	delete second;

	std::vector<QWORD> arrivals = probe.Collect(400, 10);
	ASSERT_EQ(arrivals.size(), 10u);

	const QWORD spread = SpreadMs(arrivals);
	EXPECT_GE(spread, 50u)
		<< "rattrapage en rafale après le silence (" << spread << " ms pour 100 ms de budget)";
}

// L'étalement d'une image est borné par la LATENCE (MaxSpreadUs = 200 ms), pas
// par la période d'image. Un budget démesuré est ramené à cette borne : sans
// elle, une image dont l'encodeur a mal estimé le coût retiendrait ses derniers
// paquets pendant des secondes.
TEST(RtpPacer, LEtalementDUneImageEstBorne)
{
	ProbeSocket probe;
	ASSERT_TRUE(probe.Open());
	Sender sender(probe);
	ASSERT_TRUE(sender.Ok());

	probe.Collect(120, 0);

	//5 s de budget pour 10 paquets : sans borne, le dernier partirait à t+4,5 s.
	VideoFrame* big = MakeFrame(10, 400);
	ASSERT_EQ(sender.Smoother().SendFrame(big, 5000), 1);
	delete big;

	std::vector<QWORD> arrivals = probe.Collect(1500, 10);
	ASSERT_EQ(arrivals.size(), 10u) << "l'image doit sortir malgré un budget démesuré";

	const QWORD spread = SpreadMs(arrivals);
	//Ramené à 200 ms : on laisse une marge d'ordonnancement.
	EXPECT_LE(spread, 500u) << "budget non borné : " << spread << " ms d'étalement";
	//Et l'étalement reste réel, la borne n'est pas un retour à la rafale.
	EXPECT_GE(spread, 80u) << "image partie en rafale (" << spread << " ms)";
}
