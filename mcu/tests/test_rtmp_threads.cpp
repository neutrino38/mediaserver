/**
 * test_rtmp_threads.cpp — cycle de vie des threads des objets RTMP :
 * FLVEncoder, RTMPParticipant et RTMPConnection.
 *
 * Ces trois classes portaient des pthread bruts créés par createPriorityThread,
 * avec un trampoline statique par thread et un pthread_join gardé par un
 * drapeau. Elles portent désormais des std::thread membres, comme VideoStream
 * et AudioStream (cf. test_consumer_threads.cpp, même motif, mêmes raisons).
 *
 * Ce que ces tests protègent :
 *  - réaffecter un std::thread JOIGNABLE appelle std::terminate(), donc
 *    abort() : chaque redémarrage réaffecte le membre, donc chaque arrêt DOIT
 *    joindre, y compris quand le corps est sorti de lui-même ;
 *  - un arrêt qui ne rend pas la main est un join éternel : on borne sa durée ;
 *  - un thread non joint reste comptabilisé par le noyau : le compte de
 *    /proc/self/task doit revenir à son niveau de départ.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <dirent.h>
#include <memory>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "log.h"
#include "FLVEncoder.h"
#include "rtmpconnection.h"
#include "rtmpparticipant.h"
#include "pipeaudioinput.h"
#include "pipetextinput.h"
#include "pipevideoinput.h"

namespace {

int CountThreads()
{
	DIR* dir = opendir("/proc/self/task");
	if (!dir)
		return -1;
	int n = 0;
	while (struct dirent* e = readdir(dir))
		if (e->d_name[0] != '.')
			n++;
	closedir(dir);
	return n;
}

// Les threads de la plateforme (ffmpeg, ImageMagick) vont et viennent seuls :
// on attend que le compte se stabilise avant de le comparer.
int SettledThreadCount()
{
	int last = CountThreads();
	for (int i = 0; i < 20; i++)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(25));
		const int now = CountThreads();
		if (now == last)
			return now;
		last = now;
	}
	return last;
}

const int kStopBudgetMs = 3000;

template <typename F>
long TimedMs(F&& f)
{
	const auto start = std::chrono::steady_clock::now();
	f();
	return (long)std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - start).count();
}

// Écoute des événements d'une connexion RTMP. onDisconnect NE DOIT PAS joindre
// : il est appelé DEPUIS le thread de lecture, et joindre son propre thread
// lève std::system_error. Le vrai RTMPServer met la connexion de côté et la
// termine depuis son thread d'acceptation ; on note juste le passage.
class ConnectionSpy : public RTMPConnection::Listener
{
public:
	virtual std::shared_ptr<RTMPNetConnection> OnConnect(const std::wstring& appName,RTMPNetConnection::Listener *listener)
	{
		return nullptr;
	}
	virtual void onDisconnect(RTMPConnection* con) { disconnected++; }
	int disconnected = 0;
};

} // namespace

/* ------------------------------------------------------------------------- *
 *                                 FLVEncoder                                 *
 * ------------------------------------------------------------------------- */

TEST(RtmpThreads, FLVEncoderRendSesDeuxThreadsAChaqueCycle)
{
	PipeAudioInput audio;
	PipeVideoInput video;
	PipeTextInput  text;
	audio.Init(8000);
	video.Init();
	text.Init();

	FLVEncoder encoder;
	ASSERT_EQ(encoder.Init(&audio,&video,&text), 1);

	// Un cycle à blanc AVANT la mesure : l'ouverture d'un encodeur crée des
	// threads de plateforme (ffmpeg) qui, eux, ne sont créés qu'une fois et
	// vivent jusqu'à la fin du processus. Sans ce préchauffage, le test
	// compte ces threads-là dès qu'il est joué en premier.
	encoder.StartEncoding();
	encoder.StopEncoding();

	const int before = SettledThreadCount();

	// Trois cycles : c'est la RÉAFFECTATION du membre std::thread au deuxième
	// StartEncoding qui tue le processus si le StopEncoding précédent n'a pas
	// joint.
	for (int cycle = 0; cycle < 3; cycle++)
	{
		ASSERT_EQ(encoder.StartEncoding(), 1) << "cycle " << cycle;

		const long stopMs = TimedMs([&] { encoder.StopEncoding(); });
		EXPECT_LT(stopMs, kStopBudgetMs)
			<< "cycle " << cycle << " : l'arret n'a pas rendu la main";
	}

	EXPECT_EQ(before, SettledThreadCount())
		<< "un thread de FLVEncoder n'a pas ete joint";

	encoder.End();
	audio.End();
	video.End();
	text.End();
}

/* ------------------------------------------------------------------------- *
 *                              RTMPParticipant                               *
 * ------------------------------------------------------------------------- */

TEST(RtmpThreads, RTMPParticipantRendSesThreadsDEmissionAChaqueCycle)
{
	auto audio = std::make_shared<PipeAudioInput>();
	auto video = std::make_shared<PipeVideoInput>();
	auto text  = std::make_shared<PipeTextInput>();
	audio->Init(8000);
	video->Init();
	text->Init();

	RTMPParticipant part(1);
	part.SetAudioInput(audio);
	part.SetVideoInput(video);
	part.SetTextInput(text);
	Properties none;
	part.SetAudioCodec(AudioCodec::PCMU,none);
	part.SetVideoCodec(VideoCodec::H264,CIF,15,256,600,none);
	ASSERT_TRUE(part.Init());

	//Cycle à blanc, même raison que pour FLVEncoder.
	part.StartSending();
	part.StopSending();

	const int before = SettledThreadCount();

	for (int cycle = 0; cycle < 3; cycle++)
	{
		ASSERT_TRUE(part.StartSending()) << "cycle " << cycle;

		const long stopMs = TimedMs([&] { part.StopSending(); });
		EXPECT_LT(stopMs, kStopBudgetMs)
			<< "cycle " << cycle << " : l'arret n'a pas rendu la main";
	}

	EXPECT_EQ(before, SettledThreadCount())
		<< "un thread d'emission de RTMPParticipant n'a pas ete joint";

	part.End();
	audio->End();
	video->End();
	text->End();
}

/* ------------------------------------------------------------------------- *
 *                              RTMPConnection                                *
 * ------------------------------------------------------------------------- */

// Init() démarre les deux threads (lecture et écriture) et End() les joint.
// Un socketpair suffit : le pair n'envoie jamais rien, donc la lecture reste
// dans son poll jusqu'à ce que Stop() ferme le descripteur.
TEST(RtmpThreads, RTMPConnectionJointSesDeuxThreadsAChaqueCycle)
{
	ConnectionSpy spy;
	RTMPConnection connection(&spy);

	//Cycle à blanc, même raison que pour FLVEncoder.
	{
		int warm[2];
		ASSERT_EQ(socketpair(AF_UNIX,SOCK_STREAM,0,warm), 0);
		connection.Init(warm[0]);
		connection.End();
		close(warm[1]);
	}

	const int before = SettledThreadCount();

	for (int cycle = 0; cycle < 3; cycle++)
	{
		int fds[2];
		ASSERT_EQ(socketpair(AF_UNIX,SOCK_STREAM,0,fds), 0);

		ASSERT_EQ(connection.Init(fds[0]), 1) << "cycle " << cycle;

		const long endMs = TimedMs([&] { connection.End(); });
		EXPECT_LT(endMs, kStopBudgetMs)
			<< "cycle " << cycle << " : l'arret n'a pas rendu la main";

		close(fds[1]);
	}

	EXPECT_EQ(before, SettledThreadCount())
		<< "un thread de RTMPConnection n'a pas ete joint";
}
