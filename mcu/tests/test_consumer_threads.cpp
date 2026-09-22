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

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <ctime>
#include <thread>
#include <dirent.h>
#include <memory>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "log.h"
#include "audiostream.h"
#include "textencoder.h"
#include "participanttextws.h"
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

// CPU consommé par le processus, tous threads confondus, en ms. Une boucle qui
// tourne à vide se voit ici et nulle part ailleurs : elle n'a aucun effet
// observable, elle brûle juste un cœur.
long ProcessCpuMs()
{
	return (long)(clock() * 1000.0 / CLOCKS_PER_SEC);
}

// Budget CPU d'une boucle au repos pendant `kIdleWindowMs`. Une boucle chaude
// en consomme la fenêtre entière.
const int kIdleWindowMs = 300;
const int kIdleCpuBudgetMs = 100;

// Durée d'un appel, en ms.
template <typename F>
long TimedMs(F&& f)
{
	const auto start = std::chrono::steady_clock::now();
	f();
	return (long)std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - start).count();
}

// Même mesure, BORNÉE. Mesurer un arrêt en ligne ne protège que d'un arrêt
// LENT : un arrêt qui ne rend JAMAIS la main — le join éternel du 2026-09-02 —
// fige la suite entière avant d'avoir pu être comparé à quoi que ce soit. Le
// gel a été observé ici le 2026-09-22 : boucle chaude sur un cœur, thread
// principal dans le join, aucune sortie pendant 18 minutes.
//
// L'appel part donc dans un thread à part. Au dépassement, rend -1 et ABANDONNE
// ce thread, qui tourne toujours : l'appelant DOIT alors fuir l'objet au lieu
// de le détruire, et `f` ne doit capturer que des choses qui survivront au test.
template <typename F>
long BoundedMs(F f, long budgetMs)
{
	std::shared_ptr< std::atomic<long> > done = std::make_shared< std::atomic<long> >(-1);
	std::thread worker([f, done]() mutable { done->store(TimedMs(f)); });

	const auto deadline = std::chrono::steady_clock::now()
			    + std::chrono::milliseconds(budgetMs);
	while (done->load() < 0 && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(2));

	if (done->load() < 0)
	{
		worker.detach();
		return -1;
	}
	worker.join();
	return done->load();
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

	// Et une reprise derrière, avec un codec valide : elle doit MARCHER.
	// L'état de tâche revient à TaskIdle sur la sortie d'erreur du corps, comme
	// sur sa sortie de boucle. Sans ça, StartSending — qui exige TaskIdle —
	// refusait « bad state » à vie sur ce stream : une seule erreur d'ouverture
	// de codec condamnait l'émission du participant.
	RTPMap good;
	good[0] = AudioCodec::PCMU;
	stream.SetAudioCodec(AudioCodec::PCMU, none);

	EXPECT_GT(stream.StartSending(ip, 41004, good), 0)
		<< "l'emission ne redemarre pas apres une erreur d'ouverture de codec";
	stream.StopSending();

	EXPECT_EQ(before, SettledThreadCount());

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

/* ------------------------------------------------------------------------- *
 *                     Cadence de la boucle d'émission texte                 *
 * ------------------------------------------------------------------------- */

// Sonde UDP : compte les datagrammes reçus pendant une fenêtre.
class UdpProbe
{
public:
	~UdpProbe() { if (fd >= 0) close(fd); }

	bool Open()
	{
		fd = socket(AF_INET, SOCK_DGRAM, 0);
		if (fd < 0)
			return false;
		sockaddr_in addr = {};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = 0;
		if (bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0)
			return false;
		socklen_t len = sizeof(addr);
		if (getsockname(fd, (sockaddr*)&addr, &len) < 0)
			return false;
		port = ntohs(addr.sin_port);
		return true;
	}

	int Port() const { return port; }

	// Compte les datagrammes PORTEURS D'UNE CHARGE arrivés pendant `windowMs`,
	// en s'arrêtant à `max`. Les datagrammes d'en-tête seul (12 octets) sont
	// ignorés : `RTPSession::SetRemotePort` en émet une rafale pour faire
	// latcher un pair symétrique (`ArmNATPriming`), et ils ne viennent pas de
	// la boucle d'émission.
	int CountFor(int windowMs, int max)
	{
		int n = 0;
		const auto deadline = std::chrono::steady_clock::now()
			+ std::chrono::milliseconds(windowMs);
		while (n < max)
		{
			const auto now = std::chrono::steady_clock::now();
			if (now >= deadline)
				break;
			const int leftMs = (int)std::chrono::duration_cast<
				std::chrono::milliseconds>(deadline - now).count();
			pollfd p = { fd, POLLIN, 0 };
			if (poll(&p, 1, leftMs) <= 0)
				break;
			BYTE buffer[2048];
			const int len = recv(fd, buffer, sizeof(buffer), 0);
			if (len <= 0)
				break;
			if (len > RtpHeaderLen)
				n++;
		}
		return n;
	}

private:
	static const int RtpHeaderLen = 12;

	int fd   = -1;
	int port = 0;
};

// Le pipe du mixeur n'est PAS inité tant que le participant n'y est pas
// rattaché — et `TextInput::GetFrame` n'honore son timeout que s'il l'est :
// sinon il rend NULL sans attendre. La boucle d'émission émettait alors un
// keep-alive T.140 par tour, freinée par le seul `msleep(200)` — qui vaut 200
// MICROsecondes : environ 5000 paquets par seconde sur le réseau.
TEST(SendLoopPacing, UnPipeNonIniteNInondePasLeReseau)
{
	UdpProbe probe;
	if (!probe.Open())
		GTEST_SKIP() << "socket loopback indisponible";

	auto input  = std::make_shared<PipeTextInput>();
	auto output = std::make_shared<PipeTextOutput>();
	// Volontairement PAS d'Init() sur l'entrée : c'est le cas reproduit.
	output->Init();

	TextStream stream(NULL);
	if (stream.Init(input, output) <= 0)
		GTEST_SKIP() << "impossible de binder une paire de ports RTP";

	RTPMap map;
	map[98] = TextCodec::T140;
	char ip[] = "127.0.0.1";
	ASSERT_GT(stream.StartSending(ip, probe.Port(), map), 0);

	// 300 ms : de quoi voir passer des centaines de paquets si la cadence est
	// perdue. L'intervalle de keep-alive, lui, est de 25 s.
	const int seen = probe.CountFor(300, 200);
	EXPECT_EQ(0, seen) << "keep-alive emis en boucle : la cadence n'est pas tenue";

	// Et l'arrêt ne doit pas attendre la fin de l'intervalle de keep-alive :
	// l'attente est annulable, un msleep ne l'était pas.
	const long stopMs = TimedMs([&] { stream.StopSending(); });
	EXPECT_LT(stopMs, kStopBudgetMs) << "l'arret attend la fin du keep-alive";

	stream.End();
}

// Le garde-fou de l'intention : l'attente ajoutée ne doit pas retenir le texte.
// Une trame écrite dans un pipe inité part tout de suite.
TEST(SendLoopPacing, UneTrameEcriteParImmediatement)
{
	UdpProbe probe;
	if (!probe.Open())
		GTEST_SKIP() << "socket loopback indisponible";

	auto input  = std::make_shared<PipeTextInput>();
	auto output = std::make_shared<PipeTextOutput>();
	input->Init();
	output->Init();

	TextStream stream(NULL);
	if (stream.Init(input, output) <= 0)
		GTEST_SKIP() << "impossible de binder une paire de ports RTP";

	RTPMap map;
	map[98] = TextCodec::T140;
	char ip[] = "127.0.0.1";
	ASSERT_GT(stream.StartSending(ip, probe.Port(), map), 0);

	input->WriteText(L"bonjour");

	EXPECT_EQ(1, probe.CountFor(kStopBudgetMs, 1))
		<< "la trame n'est pas partie";

	stream.StopSending();
	stream.End();
}

// `TextEncoder::Encode` attend ses trames avec `GetFrame(0)`, donc INDÉFINIMENT
// — tant que le pipe est inité. Un pipe que le mixeur texte a terminé rend NULL
// tout de suite, et la boucle repartait par `continue` : un cœur à 100 % pour
// rien, sans le moindre effet observable.
TEST(SendLoopPacing, LEncodeurTexteNeTournePasAVide)
{
	PipeTextInput input;	// volontairement PAS Init() : le cas reproduit

	TextEncoder encoder;
	ASSERT_GT(encoder.Init(&input), 0);
	ASSERT_GT(encoder.StartEncoding(), 0);

	const long cpuBefore = ProcessCpuMs();
	std::this_thread::sleep_for(std::chrono::milliseconds(kIdleWindowMs));
	const long cpuUsed = ProcessCpuMs() - cpuBefore;

	const long stopMs = TimedMs([&] { encoder.StopEncoding(); });

	EXPECT_LT(cpuUsed, kIdleCpuBudgetMs)
		<< "boucle a vide : " << cpuUsed << " ms de CPU pour "
		<< kIdleWindowMs << " ms d'attente";
	EXPECT_LT(stopMs, kStopBudgetMs) << "l'arret n'a pas rendu la main";

	encoder.End();
}

// Même défaut dans le pont texte WebSocket, qui tire le mix de sa jambe avec
// `GetFrame(10000)`.
TEST(SendLoopPacing, LeTirageWebSocketNeTournePasAVide)
{
	auto input  = std::make_shared<PipeTextInput>();
	auto output = std::make_shared<PipeTextOutput>();
	// Ni l'un ni l'autre inité : le pont peut démarrer avant le mixeur.

	ParticipantTextWS bridge(input, output);
	ASSERT_GT(bridge.Init(), 0);

	const long cpuBefore = ProcessCpuMs();
	std::this_thread::sleep_for(std::chrono::milliseconds(kIdleWindowMs));
	const long cpuUsed = ProcessCpuMs() - cpuBefore;

	const long endMs = TimedMs([&] { bridge.End(); });

	EXPECT_LT(cpuUsed, kIdleCpuBudgetMs)
		<< "boucle a vide : " << cpuUsed << " ms de CPU pour "
		<< kIdleWindowMs << " ms d'attente";
	EXPECT_LT(endMs, kStopBudgetMs) << "End() n'a pas rendu la main";
}

// Init() puis End() sans laisser au thread le temps de démarrer : le corps
// posait `pulling = TaskRunning` sans garde, écrasant le `TaskStopping` que
// End() venait d'écrire — la boucle ne sortait plus et StopThread() joignait à
// vie. Vingt cycles, chacun borné.
//
// Le pont est sur le TAS, et il n'est pas détruit si un End() dépasse : son
// thread tourne encore, et écrirait dans un objet libéré. On fuit l'objet — il
// détient une copie des deux pipes, qui lui survivent donc aussi — et on échoue.
TEST(SendLoopPacing, LeTirageWebSocketSArreteMemeAPeineDemarre)
{
	auto input  = std::make_shared<PipeTextInput>();
	auto output = std::make_shared<PipeTextOutput>();
	input->Init();
	output->Init();

	ParticipantTextWS* bridge = new ParticipantTextWS(input, output);

	for (int cycle = 0; cycle < 20; cycle++)
	{
		ASSERT_GT(bridge->Init(), 0) << "cycle " << cycle;

		const long endMs = BoundedMs([bridge] { bridge->End(); }, kStopBudgetMs);
		if (endMs < 0)
			FAIL() << "cycle " << cycle << " : End() n'a jamais rendu la main";

		EXPECT_LT(endMs, kStopBudgetMs) << "cycle " << cycle << " : arret trop lent";
	}

	delete bridge;
}

/* ------------------------------------------------------------------------- *
 *                    Réveil d'un lecteur de pipe texte                      *
 * ------------------------------------------------------------------------- */

// `PipeTextInput::Cancel()` doit réveiller son lecteur, y compris quand il
// arrive pendant que le lecteur ENTRE dans l'attente. Il notifiait hors du
// verrou : le lecteur, qui tient ce verrou et a déjà lu `inited` à true, n'est
// pas encore endormi ; `notify_all` ne trouve aucun waiter et le réveil est
// PERDU. `GetFrame` dort alors son timeout entier — 25 s pour le flux texte
// RTP, 10 s pour le pont WebSocket — et le join de l'arrêt attend avec lui.
// Symptôme observé : un arrêt sur deux cents qui prend l'intervalle complet.
TEST(PipeTextInputWake, UnCancelConcurrentReveilleToujoursLeLecteur)
{
	const long kReaderBudgetMs = 2000;

	// La fenêtre ne dure que quelques instructions : le lecteur annonce qu'il
	// va appeler GetFrame, puis l'annulateur attend un nombre CROISSANT de
	// tours de boucle avant d'annuler. Le balayage traverse ainsi la prise du
	// verrou, la lecture du prédicat et l'endormissement.
	for (int cycle = 0; cycle < 3000; cycle++)
	{
		PipeTextInput pipe;
		pipe.Init();

		std::atomic<bool> ready{false};
		std::atomic<long> waitedMs{-1};
		std::thread reader([&] {
			const auto start = std::chrono::steady_clock::now();
			ready = true;
			TextFrame* frame = pipe.GetFrame(30000);
			waitedMs = (long)std::chrono::duration_cast<
				std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - start).count();
			delete frame;
		});

		while (!ready)
			std::this_thread::yield();
		for (volatile int spin = 0; spin < cycle; spin++)
			;
		pipe.Cancel();
		reader.join();

		ASSERT_LT(waitedMs.load(), kReaderBudgetMs)
			<< "cycle " << cycle << " : reveil perdu, le lecteur a dormi "
			<< waitedMs.load() << " ms";
	}
}
