#ifndef _AUDIOSTREAM_H_
#define _AUDIOSTREAM_H_

#include <mutex>
#include <pthread.h>
#include <vector>
#include <memory>
#include "config.h"
#include "medkit/codecs.h"
#include "rtpsession.h"
#include "audio.h"
#include "dtmfmessage.h"
#include "task.h"

#define MAX_DTMF_BUFFER 64

class AudioStream
{
public:
class Listener : public RTPSession::Listener
{
public:
	virtual void onDTMF( DTMFMessage* dtmf) = 0;
};
public:
	AudioStream(Listener* listener);
	~AudioStream();

	int Init(std::shared_ptr<AudioInput> input,std::shared_ptr<AudioOutput> output);
	//M-6 : arme le listener géré par shared_ptr sur la session interne.
	void SetWeakListener(std::weak_ptr<RTPSession::Listener> l) { rtp.SetWeakListener(std::move(l)); }
	//Session RTP de ce flux : le profil d'adressage se pose dessus (NETWORK-CONFIGURATION.md).
	RTPSession& GetOwnSession() { return rtp; }
	void SetRemoteRateEstimator(RemoteRateEstimator* estimator);
	int SetAudioCodec(AudioCodec::Type codec,const Properties& properties);
	int StartSending(char* sendAudioIp,int sendAudioPort,RTPMap& rtpMap);
	int StopSending();
	int StartReceiving(RTPMap& rtpMap);
	int StopReceiving();
	int SetMute(bool isMuted);
	int SetLocalCryptoSDES(const char* suite, const char* key64);
	int SetRemoteCryptoSDES(const char* suite, const char* key64);
	int SetRemoteCryptoDTLS(const char *setup,const char *hash,const char *fingerprint);
	int SetLocalSTUNCredentials(const char* username, const char* pwd);
	int SetRemoteSTUNCredentials(const char* username, const char* pwd);
	int SetRTPProperties(const Properties& properties);
	int End();

	int IsSending()	  { return sendingAudio;  }
	int IsReceiving() { return receivingAudio;}
	MediaStatistics GetStatistics();
	
	int SendDTMF(DTMFMessage* dtmf);

protected:
	int SendAudio();
	int RecAudio();

private:
	//Funciones propias
	static void *startSendingAudio(void *par);
	static void *startReceivingAudio(void *par);


	Listener* listener;

	//Los objectos gordos
public:
	//P7/S1 : (re)arme le chien de garde d'inactivite RTP sur la session interne.
	//timeoutMs > 0 (re)configure le seuil ET arme, chrono a partir de maintenant ;
	//0 desarme. Le mecanisme est celui deja utilise par JSR-309
	//(MediaSession::EndpointStartRTPTimeout), on ne fait que l'exposer au MCU.
	void ArmRTPTimeout(DWORD timeoutMs) { rtp.ArmRTPTimeout(timeoutMs); }

private:
	RTPSession	rtp;
	//Co-propriété du pipe du mixer (Point 1 / C-4) : le pipe reste vivant tant
	//que ce stream le détient, même si DeleteMixer a déjà retiré la map.
	std::shared_ptr<AudioInput>	audioInput;
	std::shared_ptr<AudioOutput>	audioOutput;

	//Parametros del audio
	AudioCodec::Type audioCodec;
public:
	//P8a : les proprietes codec locales, telles que SetRTPProperties les a retenues
	//(prefixe "codec." deja retire). C'est de la que le negociateur derive le fmtp
	//que NOUS annoncons, d'ou l'obligation pour le controleur de les envoyer AVANT
	//StartReceiving (cf. mcu_module.md decision 8).
	const Properties& GetCodecProperties() const { return audioProperties; }

private:
	Properties	 audioProperties;
	
	//Las threads
	pthread_t 	recAudioThread;
	pthread_t 	sendAudioThread;

	std::mutex mutex;

	//Controlamos si estamos mandando o no
	enum TaskState 	sendingAudio;
	enum TaskState 	receivingAudio;
	std::vector<DTMFMessage*> dtmfBuffer;
	
	bool		muted;

};
#endif
