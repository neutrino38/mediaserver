/**
 * test_consumer_threads.cpp — cycle de vie des threads consommateurs d'une
 * jambe RTP : AudioStream, TextStream et RTPEndpoint.
 *
 * Ces trois classes portaient un pthread brut créé par createPriorityThread,
 * avec un trampoline statique, un pthread_exit en fin de corps et un
 * pthread_join gardé par l'état de la tâche. Elles portent désormais un
 * std::thread membre, comme VideoStream.
 *
 * Ce que ces tests protègent, et qui n'est pas une préférence de style :
 *  - un std::thread JOIGNABLE qu'on réaffecte appelle std::terminate(), donc
 *    abort() : chaque Start* réaffecte le membre, donc chaque Stop* DOIT
 *    joindre, y compris quand le corps du thread a rendu la main tout seul
 *    (échec d'ouverture de codec) ;
 *  - un arrêt qui ne rend pas la main est le défaut trouvé en recette le
 *    2026-09-02 (drapeau de tâche écrasé au démarrage, join éternel, SIGTERM
 *    sans effet) : on borne donc la durée de l'arrêt ;
 *  - un thread non joint reste comptabilisé par le noyau : le compte de
 *    /proc/self/task doit revenir à son niveau de départ.
 *
 * Ils passent AVANT comme APRÈS la conversion : ce sont des garde-fous, pas
 * la preuve d'un correctif.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <dirent.h>
#include <memory>

#include "log.h"
#include "audiostream.h"
#include "textstream.h"
#include "pipeaudioinput.h"
#include "pipeaudiooutput.h"
#include "pipetextinput.h"
#include "pipetextoutput.h"
#include "../src/jsr309/RTPEndpoint.h"

namespace {

// Threads du processus. Preuve directe et pas chère qu'un cycle n'en laisse
// aucun derrière lui.
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

// Les threads de la plateforme (ffmpeg, ImageMagick) démarrent et s'arrêtent
// tout seuls : on ne compare que ce que NOS cycles ajoutent, et on laisse au
// processus le temps de retomber à son niveau.
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

// Durée d'un appel, en ms. Un arrêt qui ne rend pas la main est un join
// éternel : le test ne doit pas mourir sur un timeout de suite entière.
template <typename F>
long TimedMs(F&& f)
{
	const auto start = std::chrono::steady_clock::now();
	f();
	return (long)std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - start).count();
}

} // namespace

/* ------------------------------------------------------------------------- *
 *                                AudioStream                                *
 * ------------------------------------------------------------------------- */

TEST(ConsumerThreads, AudioStreamRendSesThreadsAChaqueCycle)
{
	auto input  = std::make_shared<PipeAudioInput>();
	auto output = std::make_shared<PipeAudioOutput>(false);
	input->Init(8000);
	output->Init(8000);

	AudioStream stream(NULL);
	if (stream.Init(input, output) <= 0)
		GTEST_SKIP() << "impossible de binder une paire de ports RTP";

	const int before = SettledThreadCount();

	RTPMap map;
	map[0] = AudioCodec::PCMU;
	char ip[] = "127.0.0.1";

	// Trois cycles : c'est la RÉAFFECTATION du membre std::thread au deuxième
	// Start qui tue le processus si le Stop précédent n'a pas joint.
	for (int cycle = 0; cycle < 3; cycle++)
	{
		ASSERT_GT(stream.StartReceiving(map), 0) << "cycle " << cycle;
		ASSERT_GT(stream.StartSending(ip, 41000, map), 0) << "cycle " << cycle;

		const long stopMs = TimedMs([&] {
			stream.StopSending();
			stream.StopReceiving();
		});
		EXPECT_LT(stopMs, kStopBudgetMs)
			<< "cycle " << cycle << " : l'arret n'a pas rendu la main";
	}

	EXPECT_EQ(before, SettledThreadCount())
		<< "un thread de AudioStream n'a pas ete joint";

	stream.End();
}

