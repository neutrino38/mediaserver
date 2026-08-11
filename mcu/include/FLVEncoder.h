#ifndef _FLVENCODER_H_
#define _FLVENCODER_H_
#include <pthread.h>
#include "wait.h"
#include "video.h"
#include "text.h"
#include "textencoder.h"
#include "audio.h"
#include "medkit/codecs.h"
#include "rtmpstream.h"

class FLVEncoder : public RTMPMediaStream
{
public:
	FLVEncoder();
	~FLVEncoder();
	void SetCodec( AudioCodec::Type codec )
	{
	    audioCodec = codec;
	}
	
	// Taille (et cadence) de la vidéo encodée. À appeler AVANT StartEncoding :
	// le thread d'encodage lit ces valeurs une fois, à l'ouverture du codec.
	// Sans cet appel, la valeur par défaut du constructeur (CIF 352x288)
	// s'applique — ce qui, pour un enregistrement de conférence, ne correspond
	// pas à la taille du composite de la mosaïque (cf. MultiConf::StartRecordingBroadcaster).
	// fps et bitrate à 0 : conserver les valeurs courantes.
	void SetVideoSize(int width, int height, int fps = 0, int bitrate = 0)
	{
		if (width > 0 && height > 0)
		{
			this->width  = width;
			this->height = height;
		}
		if (fps > 0)     this->fps     = fps;
		if (bitrate > 0) this->bitrate = bitrate;
	}

	int Init(AudioInput* audioInput,VideoInput *videoInput, TextInput *textInput);
	int StartEncoding();
	int StopEncoding();
	int End();
	/* Overrride from RTMPMediaStream*/
	virtual DWORD AddMediaListener(RTMPMediaStream::Listener* listener);
	//Add listenest for media stream
	virtual DWORD AddMediaFrameListener(MediaFrame::Listener* listener);
	virtual DWORD RemoveMediaFrameListener(MediaFrame::Listener* listener);

protected:
	int EncodeAudio();
	int EncodeVideo();

private:
	//Funciones propias
	static void *startEncodingAudio(void *par);
	static void *startEncodingVideo(void *par);

private:
	typedef std::set<MediaFrame::Listener*> MediaFrameListeners;

	
private:
	AudioCodec::Type	audioCodec;
	AudioInput*		audioInput;
	pthread_t		encodingAudioThread;
	int			encodingAudio;
	Properties		audioProperties;

	VideoCodec::Type	videoCodec;
	VideoInput*		videoInput;
	pthread_t		encodingVideoThread;
	int			encodingVideo;

	// Text is entirely managed by text encoder
	TextEncoder		textEncoder;
	
	RTMPMetaData*	meta;
	RTMPVideoFrame* frameDesc;
	RTMPAudioFrame* aacSpecificConfig;
	int		width;
	int		height;
	int		bitrate;
	int		fps;
	int		intra;


	int		inited;
	bool		sendFPU;
	timeval		first;
	//Protège listeners et méta (état partagé)
	pthread_mutex_t mutex;
	//Cadence de la boucle d'encodage (l'ancienne cond n'était JAMAIS
	//signalée : pur sommeil, désormais réveillable)
	::Wait		pacer;

	Use		use;

	MediaFrameListeners mediaListeners;
	
};

#endif
