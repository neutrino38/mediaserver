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
#include "videopipe.h"
#include <string>

class VideoTranscoder :
	public Joinable,
	public Joinable::Listener
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
	virtual void RemoveListener(Joinable::Listener *listener);
	//Phase 5 : les bornes négociées de la patte émettrice descendent à l'encodeur.
	virtual void SetNegotiatedCodecProperties(const std::map<int,Properties>& byCodec);

	//Virtuals from Joinable::Listener
	virtual void onRTPPacket(RTPPacket &packet);
	virtual void onResetStream();
	virtual void onEndStream();

	//Attach
	int Attach(const std::shared_ptr<Joinable> & join);
	int Dettach();

	const std::wstring& GetName() { return tag;	}

private:
	//Retire le transcodeur des listeners de la source courante, s'il y est.
	//Appelé par Attach, Dettach et End : en mode pont c'est LUI qui est inscrit
	//comme listener, donc c'est lui qui doit se retirer.
	void UnlistenSource();

	//Demande une intra à la SOURCE (FIR/PLI RTCP via l'endpoint amont), bornée à
	//une par seconde : RequestFPU n'a aucun anti-rebond, et relayer 1:1 les PLI
	//du puits transformerait une rafale aval en tempête de FIR vers la source.
	void RequestSourceFPU();

	//Pousse la consigne négociée de la patte émettrice (SetCodec, kbps) vers la
	//source en TMMBR. Appelé au basculement en mode pont et quand SetCodec
	//change la consigne pendant le pont : sans encodeur dans le chemin, seule
	//la source peut respecter la bande passante négociée du puits.
	void PushSourceBitrateLimit();

	VideoEncoderMultiplexerWorker	encoder;
	VideoDecoderJoinableWorker	decoder;
	VideoPipe	pipe;
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
};

#endif	/* VIDEOTRANSCODER_H */

