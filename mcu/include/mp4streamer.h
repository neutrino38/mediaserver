#ifndef _MP4STREAMER_H_
#define _MP4STREAMER_H_

#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include "media.h"
#include "rtp.h"
#include "text.h"
#include "audio.h"
#include "video.h"
#include "codecs.h"
#include "avcdescriptor.h"

// Lecteur/ordonnanceur MP4 du mcu, bâti sur le lecteur ffmpeg de libmedkit
// (classe Mp4FfReader, libavformat). Toute la mécanique bas niveau (démux,
// lecture des trames, ordonnancement temporel) est déléguée à Mp4FfReader ;
// MP4Streamer n'en reste que le pilote : un thread de lecture (std::thread) qui
// récupère les trames et les publie via le Listener.
//
// Mp4FfReader est déclarée en avant pour ne pas imposer <medkit/ffmp4reader.h>
// à tous les consommateurs de cet en-tête.
class Mp4FfReader;

class MP4Streamer
{
public:
	class Listener
	{
	public:
		virtual void onRTPPacket(RTPPacket &packet) = 0;
		virtual void onTextFrame(TextFrame &text) = 0;
		virtual void onMediaFrame(MediaFrame &frame) = 0;
		virtual void onEnd() = 0;
	};
public:
	MP4Streamer(Listener *listener);
	~MP4Streamer();

	int Open(const char* filename);
	bool HasAudioTrack();
	bool HasVideoTrack();
	bool HasTextTrack();
	DWORD GetAudioCodec()	{ return audioCodec;	}
	DWORD GetVideoCodec()	{ return videoCodec;	}
	double GetDuration();
	DWORD GetVideoWidth();
	DWORD GetVideoHeight();
	DWORD GetVideoBitrate();
	double GetVideoFramerate();
	AVCDescriptor* GetAVCDescriptor();
	int Play();
	QWORD PreSeek(QWORD time);
	int Seek(QWORD time);
	QWORD Tell();
	int Stop();
	int Close();

private:
	void PlayLoop();
	void Dispatch(MediaFrame *frame);
	void DispatchRtp(MediaFrame *frame);
	// Arrête le thread de lecture. À appeler avec lifecycleMutex verrouillé.
	void StopWorkerLocked();

private:
	Listener *listener;
	Mp4FfReader *reader;
	bool opened;

	DWORD audioCodec;
	DWORD videoCodec;

	// Paquets RTP réutilisés pour reconstruire les paquets depuis les
	// MediaFrame produits par mp4reader (évite une allocation par paquet).
	RTPPacket *audioPacket;
	RTPPacket *videoPacket;

	// std::atomic pour que la boucle de lecture teste l'état de lecture sans
	// verrou à chaque itération (le mutex n'est pris que pour l'attente
	// interruptible entre deux trames).
	std::atomic<bool>	playing;
	QWORD			startPos;	// (QWORD)-1 => depuis le début, sinon cible de seek
	std::thread		worker;
	std::mutex		lifecycleMutex;	// sérialise Open/Play/Seek/Stop/Close
	std::mutex		waitMutex;	// protège l'attente sur waitCv
	std::condition_variable	waitCv;
};

#endif
