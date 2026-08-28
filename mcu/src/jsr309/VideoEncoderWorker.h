/* 
 * File:   VideoEncoderWorker.h
 * Author: Sergio
 *
 * Created on 2 de noviembre de 2011, 23:37
 */

#ifndef VIDEOENCODERWORKER_H
#define	VIDEOENCODERWORKER_H


#include <atomic>
#include <mutex>
#include "config.h"
#include "acumulator.h"
#include "medkit/codecs.h"
#include "medkit/videorescaler.h"
#include "video.h"
#include "RTPMultiplexerSmoother.h"

//Encodeur vidéo d'une patte émettrice JSR-309. UN SEUL corps de traitement,
//`EncodePicture`, et deux façons de l'alimenter (lot 4 de
//`jsr309_transcode_sans_thread.md`) :
//  - TIRÉ (port de mixeur, `Init(VideoInput*)`) : un thread échantillonne le
//    mixeur à `fps` — c'est le seul composant réellement CADENCÉ du chantier,
//    et il garde donc sa boucle ;
//  - POUSSÉ (transcodeur, `Init()`) : le décodeur livre son image et elle est
//    encodée sur le thread de la source, sans file ni thread. Le puits ne reçoit
//    alors plus rien tant que la source se tait — plus d'image dupliquée à
//    cadence constante (§3.3, arbitré le 2026-08-28).
class VideoEncoderMultiplexerWorker :
	public RTPMultiplexerSmoother
{
public:
	VideoEncoderMultiplexerWorker();
	virtual ~VideoEncoderMultiplexerWorker();

	//Mode TIRÉ : le worker échantillonne `input` à `fps`.
	int Init(VideoInput *input);
	//Mode POUSSÉ : les images arrivent par EncodePicture, sans thread.
	int Init();
	int SetCodec(VideoCodec::Type codec,int mode,int fps,int bitrate,int intraPeriod, Properties & properties);
	int End();
	void UseInputSize(bool use) { useInputSize = use; }

	//Encode une image et l'étale dans le lisseur. Rend 1 si une trame a été
	//produite, 0 sinon. Corps commun aux deux modes.
	int EncodePicture(PictPtr pic);

	//Mode poussé : taille NATIVE du producteur, que le transcodeur relaie depuis
	//son décodeur (VideoOutput::SetVideoSize). En mode tiré, c'est le VideoInput
	//qui la porte (HasNativeSizeChanged) et cet appel n'a pas lieu.
	void SetNativeSize(DWORD width,DWORD height);

	//§3.6 : cadence RÉELLE des images offertes à l'encodeur, mesurée par le
	//transcodeur sur les horodatages RTP de la source. 0 = aucune mesure. Elle
	//ne peut que BAISSER la consigne, jamais la dépasser : `fps` est une borne
	//du contrôleur (et du niveau AV1).
	void SetMeasuredFrameRate(int fps);

	//Joinable interface
	virtual void AddListener(Listener *listener);
	virtual void Update();
	virtual void SetREMB(int bitrate);
	virtual void SetSenderEstimate(DWORD bitrate);
	virtual void RemoveListener(Listener *listener);
	//Phase 5 (nego_fmtp §6.3) : bornes négociées par code codec, fusionnées
	//par-dessus `params` à l'ouverture de l'encodeur.
	//
	//Encodeur EN MARCHE : les bornes sont mémorisées et un drapeau est levé — le
	//chemin des paquets les applique lui-même en jetant son encodeur. PAS de
	//Stop/Start ici : cet appel arrive du thread XML-RPC, qui tient le verrou de la
	//MediaSession, et joindre le thread d'encodage sous ce verrou a gelé le serveur
	//entier le 2026-08-13 (le deinit de SVT-AV1 0.9.0 ne revenait jamais, et les
	//threads de dispatch s'empilaient derrière le verrou jusqu'à ce que le serveur
	//cesse d'accepter). Le prix du report est un GOP émis avec les bornes
	//précédentes. Depuis le lot 4, SetCodec suit exactement le même motif.
	virtual void SetNegotiatedCodecProperties(const std::map<int,Properties>& byCodec);

	inline VideoCodec::Type GetCodec() { return codec; }
	//Consigne configurée (SetCodec), en kbps ; 0 tant que SetCodec n'a pas été
	//appelé. Sert de borne au relais TMMBR/REMB du mode pont : le puits ne peut
	//pas « autoriser » plus que ce que sa négociation porte.
	inline int GetBitrate() { return bitrate; }
	//Consigne de cadence (SetCodec), en images par seconde ; 0 avant SetCodec.
	//Le transcodeur en a besoin pour borner sa propre cadence de sortie.
	inline int GetConfiguredFps() { return configuredFps; }
	//Cadence et période intra RÉELLEMENT appliquées à l'encodeur (§3.6) : ce
	//qu'un exploitant veut voir, et ce que la recette vérifie. 0 avant Start().
	int GetEffectiveFps();
	int GetEffectiveIntraPeriod();

	int Start();
	int Stop();

protected:
	int Encode();

	//Recalcule les bornes effectives (config + bornes négociées) et la géométrie
	//qui en découle. Appelé à l'ouverture ET quand la configuration a changé sous
	//nos pieds : c'est ce qui permet de réappliquer sans redémarrer le thread.
	//À appeler sous `encodeLock`.
	void ComputeEffective();

private:
	//Le lisseur (RTPMultiplexerSmoother) est déjà un Worker et occupe l'héritage :
	//la boucle d'encodage est donc un Worker COMPOSÉ. Elle remplace le pthread
	//brut historique — dernier du dossier avec RTPEndpoint::run.
	class EncodeLoop : public Worker
	{
	public:
		explicit EncodeLoop(VideoEncoderMultiplexerWorker* owner) : owner(owner) {}
		virtual ~EncodeLoop() { StopThread(); }

		using Worker::StartThread;
		using Worker::StopThread;
		using Worker::IsThreadRunning;

		//Cadence de la boucle (précision µs), annulée par StopThread.
		::Wait& Pacer() { return wait; }

	protected:
		virtual int Run() { return owner->Encode(); }

	private:
		VideoEncoderMultiplexerWorker* owner;
	};

	//Recalcule `fps` et `intraEffective` d'après la consigne et la cadence
	//mesurée. Rend true si l'une des deux a changé. À appeler sous `encodeLock`.
	bool RecomputeFrameRate();
	//Ouvre l'encodeur si besoin. À appeler sous `encodeLock`.
	bool EnsureEncoder();
	//Suit la taille native du producteur (useInputSize). À appeler sous `encodeLock`.
	void ApplyNativeSize();

private:
	VideoInput *input;
	//Aucune source à échantillonner : les images sont poussées (transcodeur).
	bool		pushed;
	EncodeLoop	loop;

	VideoCodec::Type codec;

	int mode;
	int width;
	int height;
	//Cadence EFFECTIVE de l'encodeur ouvert : la consigne, éventuellement
	//abaissée à la cadence réelle de la source (§3.6).
	int fps;
	//Période intra EFFECTIVE, en images : `intraPeriod` mis à l'échelle de
	//`fps`, pour que sa durée en SECONDES ne change pas quand la source ralentit.
	int intraEffective;
	//La cadence CONFIGURÉE (SetCodec), dont `fps` repart à chaque (ré)ouverture :
	//l'écrêtage AV1 (phase 5b) doit suivre aussi une borne qui s'ASSOUPLIT.
	int configuredFps;
	//Plafond de cadence après écrêtage AV1 : borne haute de `fps`.
	int ceilingFps;
	//Cadence réelle mesurée par le transcodeur (0 = pas de mesure).
	std::atomic<int> measuredFps;
	int bitrate;
	int intraPeriod;
	//Limite TMMBR/REMB en vigueur (kbps, 0 = aucune) : plafond STRICT du
	//débit cible, persistant jusqu'à remplacement par une nouvelle valeur
	//(RFC 5104) — écrit par le plan de contrôle, lu par le chemin des paquets.
	std::atomic<int> videoBitrateLimit;
	std::atomic<int> senderBweLimit;
	Properties params;

	//── Plan de contrôle (thread XML-RPC) ────────────────────────────────────
	//Configuration (SetCodec) et bornes négociées par code codec (phase 5) :
	//écrites sous ce verrou court, signalées par un drapeau que le chemin des
	//paquets consomme au tour suivant. UN SEUL motif pour les deux (§4.4).
	std::mutex configLock;
	std::map<int,Properties> negotiated;
	std::atomic<bool> configDirty;

	//── Chemin des paquets ───────────────────────────────────────────────────
	//Sans thread d'encodage en mode poussé, Stop() ne joint plus rien : c'est ce
	//verrou qui empêche le plan de contrôle de détruire l'encodeur sous une
	//image en cours (§4.3). Ordre : Port(source).mutex -> encodeLock ->
	//configLock -> file du lisseur. Jamais l'inverse.
	std::mutex encodeLock;
	VideoEncoder* videoEncoder;
	//Bornes effectives de l'encodeur ouvert (config + négociation).
	Properties effective;
	//Débit appliqué à l'encodeur, en kbps.
	int current;
	//Base des horodatages de sortie : posée au Start du run d'encodage.
	timeval encodeStart;
	Acumulator bitrateAcu;
	Acumulator fpsAcu;
	DWORD num;
	//Mise à l'échelle vers la géométrie de sortie. Rend un partage zéro-copie
	//quand l'image est déjà à la bonne taille — donc gratuit pour le mixeur, qui
	//passe déjà par VideoPipe.
	VideoRescaler resizer;

	//Taille native relayée par le transcodeur (mode poussé).
	std::atomic<DWORD> pushedWidth;
	std::atomic<DWORD> pushedHeight;
	std::atomic<bool>  pushedSizeChanged;

	std::atomic<bool> encoding;
	std::atomic<bool> sendFPU;
	bool    useInputSize;
};

#endif	/* VIDEOENCODERWORKER_H */
