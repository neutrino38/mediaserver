/* 
 * File:   rtpparticipant.h
 * Author: Sergio
 *
 * Created on 19 de enero de 2012, 18:41
 */

#ifndef RTPPARTICIPANT_H
#define	RTPPARTICIPANT_H
#include <memory>
#include <map>
#include <string>
#include "config.h"
#include "participant.h"
#include "videostream.h"
#include "audiostream.h"
#include "textstream.h"
#include "mp4recorder.h"
#include "eventstreaminghandler.h"

#define MAX_VIDEO_STREAM 2

class RTPParticipant : public Participant, public VideoStream::Listener, public AudioStream::Listener, public std::enable_shared_from_this<RTPParticipant>
{
public:
	RTPParticipant(DWORD partId,const std::wstring &uuid);
	virtual ~RTPParticipant();

	virtual int SetVideoCodec(VideoCodec::Type codec,int mode,int fps,int bitrate,int intraPeriod,const Properties& properties,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN);
	virtual int SetAudioCodec(AudioCodec::Type codec,const Properties& properties);
	virtual int SetTextCodec(TextCodec::Type codec);

	virtual int SendVideoFPU(MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN);
	virtual int SendDTMF(DTMFMessage* dtmf);
	
	virtual MediaStatistics GetStatistics(MediaFrame::Type type,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN);

	virtual int SetVideoInput(VideoInput* input,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN)	{  video[role]->SetVideoInput(input); return 1;	}
	virtual int SetVideoOutput(VideoOutput* output,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN) {  video[role]->SetVideoOutput(output); return 1;	}
	virtual int SetVideoInput(std::shared_ptr<VideoInput> input,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN)	{  video[role]->SetVideoInput(std::move(input)); return 1;	}
	virtual int SetVideoOutput(std::shared_ptr<VideoOutput> output,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN) {  video[role]->SetVideoOutput(std::move(output)); return 1;	}
	virtual VideoOutput* GetVideoOutput(MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN) { return video[role]->GetVideoOutput();	}
	virtual int SetAudioInput(std::shared_ptr<AudioInput> input)	{ audioInput	= std::move(input);	return 0; }
	virtual int SetAudioOutput(std::shared_ptr<AudioOutput> output)	{ audioOutput	= std::move(output);	return 0; }
	virtual int SetTextInput(std::shared_ptr<TextInput> input)	{ textInput	= std::move(input);	return 0; }
	virtual int SetTextOutput(std::shared_ptr<TextOutput> output)	{ textOutput	= std::move(output);	return 0; }

	virtual int SetMute(MediaFrame::Type media, bool isMuted ,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN);

	virtual int Init();
	virtual int End();

	//Profil d'adressage demandé par le contrôleur pour CETTE jambe (voir
	//NETWORK-CONFIGURATION.md) : il décide de l'adresse liée — donc de l'interface — et de
	//l'adresse annoncée. À appeler AVANT StartSending/StartReceiving, qui
	//publient le port. NULL ou chaîne vide = profil par défaut, soit le
	//comportement d'un contrôleur qui ignore cette notion.
	int SetAddressProfile(MediaFrame::Type media,const char* profile,std::string& error,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN);
	//Adresse à publier dans le SDP pour cette jambe (celle du profil retenu).
	IPAddress GetAnnouncedAddress(MediaFrame::Type media,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN);
	int StartSending(MediaFrame::Type media,char *sendIp,int sendPort,RTPMap& rtpMap,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN);
	int StartSending(MediaFrame::Type media,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN);
	int StopSending(MediaFrame::Type media,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN);
	int StartReceiving(MediaFrame::Type media,RTPMap& rtpMap,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN);
	//P8a : variante negociee (mcu_module.md §16.3). Delegue la selection des codecs
	//au media serveur au lieu de laisser le controleur SIP l'arbitrer seul.
	//
	//  offerFmtp         : le fmtp de l'offre, par PAYLOAD TYPE (vide si le
	//                      controleur n'en transmet pas -> on annonce notre config).
	//  rtpMap            : REMPLACEE par la map filtree, celle reellement installee.
	//  negotiatedFmtpOut : le fmtp par PT ACCEPTE. Contrat : tout PT accepte est une
	//                      cle, y compris les codecs SANS fmtp (valeur vide) ; un PT
	//                      absent a ete filtre. La presence de la cle EST le signal
	//                      d'acceptation, c'est la seule source du controleur.
	int StartReceiving(MediaFrame::Type media,RTPMap& rtpMap,
	                   const std::map<int,std::string>& offerFmtp,
	                   std::map<int,std::string>& negotiatedFmtpOut,
	                   MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN);
	int StartReceiving(MediaFrame::Type media,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN);
	int StopReceiving(MediaFrame::Type media,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN);
	int SetLocalCryptoSDES(MediaFrame::Type media,const char* suite, const char* key64,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN);
	int SetRemoteCryptoSDES(MediaFrame::Type media,const char* suite, const char* key64,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN,int keyRank=0);
        int SetRemoteCryptoDTLS(MediaFrame::Type media,const char *setup,const char *hash,const char *fingerprint,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN);
	int SetLocalSTUNCredentials(MediaFrame::Type media,const char* username, const char* pwd,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN);
	int SetRemoteSTUNCredentials(MediaFrame::Type media,const char* username, const char* pwd,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN);
	int SetRTPProperties(MediaFrame::Type media,const Properties& properties,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN);
	
	int SetMediaListener(MediaFrame::Listener *listener,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN) { return video[role]->SetMediaListener(listener); }

	//P7/S1 : (re)arme ou desarme le chien de garde d'inactivite RTP d'un media.
	//timeoutMs > 0 arme (chrono a partir de maintenant), 0 desarme. Jamais appele
	//sur le texte par le controleur : le T.140 est legitimement silencieux entre
	//deux frappes et declencherait un faux positif.
	int StartRTPTimeout(MediaFrame::Type media,DWORD timeoutMs,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN);

	//RTPSession::Listener
	virtual void onFPURequested(RTPSession *session);
	virtual void onReceiverEstimatedMaxBitrate(RTPSession *session,DWORD bitrate);
	virtual void onTempMaxMediaStreamBitrateRequest(RTPSession *session,DWORD bitrate,DWORD overhead);
	virtual void onSenderEstimatedBitrate(RTPSession *session,DWORD bitrate);
	virtual void onRequestFPU();
	virtual void onNewStream( RTPSession *session, DWORD newSsrc, bool receiving );
	virtual void onDTMF(DTMFMessage* dtmf);
	//P7/S1-S2. La session porte son propre media et son role (RTPSession::
	//GetMediaType/GetMediaRole), donc aucun besoin de comparer des pointeurs pour
	//savoir laquelle des trois piles a parle.
	virtual void onRTPTimeout( RTPSession *session );
	virtual void onRTPPacketReceived( RTPSession *session );
	
        virtual int DumpInfo(std::string & info);
		
public:
	MP4Recorder	recorder; //FIX this!
private:
	VideoStream* 	video[MAX_VIDEO_STREAM];
	AudioStream		audio;
	TextStream		text;
	RemoteRateEstimator estimator;
	EvenSource		eventSource;

	std::shared_ptr<AudioInput>	audioInput;
	std::shared_ptr<AudioOutput>	audioOutput;
	std::shared_ptr<TextInput>	textInput;
	std::shared_ptr<TextOutput>	textOutput;
};

typedef std::shared_ptr<RTPParticipant> RTPParticipantPtr;

#endif	/* RTPPARTICIPANT_H */

