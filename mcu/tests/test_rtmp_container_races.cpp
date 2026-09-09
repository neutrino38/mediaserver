/**
 * test_rtmp_container_races.cpp — deux conteneurs RTMP mutés hors du verrou
 * `Use` qui les protège partout ailleurs.
 *
 * `RTMPConnection::chunkOutputStreams` : l'insertion d'un chunk stream, quand
 * `onMediaFrame` voit un streamId jamais reçu, prend le verrou en écrivain ;
 * l'itération d'écriture de `SerializeChunkData` le prend en lecteur. Mais les
 * accès aux clés fixes 2 et 3 ne le prenaient pas du tout : les envois de
 * commande et de contrôle, plus `onMetaData`. Le thread de lecture les joue
 * pendant que le thread média insère.
 *
 * `RTMPCachedPipedMediaStream::cached` : `Clear()` et `AddMediaListener()`
 * prennent le verrou `use`, le `push_back` de `SendMediaFrame` ne le prenait
 * pas. Un spectateur qui rejoint un broadcast rejoue la liste qu'une trame est
 * en train d'étendre.
 *
 * ATTENTION — comme test_rtp_stream_race.cpp, ces tests ne prouvent rien sans
 * instrumentation : une lecture concurrente d'un `std::map` ou d'un
 * `std::list` passe le plus souvent inaperçue. Ils ne sont des garde-fous que
 * sous ThreadSanitizer :
 *
 *     cd mcu && make check TSAN=yes TAG=tsan
 *
 * Sans TSan, ils vérifient seulement que les deux chemins cohabitent sans
 * planter et sans bloquer.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>
#include <sys/socket.h>
#include <unistd.h>

#include "rtmpconnection.h"
#include "rtmpstream.h"

namespace {

// Listener bidon : le stream refuse un listener nul, et compte ce qu'il relaie.
class StubStreamListener : public RTMPMediaStream::Listener
{
public:
	virtual void onAttached(RTMPMediaStream*) {}
	virtual bool onMediaFrame(DWORD,RTMPMediaFrame*) { frames++; return true; }
	virtual void onMetaData(DWORD,RTMPMetaData*) {}
	virtual void onCommand(DWORD,const wchar_t*,AMFData*) {}
	virtual void onStreamBegin(DWORD) {}
	virtual void onStreamEnd(DWORD) {}
	virtual void onStreamReset(DWORD) {}
	virtual void onDetached(RTMPMediaStream*) {}

	std::atomic<int> frames{0};
};

class ConnectionSpy : public RTMPConnection::Listener
{
public:
	virtual std::shared_ptr<RTMPNetConnection> OnConnect(const std::wstring&,RTMPNetConnection::Listener*)
	{
		return nullptr;
	}
	virtual void onDisconnect(RTMPConnection*) {}
};

void FillAudio(RTMPAudioFrame& frame)
{
	BYTE payload[32] = {0};
	frame.SetAudioCodec(RTMPAudioFrame::SPEEX);
	frame.SetSoundRate(RTMPAudioFrame::RATE11khz);
	frame.SetAudioFrame(payload,sizeof(payload));
}

// Une intra fait appeler Clear() par SendMediaFrame : c'est ce qui borne le
// cache pendant le test, et c'est le second écrivain de `cached`.
void FillVideoIntra(RTMPVideoFrame& frame)
{
	BYTE payload[32] = {0};
	frame.SetVideoCodec(RTMPVideoFrame::AVC);
	frame.SetFrameType(RTMPVideoFrame::INTRA);
	frame.SetAVCType(RTMPVideoFrame::AVCNALU);
	frame.SetVideoFrame(payload,sizeof(payload));
}

const int   kRounds     = 400;
const DWORD kInsertions = 3000;

} // namespace

/* ========================================================================== *
 *            RTMPCachedPipedMediaStream : le cache de trames                 *
 * ========================================================================== */

