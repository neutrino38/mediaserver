/* 
 * File:   VideoEncoderWorker.h
 * Author: Sergio
 *
 * Created on 2 de noviembre de 2011, 23:37
 */

#ifndef VIDEOENCODERWORKER_H
#define	VIDEOENCODERWORKER_H


#include <pthread.h>
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
	virtual void RemoveListener(Listener *listener);
	//Phase 5 (nego_fmtp §6.3) : bornes négociées par code codec, fusionnées
	//par-dessus `params` à l'ouverture de l'encodeur. Redémarre l'encodeur si
	//les bornes changent en cours d'encodage (les Properties ne sont lues qu'à
	//CreateEncoder).
	virtual void SetNegotiatedCodecProperties(const std::map<int,Properties>& byCodec);

private:
	int Start();
	int Stop();
protected:
	int Encode();

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
	int videoBitrateLimit;
	int videoBitrateLimitCount;
	Properties params;
	//Bornes négociées par code codec (phase 5) : ce que le pair de la patte
	//émettrice a déclaré savoir décoder. Fusionnées par-dessus `params` à
	//l'ouverture — la config du contrôleur reste, les bornes gagnent.
	std::map<int,Properties> negotiated;

	pthread_t	thread;
	pthread_mutex_t mutex;
	pthread_cond_t	cond;
	bool	encoding;
	bool	sendFPU;
        bool    useInputSize;
};

#endif	/* VIDEOENCODERWORKER_H */

