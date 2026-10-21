/*
 * SpeechProcessor.h — le Joinable JSR-309 qui met la voix et le texte d'un
 * appel entre deux endpoints (SPEC docs/conception/SPEECH-PROCESSOR §4).
 *
 *   Endpoint <==> (port A) SpeechProcessor (port B) <==> Endpoint
 *
 * Deux ports exactement, A (0) et B (1), chacun Joinable pour l'AUDIO et le
 * TEXTE, chaque média facultatif. La vidéo n'y passe pas : elle va
 * directement d'un endpoint à l'autre.
 *
 * Ce que fait ce lot (le 2 du plan, §10.4), sans service speech :
 *  - le relais AUTOMATIQUE : un média entré sur un port sort sur l'autre dès
 *    que les deux bouts sont branchés, sans appel qui l'active ;
 *  - l'audio est TOUJOURS décodé puis réencodé (§4.2) : SSRC et timestamps
 *    continus, un codec par port, le bruit de confort mêlé sans cas
 *    particulier ;
 *  - un tour de 20 ms (§4.3) compose pour chaque port :
 *        mix = relais(entrée de l'autre port) + bruit(port)
 *    à MixRate, en addition saturée ;
 *  - le texte sortant d'un port passe par un TextMux (§9) : source 0 le texte
 *    relayé, source 1 la transcription.
 *
 * La reconnaissance (lot 3, §6) : par port source, une VAD décide de ce qui
 * part au flux asr de speech-server, avec 300 ms de pré-roulement. Le texte
 * reconnu va aux événements et/ou au TextMux d'un port.
 *
 * La synthèse (lot 4, §7) : par port destination, un flux tts dit les Say du
 * contrôleur et/ou la frappe d'un port source. Le PCM reçu va dans un tampon
 * de lecture, que le tour mêle au mix du port, 20 ms à la fois.
 *
 * Fils d'exécution : le décodeur d'un port tourne sur le thread de sa source
 * (l'endpoint) et ne fait que déposer dans un anneau ; le texte entrant de
 * même dans le TextMux de l'autre port ; les rappels du flux asr arrivent sur
 * le réacteur WebSocket et ne font que remplir une file. Le tour de 20 ms est
 * le seul à encoder, à émettre et à publier les événements. Aucun ne bloque.
 */
#ifndef SPEECHPROCESSOR_H
#define	SPEECHPROCESSOR_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include "config.h"
#include "media.h"
#include "medkit/codecs.h"
#include "worker.h"
#include "Joinable.h"
#include "JSR309Event.h"

class SpeechService;

class SpeechProcessor : public Worker
{
public:
	static constexpr int	Ports = 2;
	//Fréquence du mix, jamais écrite en dur dans les calculs : elle passera à
	//48 kHz si la recette le demande (§4.2).
	static constexpr DWORD	MixRate = 16000;
	static constexpr int	FrameMs = 20;
	static constexpr int	FrameSamples = MixRate*FrameMs/1000;
	//Anneau du relais : au plus 60 ms, le plus ancien est jeté au-delà, pour
	//que la latence ne dérive pas quand les horloges des pairs divergent. Le
	//relais ne démarre qu'avec 40 ms d'avance, pour absorber la gigue.
	static constexpr int	RelayMaxMs = 60;
	static constexpr int	RelayStartMs = 40;
	//Le T.140 part par paquets espacés de 300 ms au plus (RFC 4103 §5).
	static constexpr int	TextBufferMs = 300;
	static constexpr int	DefaultComfortNoiseDbov = -65;
	static constexpr int	MinComfortNoiseDbov = -96;
	//Reconnaissance (§6) : ce qui précède la parole détectée, le silence qui
	//met le flux en pause, le regroupement des deltas, la fin d'énoncé.
	static constexpr int	PreRollMs = 300;
	//La parole n'est déclarée qu'après OnsetMs de voix, coupée de moins de
	//OnsetGapMs : un souffle ou un clic ne coupe pas la synthèse (barge-in).
	static constexpr int	OnsetMs = 250;
	static constexpr int	OnsetGapMs = 100;
	static constexpr int	SilenceMs = 700;
	static constexpr int	PartialEventMs = 250;
	static constexpr int	UtteranceEndMs = 500;
	//Synthèse (§7.5) : une phrase corrigée est redite quand la ligne n'a pas
	//changé depuis ce délai.
	static constexpr int	CorrectionStableMs = 2000;

