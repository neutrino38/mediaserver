/* 
 * File:   VideoTranscoder.h
 * Author: Sergio
 *
 * Created on 19 de marzo de 2013, 12:32
 */

#ifndef VIDEOTRANSCODER_H
#define	VIDEOTRANSCODER_H
#include "VideoEncoderWorker.h"
#include "VideoDecoderWorker.h"
#include "FrameDecimator.h"
#include <string>

//Transcodeur vidéo d'une jambe JSR-309. Depuis le lot 4 de
//`jsr309_transcode_sans_thread.md`, il est LUI-MÊME le VideoOutput de son
//décodeur : l'image décodée traverse l'encodeur sur le thread de la source,
//sans VideoPipe ni thread entre les deux. Conséquence assumée (§3.3, arbitrée
//le 2026-08-28) : plus d'image dupliquée quand la source se tait — le puits
//reçoit ce que la source envoie, comme en mode pont.
class VideoTranscoder :
	public Joinable,
	public Joinable::Listener,
	public VideoOutput
{


public:
	VideoTranscoder(std::wstring &name);
	virtual ~VideoTranscoder();

	//`allowBridging` : autorise le mode pont, comme AudioTranscoder — quand le
	//puits sait porter le codec qui arrive, le paquet est relayé tel quel au lieu
	//d'être décodé puis ré-encodé. Décidé par paquet, pas par le plan de contrôle.
	int Init(bool adpatative = false, bool allowBridging = false);
	int SetCodec(VideoCodec::Type codec,int mode,int fps,int bitrate,int intraPeriod, Properties & properties);
	int End();

	//Joinable interface
	virtual void AddListener(Joinable::Listener *listener);
	virtual void Update();
	virtual void SetREMB(DWORD estimation);
	//Cible du BWE émetteur LOCAL de la patte émettrice (lot 6.3), estimée sur
	//ses seuls RR quand le pair n'offre pas transport-cc. Sans cette surcharge
	//elle tombait dans le no-op de Joinable : l'estimation existait, l'encodeur
	//ne l'apprenait jamais.
	virtual void SetSenderEstimate(DWORD estimation);
	virtual void RemoveListener(Joinable::Listener *listener);
	//Phase 5 : les bornes négociées de la patte émettrice descendent à l'encodeur.
	virtual void SetNegotiatedCodecProperties(const std::map<int,Properties>& byCodec);

	//Virtuals from VideoOutput : le décodeur nous livre ici ses images. C'est
	//aussi là que se mesure la cadence RÉELLE de la source (§3.6).
	virtual int NextFrame(PictPtr pic);
	virtual int SetVideoSize(int width,int height);

	//Virtuals from Joinable::Listener
	virtual void onRTPPacket(RTPPacket &packet);
	virtual void onResetStream();
	virtual void onEndStream();

	//Attach
	int Attach(const std::shared_ptr<Joinable> & join);
	int Dettach();

	const std::wstring& GetName() { return tag;	}

	//Cadence et période intra réellement appliquées à l'encodeur (§3.6).
	int GetEffectiveFps()		{ return encoder.GetEffectiveFps();		}
	int GetEffectiveIntraPeriod()	{ return encoder.GetEffectiveIntraPeriod();	}
	//Géométrie appliquée à l'encodeur (celle de la source si adaptatif).
	int GetEffectiveWidth()		{ return encoder.GetEffectiveWidth();		}
	int GetEffectiveHeight()	{ return encoder.GetEffectiveHeight();		}
	//Pas de décimation en vigueur : 1 = toutes les images sont encodées, k = une
	//sur k (encodeur trop lent pour la cadence de la source, cf. FrameDecimator).
	int GetDecimationStep()		{ return decimator.GetStep();			}

private:
	//Retire le transcodeur des listeners de la source courante, s'il y est.
	//Appelé par Attach, Dettach et End : en mode pont c'est LUI qui est inscrit
	//comme listener, donc c'est lui qui doit se retirer.
	void UnlistenSource();

	//Demande une intra à la SOURCE (FIR/PLI RTCP via l'endpoint amont), bornée à
	//une par seconde : RequestFPU n'a aucun anti-rebond, et relayer 1:1 les PLI
	//du puits transformerait une rafale aval en tempête de FIR vers la source.
	void RequestSourceFPU();

	//§3.6 — cadence réelle de la source, mesurée sur les horodatages RTP (90 kHz)
	//que le décodeur pose sur chaque image, et NON sur leur heure d'arrivée : la
	//gigue réseau et la latence du thread de démux n'y entrent pas.
	void MeasureFrameRate(DWORD pts);
	//Vide la fenêtre : une pause, un saut de pts ou une nouvelle source ne disent
	//rien de la cadence qui suit.
	void ResetFrameRateWindow();
	//L'image est-elle due ? Borne la cadence de SORTIE à la consigne — c'est ce
	//que faisait le GrabFrame(1/fps) du thread supprimé.
	bool DueForEncoding();
	//Écart moyen entre deux images de la source (µs) sur la fenêtre de mesure,
	//0 tant qu'elle est trop courte. C'est le budget d'une image.
	QWORD SourceFrameBudgetUs() const;
	//Trace du pas de décimation : à chaque changement, et rappel périodique
	//tant que des images sont sautées.
	void LogDecimation(bool changed);
	//Le pas vient de changer : l'encodeur reçoit désormais source/k images par
	//seconde, il doit le savoir TOUT DE SUITE — son budget de débit par image
	//en dépend. Sans attendre l'hystérésis de MeasureFrameRate.
	void ApplyDecimatedFrameRate();

	//Pousse la consigne négociée de la patte émettrice (SetCodec, kbps) vers la
	//source en TMMBR. Appelé au basculement en mode pont et quand SetCodec
	//change la consigne pendant le pont : sans encodeur dans le chemin, seule
	//la source peut respecter la bande passante négociée du puits.
	void PushSourceBitrateLimit();

	//Relaie une limite (bps) du puits vers la source, bornée en haut par la
	//consigne négociée de la patte émettrice et en bas, en transcodage, par le
	//plancher de la sonde. L'amortissement du feedback est celui de la jambe
	//amont (RembThrottler) : ce chemin n'en refait pas un second.
	void RelayToSource(DWORD estimation);

	//Fenêtre de mesure : 30 écarts, soit 1 s à 30 im/s et 2 s à 15 im/s. Assez
	//pour lisser le rendu irrégulier d'un navigateur (Chrome oscille entre 24 et
	//30), assez court pour suivre une vraie bascule.
	static const int	FpsWindow = 30;
	//Au-delà d'une seconde, ce n'est pas un écart de cadence mais une PAUSE
	//(mute vidéo) : compter ce temps ferait tomber la mesure vers 0 et la reprise
	//serait encodée à 1 im/s.
	static const DWORD	FpsPauseTicks = 90000;
	//Une BAISSE de cadence n'est appliquée qu'après 20 s (unités 90 kHz) passées
	//sous l'hystérésis sans interruption ; une hausse s'applique tout de suite.
	static const DWORD	FpsDropHoldTicks = 20*90000;

	VideoEncoderMultiplexerWorker	encoder;
	VideoDecoderJoinableWorker	decoder;
	std::wstring	tag;
	bool		inited;
	int		state;		// 0 = probing, 1 = transcoding, 2 = bridging
	int		recCodec;	// dernier codec entrant observé
	bool		allowBridging;
	//Source écoutée en mode pont (lien retour non possédant, comme
	//VideoDecoderJoinableWorker::joined). Vide en mode transcodage seul : c'est
	//alors le décodeur qui est inscrit auprès de la source, et lui qui se retire.
	std::weak_ptr<Joinable>	joined;
	//Dernière demande d'intra relayée à la source (anti-tempête, cf.
	//RequestSourceFPU) — même borne que lastFPURequest du décodeur.
	timeval		lastSourceFPU;

	//── Mesure de cadence (§3.6) ─────────────────────────────────────────────
	DWORD		lastPts;
	bool		hasLastPts;
	DWORD		gaps[FpsWindow];	// écarts de pts, en unités 90 kHz
	int		gapCount;		// écarts retenus (plein à FpsWindow)
	int		gapIndex;		// tête du tampon circulaire
	QWORD		gapSum;			// somme des écarts retenus
	//Dernière cadence poussée à l'encodeur (0 = aucune) et instant de cette
	//poussée : hystérésis de 25 % et une application au plus toutes les 5 s,
	//chacune coûtant une trame clé.
	int		appliedFps;
	timeval		lastFpsApply;
	//Temps de flux (90 kHz) passé sous l'hystérésis depuis la dernière mesure
	//dans la bande : la baisse n'est poussée qu'à FpsDropHoldTicks.
	DWORD		lowTicks;
	//Instant du dernier encodage (µs) : borne la cadence de sortie à la consigne.
	QWORD		lastEncodedUs;

	//── Décimation (encodeur trop lent pour la source) ───────────────────────
	FrameDecimator	decimator;
	//Compteur d'images reçues du décodeur : l'image n est encodée si n % pas == 0.
	DWORD		frameIndex;
	//Dernier rappel dans le log pendant une décimation (µs, 0 = jamais).
	QWORD		lastDecimationLogUs;
	//Fenêtre de mesure trop courte pour un budget : 5 écarts suffisent pour une
	//moyenne, la décision de réouverture (FpsWindow) reste à 30.
	static const int	BudgetMinGaps = 5;
	//Rappel dans le log toutes les 30 s tant que des images sont sautées.
	static const QWORD	DecimationLogPeriodUs = 30000000;
};

#endif	/* VIDEOTRANSCODER_H */

