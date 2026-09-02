/**
 * test_rtp_stream_race.cpp — `streams` / `defaultStream` détruits pendant que le
 * chemin RTCP les lit.
 *
 * `DeleteStreams()` détruit les RTPStream et remet `defaultStream` à NULL sous le
 * verrou écrivain `streamUse`. Tout le chemin de construction RTCP les lisait
 * SANS ce verrou : `CreateSenderReport` itère la map, une dizaine d'autres
 * fonctions déréférencent `defaultStream`.
 *
 * Ce n'était pas théorique. `RTPEndpoint::StopReceiving` appelle `DeleteStreams`
 * alors que le thread `RTPSession::Run` tourne TOUJOURS — il n'est arrêté qu'à
 * `End()` —, et ce thread émet des rapports RTCP périodiques. L'estimateur d'un
 * Endpoint étant partagé par ses jambes, le thread RTP d'une autre jambe pouvait
 * en plus entrer ici par `onTargetBitrateRequested`.
 *
 * ATTENTION — comme test_endpoint_teardown, ce test ne prouve rien sans
 * instrumentation : lire un objet fraîchement libéré passe inaperçu. Il n'est un
 * garde-fou que sous AddressSanitizer :
 *
 *     cd mcu && make clean && make check ASAN=yes
 *
 * (puis rebâtir sans ASAN=yes : les objets partagent le même répertoire.)
 */
#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "rtpsession.h"

namespace {

// La session exige un listener non nul.
class StubListener : public RTPSession::Listener
{
public:
	void onFPURequested(RTPSession*) override {}
	void onReceiverEstimatedMaxBitrate(RTPSession*, DWORD) override {}
	void onTempMaxMediaStreamBitrateRequest(RTPSession*, DWORD, DWORD) override {}
};

const DWORD kFirstSSRC   = 0x1000;
const int   kStreamCount = 8;
const int   kRounds      = 300;

// Entrées PUBLIQUES qui atteignent le chemin RTCP : RequestFPU descend vers
// SendFIR -> CreateSenderReport (itère `streams`), SetMaxReceiveBitrate vers
// SendTempMaxMediaStreamBitrateRequest / SendReceiverEstimatedMaxBitrate (lisent
// `defaultStream` puis CreateSenderReport), GetStatistics itère la map. C'est le
// même code que joue le thread RTPSession::Run par SendSenderReport, privé.
// Sans pair distant, SendPacket échoue et rend 0 — mais la map a déjà été
// parcourue, ce qui est précisément le point.
TEST(RTPStreamRace, LeCheminRtcpNeLitPasDesStreamsDetruits)
{
	StubListener listener;
	RTPSession   session(MediaFrame::Video, &listener);

	if (session.Init() != 1)
		GTEST_SKIP() << "pas de socket disponible (bac à sable réseau)";

	RTPMap map;
	map[96] = 96;
	session.SetSendingRTPMap(map);
	session.SetReceivingRTPMap(map);

	std::atomic<bool> stop(false);
	std::thread reader([&] {
		// Le plafond doit VARIER : l'amortisseur ne réémet pas une valeur déjà
		// annoncée, et ce tour ne descendrait plus dans CreateSenderReport.
		bool high = false;
		while (!stop)
		{
			session.RequestFPU();
			session.SetMaxReceiveBitrate(high ? 600000 : 300000);
			high = !high;
			MediaStatistics stats;
			session.GetStatistics(0, stats);
		}
	});

	for (int round = 0; round < kRounds; round++)
	{
		for (int i = 0; i < kStreamCount; i++)
			session.AddStream(true, kFirstSSRC + i);
		session.SetDefaultStream(true, kFirstSSRC);

		session.DeleteStreams();
	}

	stop = true;
	reader.join();

	session.End();
}

} // namespace
