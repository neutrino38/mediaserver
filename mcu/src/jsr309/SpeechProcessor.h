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
 *  - le texte sortant d'un port passe par un TextMux (§9), qui recevra au
 *    lot 3 la transcription en seconde source.
 *
 * Fils d'exécution : le décodeur d'un port tourne sur le thread de sa source
 * (l'endpoint) et ne fait que déposer dans un anneau ; le texte entrant de
 * même dans le TextMux de l'autre port. Le tour de 20 ms est le seul à
 * encoder et à émettre. Aucun ne bloque.
 */
#ifndef SPEECHPROCESSOR_H
#define	SPEECHPROCESSOR_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include "config.h"
#include "media.h"
#include "medkit/codecs.h"
#include "worker.h"
#include "Joinable.h"

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

	//Le texte entré sur `fromPort` part vers la sortie de l'autre port.
	void OnTextIn(int fromPort, const std::string& utf8);

	std::wstring			tag;
	std::shared_ptr<Port>		ports[Ports];
	bool				clocked = false;
	std::atomic<bool>		ended{false};
	//Heure du dernier tour : celle du texte entrant, pour que le TextMux ne
	//voie qu'une horloge.
	std::atomic<uint64_t>		lastTickMs{0};
};

#endif	/* SPEECHPROCESSOR_H */
