#ifndef _VIDEOSTREAM_H_
#define _VIDEOSTREAM_H_

#include <pthread.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <atomic>
#include "config.h"
#include "medkit/codecs.h"
#include "rtpsession.h"
#include "RTPSmoother.h"
#include "video.h"
#include "medkit/logo.h"
#include "task.h"

class VideoStream 
{
public:
	class Listener : public RTPSession::Listener
	{
	public:
		virtual void onRequestFPU() = 0;
	};
public:
	VideoStream(Listener* listener, Logo & muteLogo,MediaFrame::MediaRole = MediaFrame::VIDEO_MAIN);
	~VideoStream();

	int Init(VideoInput *input, VideoOutput *output);
	void SetRemoteRateEstimator(RemoteRateEstimator* estimator);
	int SetVideoCodec(VideoCodec::Type codec,int mode,int fps,int bitrate,int intraPeriod,const Properties& properties);
	int SetTemporalBitrateLimit(int bitrate);
	int StartSending(char *sendVideoIp,int sendVideoPort,RTPMap& rtpMap);
	int StartSending();
	int StopSending();
	int SendFPU();
	int StartReceiving(RTPMap& rtpMap);
	int StartReceiving();
	int StopReceiving();
	int SetMediaListener(MediaFrame::Listener *listener);
	int SetMute(bool isMuted);
	int SetLocalCryptoSDES(const char* suite, const char* key64);
	int SetRemoteCryptoSDES(const char* suite, const char* key64, int keyRank=0);
	int SetRemoteCryptoDTLS(const char *setup,const char *hash,const char *fingerprint);
	int SetLocalSTUNCredentials(const char* username, const char* pwd);
	int SetRemoteSTUNCredentials(const char* username, const char* pwd);
	int SetRTPProperties(const Properties& properties);
	int End();

	int IsSending()	  { return sendingVideo;  }
	int IsReceiving() { return receivingVideo;}
	MediaStatistics GetStatistics();
	
	//Chemin "emprunté" (SharedDocMixer/NULL) : shared_ptr NON possédant, ne
	//détruit jamais l'objet pointé (Point 1 / C-4).
	void SetVideoInput(VideoInput* input)	{  videoInput = input ? std::shared_ptr<VideoInput>(input, [](VideoInput*){}) : nullptr; }
	void SetVideoOutput(VideoOutput* output) { videoOutput = output ? std::shared_ptr<VideoOutput>(output, [](VideoOutput*){}) : nullptr; }
	//Chemin "possédant" : co-propriété du pipe du mixer.
	void SetVideoInput(std::shared_ptr<VideoInput> input)	{  videoInput = std::move(input);	}
	void SetVideoOutput(std::shared_ptr<VideoOutput> output) { videoOutput = std::move(output); }
	VideoOutput* GetVideoOutput() { return videoOutput.get();}
	
	//H-3 : session RTP observée en weak_ptr — SLIDES peut observer la session de
	//MAIN (alias). lock() au site d'usage protège contre un teardown concurrent.
	RTPSession& GetOwnSession() { return rtp; }
	//M-6 : arme le listener géré par shared_ptr sur la session interne.
	void SetWeakListener(std::weak_ptr<RTPSession::Listener> l)
	{ 
		rtp.SetWeakListener(std::move(l));
	}

	void SetRTPSession(std::weak_ptr<RTPSession> rtpsess, DWORD newSSRC) 
	{ 
		if (auto cur = rtpSession.lock()) cur->CancelGetPacket(recSSRC);
		rtpSession = std::move(rtpsess);
		recSSRC = newSSRC; 
	}
	
protected:
	int SendVideo();
	int RecVideo();

private:
	static void* startReceivingVideo(void *par);

	//Listners
	Listener* listener;
	MediaFrame::Listener *mediaListener;

	//Los objectos gordos
	//Co-propriété du pipe du mixer (Point 1 / C-4) : le pipe survit tant que ce
	//stream le détient, même si DeleteMixer a retiré la map. Pour le chemin
	//"emprunté" (SharedDocMixer), shared_ptr à deleter no-op.
	std::shared_ptr<VideoInput>	videoInput;
	std::shared_ptr<VideoOutput>	videoOutput;
	RTPSession      rtp;
	std::weak_ptr<RTPSession>     rtpSession;
	RTPSmoother		smoother;
	
	DWORD 	recSSRC;

	//Parametros del video
	VideoCodec::Type videoCodec;		//Codec de envio
	int		videoCaptureMode;	//Modo de captura de video actual
	int 		videoGrabWidth;		//Ancho de la captura
	int 		videoGrabHeight;	//Alto de la captur
	int 		videoFPS;
	int 		videoBitrate;
	int 		videoBitrateLimit;
	int 		videoBitrateLimitCount;
	int		videoIntraPeriod;
	Properties	videoProperties;

	//Las threads
	std::thread sendVideoThread;
	std::thread	recVideoThread;

	std::mutex mutex;
	std::condition_variable	cond;

	//Controlamos si estamos mandando o no
	std::atomic<enum TaskState> sendingVideo;	
	std::atomic<enum TaskState> receivingVideo;
	bool	inited;
	bool	sendFPU;
	bool	muted;
	MediaFrame::MediaRole mediaRole;
	Logo & logo;
};

#endif