// Le corps du thread peut rendre la main SEUL, sans que personne n'ait posé
// TaskStopping : SendAudio sort tout de suite si l'encodeur ne s'ouvre pas.
// L'état reste alors hors de TaskIdle et l'ancien join gardé ne s'exécutait
// pas — un std::thread laissé joignable là appellerait std::terminate() à la
// réaffectation suivante. C'est ce que le join inconditionnel couvre.
TEST(ConsumerThreads, AudioStreamJointUnThreadQuiSortDeLuiMeme)
{
	auto input  = std::make_shared<PipeAudioInput>();
	auto output = std::make_shared<PipeAudioOutput>(false);
	input->Init(8000);
	output->Init(8000);

	AudioStream stream(NULL);
	if (stream.Init(input, output) <= 0)
		GTEST_SKIP() << "impossible de binder une paire de ports RTP";

	const int before = SettledThreadCount();

	// Un type de codec qu'aucune fabrique ne connaît, mais présent dans la
	// carte : SetSendingCodec passe, CreateEncoder échoue.
	const AudioCodec::Type bogus = (AudioCodec::Type)200;
	RTPMap map;
	map[100] = bogus;
	Properties none;
	stream.SetAudioCodec(bogus, none);

	char ip[] = "127.0.0.1";
	ASSERT_GT(stream.StartSending(ip, 41004, map), 0);

	const long stopMs = TimedMs([&] { stream.StopSending(); });
	EXPECT_LT(stopMs, kStopBudgetMs) << "l'arret n'a pas rendu la main";

	EXPECT_EQ(before, SettledThreadCount())
		<< "le thread sorti de lui-meme n'a pas ete joint";

	// Et une reprise derrière : StartSending commence par StopSending, donc par
	// un SECOND arrêt sur un thread déjà joint. En pthread_t, ce second
	// pthread_join sur un thread déjà joint était un comportement indéfini ;
	// joinable() le rend simplement faux.
	// Ce que ce test CONSTATE, et qui est un défaut PRÉEXISTANT : l'état reste
	// hors de TaskIdle après une erreur d'initialisation, donc l'émission ne
	// redémarre plus jamais sur ce stream. Il ne crashe pas, il refuse.
	EXPECT_LE(stream.StartSending(ip, 41004, map), 0)
		<< "etat de tache non revenu a TaskIdle : defaut prealable, pas un crash";

	stream.End();
}

/* ------------------------------------------------------------------------- *
 *                                 TextStream                                *
 * ------------------------------------------------------------------------- */

TEST(ConsumerThreads, TextStreamRendSesThreadsAChaqueCycle)
{
	auto input  = std::make_shared<PipeTextInput>();
	auto output = std::make_shared<PipeTextOutput>();
	input->Init();
	output->Init();

	TextStream stream(NULL);
	if (stream.Init(input, output) <= 0)
		GTEST_SKIP() << "impossible de binder une paire de ports RTP";

	const int before = SettledThreadCount();

	RTPMap map;
	map[98] = TextCodec::T140;
	char ip[] = "127.0.0.1";

	for (int cycle = 0; cycle < 3; cycle++)
	{
		ASSERT_GT(stream.StartReceiving(map), 0) << "cycle " << cycle;
		ASSERT_GT(stream.StartSending(ip, 41002, map), 0) << "cycle " << cycle;

		const long stopMs = TimedMs([&] {
			stream.StopSending();
			stream.StopReceiving();
		});
		EXPECT_LT(stopMs, kStopBudgetMs)
			<< "cycle " << cycle << " : l'arret n'a pas rendu la main";
	}

	EXPECT_EQ(before, SettledThreadCount())
		<< "un thread de TextStream n'a pas ete joint";

	stream.End();
}

/* ------------------------------------------------------------------------- *
 *                                RTPEndpoint                                *
 * ------------------------------------------------------------------------- */

TEST(ConsumerThreads, RTPEndpointRendSonThreadDeDemuxAChaqueCycle)
{
	RTPEndpoint endpoint(MediaFrame::Video);
	endpoint.Init();
	if (endpoint.GetLocalPort() <= 0)
		GTEST_SKIP() << "impossible de binder une paire de ports RTP";

	const int before = SettledThreadCount();

	for (int cycle = 0; cycle < 3; cycle++)
	{
		ASSERT_EQ(1, endpoint.StartReceiving()) << "cycle " << cycle;
		EXPECT_TRUE(endpoint.IsReceiving());

		const long stopMs = TimedMs([&] { endpoint.StopReceiving(); });
		EXPECT_LT(stopMs, kStopBudgetMs)
			<< "cycle " << cycle << " : l'arret n'a pas rendu la main";
		EXPECT_FALSE(endpoint.IsReceiving());
	}

	EXPECT_EQ(before, SettledThreadCount())
		<< "le thread de demux n'a pas ete joint";

	endpoint.End();
}

// End() sans StopReceiving : c'est le chemin du destructeur (~RTPEndpoint
// appelle End() si le port est initialisé). Il doit joindre le thread, sinon
// ~std::thread appelle std::terminate().
TEST(ConsumerThreads, RTPEndpointEndJointLeThreadSansStopReceiving)
{
	const int before = SettledThreadCount();

	{
		RTPEndpoint endpoint(MediaFrame::Video);
		endpoint.Init();
		if (endpoint.GetLocalPort() <= 0)
			GTEST_SKIP() << "impossible de binder une paire de ports RTP";

		ASSERT_EQ(1, endpoint.StartReceiving());

		const long endMs = TimedMs([&] { endpoint.End(); });
		EXPECT_LT(endMs, kStopBudgetMs) << "End() n'a pas rendu la main";
	}

	EXPECT_EQ(before, SettledThreadCount())
		<< "le thread de demux a survecu a la destruction de l'endpoint";
}