// Le producteur étend `cached` (push_back) et le vide une fois sur vingt
// (Clear, sur intra) ; le spectateur le rejoue sous le verrou lecteur en
// rejoignant. Les trois chemins doivent parler du meme verrou.
TEST(RtmpCacheRace, UnAbonneRejoueLeCachePendantQuUneTrameLEtend)
{
	RTMPCachedPipedMediaStream stream;
	std::atomic<bool> stop{false};
	std::atomic<int>  produced{0};

	std::thread producer([&] {
		int n = 0;
		while (!stop)
		{
			if ((n % 20) == 19)
			{
				RTMPVideoFrame frame(n,32);
				FillVideoIntra(frame);
				stream.SendMediaFrame(&frame);
			}
			else
			{
				RTMPAudioFrame frame(n,32);
				FillAudio(frame);
				stream.SendMediaFrame(&frame);
			}
			n++;
			produced++;
		}
	});

	StubStreamListener listener;
	for (int i = 0; i < kRounds; ++i)
	{
		stream.AddMediaListener(&listener);
		stream.RemoveMediaListener(&listener);
	}

	stop = true;
	producer.join();

	EXPECT_GT(produced.load(), 0) << "le producteur n'a envoye aucune trame";
	// Le rejeu n'est pas garanti a chaque tour (le cache peut venir d'etre
	// vide par une intra), mais il doit avoir eu lieu.
	EXPECT_GT(listener.frames.load(), 0) << "aucun rejeu du cache observe";
}

/* ========================================================================== *
 *              RTMPConnection : la map des chunk streams                     *
 * ========================================================================== */

// onMediaFrame insere pour tout streamId jamais vu (chkid = 4 + 2*streamId),
// et onMetaData ecrit dans la cle fixe 3. Les deux tournent en parallele, comme
// le thread media et le thread de lecture d'une connexion reelle.
//
// Le streamId doit croitre SANS retour en arriere : la map ne se vide jamais,
// donc un streamId deja vu ne fait plus qu'un find, et il n'y a plus rien a
// croiser. Les deux boucles demarrent ensemble et le lecteur tourne aussi
// longtemps que l'inserteur, faute de quoi elles ne se recouvrent pas.
TEST(RtmpChunkMapRace, LesEnvoisSurClesFixesNeCourentPasContreUneInsertion)
{
	int fds[2];
	ASSERT_EQ(socketpair(AF_UNIX,SOCK_STREAM,0,fds),0);

	ConnectionSpy spy;
	RTMPConnection connection(&spy);
	ASSERT_EQ(connection.Init(fds[0]),1);

	std::atomic<bool> go{false};
	std::atomic<bool> inserting{true};
	std::atomic<int>  inserted{0};

	// streamId DECROISSANT : les chkid croissants n'inserent qu'a droite de
	// l'arbre, et le rebalancement ne touche alors presque jamais les noeuds
	// que la lecture de la cle 3 traverse. En descendant, chaque insertion se
	// fait du cote lu.
	std::thread inserter([&] {
		while (!go)
			std::this_thread::yield();
		for (DWORD streamId = kInsertions; streamId >= 1; --streamId)
		{
			RTMPAudioFrame frame(streamId,32);
			FillAudio(frame);
			connection.onMediaFrame(streamId,&frame);
			inserted++;
		}
		inserting = false;
	});

	// Deux lecteurs non verrouilles plutot qu'un : la fenetre est etroite.
	std::thread reader([&] {
		while (!go)
			std::this_thread::yield();
		while (inserting)
		{
			RTMPMetaData meta(0);
			connection.onMetaData(0,&meta);
		}
	});

	go = true;
	int written = 0;
	while (inserting)
	{
		RTMPMetaData meta(written);
		connection.onMetaData(0,&meta);
		written++;
	}

	reader.join();
	inserter.join();

	EXPECT_EQ(inserted.load(), (int)kInsertions);
	EXPECT_GT(written, 0) << "aucune ecriture sur la cle fixe pendant les insertions";

	connection.End();
	close(fds[1]);
}