	SpeechProcessor(const std::wstring& tag);
	virtual ~SpeechProcessor();

	//clocked=false : pas de thread, c'est l'appelant qui bat Tick() (tests).
	int Init(bool clocked = true);
	//Idempotent : arrête le tour, détache les entrées, arrête les encodeurs.
	int End();

	static bool IsValidPort(int port)		{ return port >= 0 && port < Ports; }
	static bool IsSupportedMedia(MediaFrame::Type media)
	{
		return media == MediaFrame::Audio || media == MediaFrame::Text;
	}

	const std::wstring& GetTag() const		{ return tag; }

	//À poser avant Init(). nullptr : pas de reconnaissance ni de synthèse.
	void SetSpeechService(SpeechService* speech)	{ this->speech = speech; }
	bool HasSpeechService() const			{ return speech != nullptr; }
	//Prend possession de l'événement ; appelé sur le thread du tour.
	void SetEventPoster(std::function<void(JSR309Event*)> poster)	{ this->poster = poster; }

	//La voix entrée sur `port` est transcrite vers les événements et/ou vers
	//la sortie texte de `textPort` (-1 : aucune). Rappeler remplace le
	//réglage, et rouvre le flux.
	int StartRecognition(int port, bool toEvents, int textPort, bool halfDuplex);
	int StopRecognition(int port);

	//Le flux tts du port destination `port` dit les Say et, si textSourcePort
	//vaut 0 ou 1, la frappe de ce port. voice : alias de speech-server, vide
	//pour le défaut. Rappeler remplace le réglage, et rouvre le flux.
	int StartSynthesis(int port, int textSourcePort, const std::string& voice, bool announceCorrections);
	int StopSynthesis(int port);
	bool IsSynthesisActive(int port);
	//Rend le sayId (> 0), 0 si la synthèse n'est pas active ou s'il n'y a rien
	//à dire.
	int Say(int port, const std::string& text);
	int StopSpeaking(int port);

	int SetPortCodec(int port, AudioCodec::Type codec);
	//dbov <= 0 ; 0 = silence exact.
	int SetComfortNoise(int port, int dbov);

	//Ce que l'endpoint écoute pour RECEVOIR du port : vue aliasing sur le port,
	//pour que son weak_ptr expire avec le SpeechProcessor (lien A). nullptr si
	//le port ou le média est invalide.
	std::shared_ptr<Joinable> GetJoinable(int port, MediaFrame::Type media);
	//Le port reçoit le média de `join`.
	int Attach(int port, MediaFrame::Type media, const std::shared_ptr<Joinable>& join);
	int Dettach(int port, MediaFrame::Type media);

	//Un tour de mix et d'émission.
	void Tick(uint64_t nowMs);

	//Audio en attente dans l'anneau d'entrée du port, en ms (tests).
	int GetRelayBufferedMs(int port);

	static uint64_t NowMs();

protected:
	virtual int Run() override;

private:
	class Port;
	friend class Port;
	class Recognizer;
	class Synthesizer;

	void Recognize(Recognizer& rec, const SWORD* frame, int samples, uint64_t nowMs);
	void Synthesize(Synthesizer& synth, uint64_t nowMs);
	bool SendPhrases(Synthesizer& synth, int sayId, const std::string& text);
	void SendHeld(Synthesizer& synth);
	void DropHeld(Synthesizer& synth);
	void StopSynthesizer(Synthesizer& synth);
	std::shared_ptr<Synthesizer> GetSynthesizer(int port);
	void CloseUtterance(Recognizer& rec);
	void Post(JSR309Event* event);

