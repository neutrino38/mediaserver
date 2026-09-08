#ifndef _TEXTSTREAM_H_
#define _TEXTSTREAM_H_

#include <pthread.h>
#include <memory>
#include "config.h"
#include "medkit/codecs.h"
#include "rtpsession.h"
#include "text.h"
#include "redcodec.h"
#include "t140bridge.h"
#include "t140datachannel.h"
#include "task.h"
#include <deque>

//Le flux texte d'un participant de conférence. Deux transports possibles, et une
//seule session RTP dans les deux cas : elle est déjà ICE + DTLS + UDP, seul
//change ce qui voyage dedans. Le pont WebSocket (ParticipantTextWS) avait dû
//contourner par la couture du mixeur, parce qu'un WebSocket n'a PAS de jambe
//ICE/DTLS/UDP ; un data channel en a une, et c'est celle-ci.
//
//Conception : docs/conception/T140-DC/SPEC.md §7.
class TextStream : public T140DataChannel::Listener
{
public:
	TextStream(RTPSession::Listener* listener);
	//Session RTP de ce flux : le profil d'adressage se pose dessus (NETWORK-CONFIGURATION.md).
	RTPSession& GetOwnSession() { return rtp; }
	~TextStream();

	//Co-propriété du pipe (Point 1) : pipe du mixer, RTPParticipant.
	int Init(std::shared_ptr<TextInput> input,std::shared_ptr<TextOutput> output);
	int SetTextCodec(TextCodec::Type codec);

	//Transport de CE flux : RTP par défaut, SCTP pour un data channel WebRTC. À
	//poser AVANT StartReceiving/StartSending — les threads en cours ne changent
	//pas de dialecte en marche.
	int SetTransport(MediaFrame::MediaProtocol proto);
	MediaFrame::MediaProtocol GetTransport() const { return transport; }
	//Paramètres SCTP : pose le `a=sctp-port` du pair et rend les nôtres, que le
	//contrôleur publie. `streamId` vaut -1 tant que le canal n'est pas ouvert.
	int SetupDataChannel(WORD remoteSCTPPort,WORD& localSCTPPort,
			     DWORD& maxMessageSize,int& streamId);
	bool IsDataChannelOpen() const { return bridge.IsOpen(); }
	//Ouvre le canal NOUS-MÊMES (DATA_CHANNEL_OPEN). Le cas nominal ne l'appelle
	//pas : nous répondons `a=setup:passive`, donc c'est le navigateur qui crée le
	//canal. `streamId` doit respecter la parité de RFC 8832 §6 : IMPAIR pour le
	//serveur DTLS, le rôle que nous tenons.
	int  OpenDataChannel(WORD streamId) { return bridge.OpenChannel(streamId); }

	//T140DataChannel::Listener — le pair a parlé sur le canal
	virtual void onT140Block(const BYTE* data,DWORD size);
	virtual void onT140ChannelOpen();
	virtual void onT140ChannelLost();
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
	//Le même travail que SendText, sur un data channel : ni codec, ni redondance,
	//ni keepalive — SCTP est fiable et le texte est légitimement silencieux.
	int SendTextOverDataChannel();
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
	//Le pont vers la pile T.140 sur data channel. Construit toujours, démarré
	//seulement en mode SCTP : son constructeur ne touche pas à usrsctp, un
	//participant en RTP n'en paie donc rien.
	T140Bridge	bridge;
	MediaFrame::MediaProtocol transport;
	//Origine des horodatages des trames qu'on injecte dans le mixeur.
	timeval		clock;
	//Co-propriété du pipe du mixer (Point 1 / C-4).
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
