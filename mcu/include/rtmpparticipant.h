/* 
 * File:   rtmpparticipant.h
 * Author: Sergio
 *
 * Created on 19 de enero de 2012, 18:43
 */

#ifndef RTMPPARTICIPANT_H
#define	RTMPPARTICIPANT_H
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#include "rtmpstream.h"
#include "medkit/codecs.h"
#include "participant.h"
#include "multiconf.h"
#include "waitqueue.h"

class RTMPParticipant :
	public Participant,
	public RTMPMediaStream,
	public RTMPMediaStream::Listener
{
public:
	RTMPParticipant(DWORD partId);
	virtual ~RTMPParticipant();

	virtual int SetVideoCodec(VideoCodec::Type codec,int mode,int fps,int bitrate,int intraPeriod,const Properties& properties,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN);
	virtual int SetAudioCodec(AudioCodec::Type codec,const Properties& properties);
	virtual int SetTextCodec(TextCodec::Type codec);

	int SetCodecProperties(MediaFrame::Type media, const Properties & properties);
	virtual int SendVideoFPU(MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN);
	virtual int SendDTMF(DTMFMessage* dtmf);
	virtual MediaStatistics GetStatistics(MediaFrame::Type type,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN);

	//Video : double chemin (emprunté brut / possédant shared_ptr, Point 1 / C-4).
	virtual int SetVideoInput(VideoInput* input,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN)	{ videoInput	= input ? std::shared_ptr<VideoInput>(input,[](VideoInput*){}) : nullptr;	return 0; }
	virtual int SetVideoOutput(VideoOutput* output,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN) { videoOutput	= output ? std::shared_ptr<VideoOutput>(output,[](VideoOutput*){}) : nullptr;	return 0; }
	virtual int SetVideoInput(std::shared_ptr<VideoInput> input,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN)	{ videoInput	= std::move(input);	return 0; }
	virtual int SetVideoOutput(std::shared_ptr<VideoOutput> output,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN) { videoOutput	= std::move(output);	return 0; }
	virtual VideoOutput* GetVideoOutput(MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN) { return videoOutput.get();	}

	virtual int SetAudioInput(std::shared_ptr<AudioInput> input)	{ audioInput	= std::move(input);	return 0; }
	virtual int SetAudioOutput(std::shared_ptr<AudioOutput> output)	{ audioOutput	= std::move(output);	return 0; }
	virtual int SetTextInput(std::shared_ptr<TextInput> input)	{ textInput	= std::move(input);	return 0; }
	virtual int SetTextOutput(std::shared_ptr<TextOutput> output)	{ textOutput	= std::move(output);	return 0; }
	virtual int SetMute(MediaFrame::Type media, bool isMuted,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN);
	virtual int Init();
	virtual int End();

	/* Overrride from RTMPMediaStream*/
	virtual DWORD AddMediaListener(RTMPMediaStream::Listener *listener);
	virtual DWORD RemoveMediaListener(RTMPMediaStream::Listener *listener);
	virtual void RemoveAllMediaListeners();

	//RTMPMediaStream Listener
	virtual void onAttached(RTMPMediaStream *stream);
	virtual bool onMediaFrame(DWORD id,RTMPMediaFrame *frame);
	virtual void onMetaData(DWORD id,RTMPMetaData *meta);
	virtual void onCommand(DWORD id,const wchar_t *name,AMFData* obj);
	virtual void onStreamBegin(DWORD id);
	virtual void onStreamEnd(DWORD id);
	virtual void onStreamReset(DWORD id);
	virtual void onDetached(RTMPMediaStream *stream);

        bool StartSending();
	void StopSending();

protected:
	int RecVideo();
	int RecAudio();
	int SendVideo();
	int SendAudio();

private:
	bool StartReceiving();
	void StopReceiving();
	//Video
	int StartSendingVideo();
	int SetSendingVideoCodec();
	int SendFPU();
	int StopSendingVideo();
	int StartReceivingVideo();
	int StopReceivingVideo();
	//Audio RTP
	int StartSendingAudio();
	int SetSendingAudioCodec();
	int StopSendingAudio();
	int StartReceivingAudio();
	int StopReceivingAudio();
	//T140 Text RTP
	int StartSendingText();
	int StopSendingText();
	int StartReceivingText();
	int StopReceivingText();

private:
	RTMPMediaStream		*attached;
	//Protège les lectures/écritures du pointeur `attached` (H-6). Indépendant
	//de `use` (Participant). Jamais tenu pendant un appel vers un autre objet.
	std::mutex		attachedMutex;
	RTMPMetaData		*meta;
	MediaStatistics		audioStats;
	MediaStatistics		videoStats;
	MediaStatistics		textStats;
	WaitQueue<RTMPVideoFrame*> videoFrames;
	WaitQueue<RTMPAudioFrame*> audioFrames;

	AudioCodec::Type audioCodec;
	Properties	 audioProperties;
	VideoCodec::Type videoCodec;
	int 		videoWidth;
	int 		videoHeight;
	int 		videoFPS;
	int 		videoBitrate;
	int		videoIntraPeriod;
	Properties	videoProperties;
	DWORD RecVideoWidth;
	DWORD RecVideoHeight;

	//Las threads
	std::thread	recVideoThread;
	std::thread	recAudioThread;
	std::thread	sendVideoThread;
	std::thread	sendAudioThread;
	//Cadence de la boucle d'envoi vidéo, réveillée par StopSendingVideo
	::Wait		pacer;

	//Controlamos si estamos mandando o no
	std::atomic<bool>	sendingVideo;
	std::atomic<bool>	receivingVideo;
	std::atomic<bool>	sendingAudio;
	std::atomic<bool>	receivingAudio;
	//sendingText ne porte plus de thread : c'est le seul aiguillage du texte
	//entrant (onMetaData), lu sur le thread de la connexion RTMP.
	std::atomic<bool>	sendingText;
	std::atomic<bool>	inited;
	bool	sendFPU;
	timeval	first;

	//Co-propriété des pipes du mixer (Point 1 / C-4).
	std::shared_ptr<VideoInput>	videoInput;
	std::shared_ptr<VideoOutput>	videoOutput;
	std::shared_ptr<AudioInput>	audioInput;
	std::shared_ptr<AudioOutput>	audioOutput;
	std::shared_ptr<TextInput>	textInput;
	std::shared_ptr<TextOutput>	textOutput;

	bool	audioMuted;
	bool	videoMuted;
	bool	textMuted;
};

typedef std::shared_ptr<RTMPParticipant> RTMPParticipantPtr;

#endif	/* RTMPPARTICIPANT_H */

