#ifndef AUDIOENCODER_H_
#define	AUDIOENCODER_H_
#include <mutex>
#include "audio.h"
#include "worker.h"
#include <set>

class AudioEncoderWorker : public Worker
{
public:
	AudioEncoderWorker();
	~AudioEncoderWorker();

	int Init(AudioInput *input);
	bool AddListener(MediaFrame::Listener *listener);
	bool RemoveListener(MediaFrame::Listener *listener);
	int SetAudioCodec(AudioCodec::Type codec);
	int StartEncoding();
	int StopEncoding();
	int End();

	int IsEncoding() { return encodingAudio;}

protected:
	int Encode();
	//Corps du Worker
	virtual int Run() { return Encode(); }

private:
	AudioEncoder* CreateAudioEncoder(AudioCodec::Type type);

private:
	typedef std::set<MediaFrame::Listener*> Listeners;
	
private:
	Listeners		listeners;
	AudioInput*		audioInput;
	AudioCodec::Type	audioCodec;
	std::mutex		mutex;
	int			encodingAudio;
};

#endif	/* AUDIOENCODER_H */

