#ifndef _TEXTSTREAM_H_
#define _TEXTSTREAM_H_

#include <pthread.h>
#include <memory>
#include "config.h"
#include "medkit/codecs.h"
#include "rtpsession.h"
#include "text.h"
#include "redcodec.h"
#include "task.h"
#include <deque>

class TextStream
{
public:
	TextStream(RTPSession::Listener* listener);
	~TextStream();

	//Surcharge brute conservée (appelant MediaBridgeSession, hors périmètre
	//Phase 5) : construit un shared_ptr NON possédant en interne.
	int Init(TextInput *input,TextOutput *output);
	//Surcharge co-propriété (Point 1) : pipe du mixer, RTPParticipant.
	int Init(std::shared_ptr<TextInput> input,std::shared_ptr<TextOutput> output);
	int SetTextCodec(TextCodec::Type codec);
	int StartSending(char* sendTextIp,int sendTextPort,RTPMap& rtpMap);
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

	int IsSending()	  { return sendingText;   }
	int IsReceiving() { return receivingText; }
        int GetLocalPort() { return rtp.GetLocalPort(); }
	MediaStatistics GetStatistics();

protected:
	int SendText();
	int RecText();

private:
        RedundentCodec redCodec;

private:
	//Funciones propias
	static void *startSendingText(void *par);
	static void *startReceivingText(void *par);
	TextCodec* CreateTextCodec(TextCodec::Type type);
	//Los objectos gordos
public:
	//P7/S1 : (re)arme le chien de garde d'inactivite RTP sur la session interne.
	//timeoutMs > 0 (re)configure le seuil ET arme, chrono a partir de maintenant ;
	//0 desarme. Le mecanisme est celui deja utilise par JSR-309
	//(MediaSession::EndpointStartRTPTimeout), on ne fait que l'exposer au MCU.
	void ArmRTPTimeout(DWORD timeoutMs) { rtp.ArmRTPTimeout(timeoutMs); }

private:
	RTPSession	rtp;
	//Co-propriété du pipe du mixer (Point 1 / C-4). Pour l'appelant
	//MediaBridgeSession, ce sont des shared_ptr à deleter no-op (non possédants).
	std::shared_ptr<TextInput>	textInput;
	std::shared_ptr<TextOutput>	textOutput;

	//Parametros del text
	TextCodec::Type textCodec;
	BYTE		t140Codec;
	
	//Las threads
	pthread_t 	recTextThread;
	pthread_t 	sendTextThread;

	//Controlamos si estamos mandando o no
	enum TaskState	sendingText;
	enum TaskState 	receivingText;

	bool		muted;
};
#endif
