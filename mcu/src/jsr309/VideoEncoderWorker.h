/* 
 * File:   VideoEncoderWorker.h
 * Author: Sergio
 *
 * Created on 2 de noviembre de 2011, 23:37
 */

#ifndef VIDEOENCODERWORKER_H
#define	VIDEOENCODERWORKER_H


#include <pthread.h>
#include <atomic>
#include <mutex>
#include "config.h"
#include "medkit/codecs.h"
#include "video.h"
#include "RTPMultiplexerSmoother.h"

class VideoEncoderMultiplexerWorker :
	public RTPMultiplexerSmoother
{
public:
	VideoEncoderMultiplexerWorker();
	virtual ~VideoEncoderMultiplexerWorker();

	int Init(VideoInput *input);
	int SetCodec(VideoCodec::Type codec,int mode,int fps,int bitrate,int intraPeriod, Properties & properties);
	int End();
    void UseInputSize(bool use) { useInputSize = use; }
	
	//Joinable interface
	virtual void AddListener(Listener *listener);
	virtual void Update();
	virtual void SetREMB(int bitrate);
	virtual void SetSenderEstimate(DWORD bitrate);
	virtual void RemoveListener(Listener *listener);
	//Phase 5 (nego_fmtp §6.3) : bornes négociées par code codec, fusionnées
	//par-dessus `params` à l'ouverture de l'encodeur.
	//
	//Encodeur EN MARCHE : les bornes sont mémorisées et un drapeau est levé — la
	//boucle d'encodage les applique elle-même en jetant son encodeur. PAS de
	//Stop/Start ici : cet appel arrive du thread XML-RPC, qui tient le verrou de la
	//MediaSession, et joindre le thread d'encodage sous ce verrou a gelé le serveur
	//entier le 2026-08-13 (le deinit de SVT-AV1 0.9.0 ne revenait jamais, et les
	//threads de dispatch s'empilaient derrière le verrou jusqu'à ce que le serveur
	//cesse d'accepter). Le prix du report est un GOP émis avec les bornes
	//précédentes.
	virtual void SetNegotiatedCodecProperties(const std::map<int,Properties>& byCodec);

	inline VideoCodec::Type GetCodec() { return codec; }
	//Consigne configurée (SetCodec), en kbps ; 0 tant que SetCodec n'a pas été
	//appelé. Sert de borne au relais TMMBR/REMB du mode pont : le puits ne peut
	//pas « autoriser » plus que ce que sa négociation porte.
	inline int GetBitrate() { return bitrate; }
private:
	int Start();
	int Stop();
protected:
	int Encode();

	//Recalcule les bornes effectives (config + bornes négociées) et la géométrie
	//qui en découle. Appelé à l'ouverture ET quand la négociation a changé sous
	//nos pieds : c'est ce qui permet de réappliquer sans redémarrer le thread.
	void ComputeEffective(Properties& effective);

private:
	static void *startEncoding(void *par);

private:
	VideoInput *input;
	VideoCodec::Type codec;

	int mode;
	int width;
	int height;
	int fps;
	//La cadence CONFIGURÉE (SetCodec), dont `fps` repart à chaque (ré)ouverture :
	//l'écrêtage AV1 (phase 5b) doit suivre aussi une borne qui s'ASSOUPLIT.
	int configuredFps;
	int bitrate;
	int intraPeriod;
	//Limite TMMBR/REMB en vigueur (kbps, 0 = aucune) : plafond STRICT du
	//débit cible, persistant jusqu'à remplacement par une nouvelle valeur
	//(RFC 5104) — écrit par le plan de contrôle, lu par la boucle d'encodage.
	int videoBitrateLimit;
	int senderBweLimit;
	Properties params;
	//Bornes négociées par code codec (phase 5) : ce que le pair de la patte
	//émettrice a déclaré savoir décoder. Fusionnées par-dessus `params` à
	//l'ouverture — la config du contrôleur reste, les bornes gagnent.
	//
	//ÉCRITE par le plan de contrôle (SetNegotiatedCodecProperties, thread XML-RPC)
	//et LUE par la boucle d'encodage : depuis que le report a remplacé le
	//Stop/Start, les deux ne sont plus séparés par l'arrêt du thread, d'où le
	//verrou. Il ne couvre que cette map, n'est jamais tenu à travers un Start, un
	//Stop ou un appel de codec — donc aucun ordre de verrouillage à respecter.
	std::map<int,Properties> negotiated;
	std::mutex negotiatedLock;
	//Bornes changées pendant que la boucle tourne : elle les applique au tour
	//suivant en jetant son encodeur (les Properties ne sont lues qu'à
	//CreateEncoder). Atomique : posé par le thread de contrôle, consommé par la
	//boucle, sans passer par negotiatedLock.
	std::atomic<bool> negotiatedDirty;

	pthread_t	thread;
	//Cadence de la boucle d'encodage (précision µs), réveillée par Stop
	::Wait		pacer;
	bool	encoding;
	bool	sendFPU;
        bool    useInputSize;
};

#endif	/* VIDEOENCODERWORKER_H */