	//Le texte entré sur `fromPort` part vers la sortie de l'autre port.
	void OnTextIn(int fromPort, const std::string& utf8);

	std::wstring			tag;
	std::shared_ptr<Port>		ports[Ports];
	bool				clocked = false;
	std::atomic<bool>		ended{false};
	SpeechService*			speech = nullptr;
	std::function<void(JSR309Event*)> poster;
	std::atomic<int>		nextSayId{1};
	//Heure du dernier tour : celle du texte entrant, pour que le TextMux ne
	//voie qu'une horloge.
	std::atomic<uint64_t>		lastTickMs{0};
};

/**
 * Événements d'un SpeechProcessor (§8.4). Sérialisation XML-RPC :
 * (isiii…) = {type, sessionTag, spId, media, port, …}. spId vient du contexte
 * d'événement, media et port de l'événement lui-même.
 */
class SpeechEvent : public JSR309Event
{
public:
	SpeechEvent(int type, MediaFrame::Type eventMedia, int port) :
		type(type), eventMedia(eventMedia), port(port) {}

	virtual xmlrpc_value* GetXmlValue(xmlrpc_env *env);

protected:
	//Les champs propres à l'événement, à la suite du préfixe commun.
	virtual xmlrpc_value* GetTail(xmlrpc_env *env) = 0;

	int			type;
	MediaFrame::Type	eventMedia;
	int			port;
};

//8 : (… , s text, b final). Le port est celui de la voix.
class SpeechRecognizedEvent : public SpeechEvent
{
public:
	SpeechRecognizedEvent(int port, const std::string& text, bool final) :
		SpeechEvent(JSR309Event::SpeechRecognizedEvent, MediaFrame::Text, port), text(text), final(final) {}
protected:
	virtual xmlrpc_value* GetTail(xmlrpc_env *env);
	std::string	text;
	bool		final;
};

//9 : (… , i sayId). sayId 0 : une phrase tapée sur le port source.
class SpeechSynthesisStartedEvent : public SpeechEvent
{
public:
	SpeechSynthesisStartedEvent(int port, int sayId) :
		SpeechEvent(JSR309Event::SpeechSynthesisStartedEvent, MediaFrame::Audio, port), sayId(sayId) {}
protected:
	virtual xmlrpc_value* GetTail(xmlrpc_env *env);
	int		sayId;
};

//10 : (… , i sayId, b interrupted).
class SpeechSynthesisDoneEvent : public SpeechEvent
{
public:
	SpeechSynthesisDoneEvent(int port, int sayId, bool interrupted) :
		SpeechEvent(JSR309Event::SpeechSynthesisDoneEvent, MediaFrame::Audio, port), sayId(sayId), interrupted(interrupted) {}
protected:
	virtual xmlrpc_value* GetTail(xmlrpc_env *env);
	int		sayId;
	bool		interrupted;
};

//11 : (… , b speaking).
class SpeechActivityEvent : public SpeechEvent
{
public:
	SpeechActivityEvent(int port, bool speaking) :
		SpeechEvent(JSR309Event::SpeechActivityEvent, MediaFrame::Audio, port), speaking(speaking) {}
protected:
	virtual xmlrpc_value* GetTail(xmlrpc_env *env);
	bool		speaking;
};

//12 : (… , s function, s state, s reason). state vaut "up" ou "down".
class SpeechServiceEvent : public SpeechEvent
{
public:
	SpeechServiceEvent(int port, const std::string& function, bool up, const std::string& reason) :
		SpeechEvent(JSR309Event::SpeechServiceEvent, MediaFrame::Audio, port), function(function), up(up), reason(reason) {}
protected:
	virtual xmlrpc_value* GetTail(xmlrpc_env *env);
	std::string	function;
	bool		up;
	std::string	reason;
};

#endif	/* SPEECHPROCESSOR_H */
