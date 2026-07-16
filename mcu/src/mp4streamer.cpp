#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <chrono>
// En-têtes mcu d'abord : ils fixent les gardes d'inclusion (media/audio/video/
// codecs/avcdescriptor) de sorte que <medkit/ffmp4reader.h>, inclus ensuite,
// réutilise ces mêmes définitions (désormais partagées avec libmedkit).
#include "log.h"
#include "medkit/codecs.h"
#include "rtp.h"
#include "text.h"
#include "audio.h"
#include "video.h"
#include "avcdescriptor.h"
#include "mp4streamer.h"
// Lecteur MP4 ffmpeg de libmedkit.
#include "medkit/ffmp4reader.h"

MP4Streamer::MP4Streamer(Listener *listener) : playing(false)
{
	//Save listener
	this->listener = listener;
	//No reader / file yet
	reader = NULL;
	opened = false;
	//No codecs
	audioCodec = 0;
	videoCodec = 0;
	//No reusable packets
	audioPacket = NULL;
	videoPacket = NULL;
	//Play from the beginning by default
	startPos = (QWORD)-1;
}

MP4Streamer::~MP4Streamer()
{
	//Close (stops the worker and releases the file)
	Close();
}

int MP4Streamer::Open(const char *filename)
{
	//Serialize with the rest of the lifecycle
	std::lock_guard<std::mutex> lock(lifecycleMutex);

	Log(">MP4Streamer opening [%s]\n", filename);

	//If already opened
	if (opened)
		return Error("Already opened\n");

	//Create the libmedkit ffmpeg reader (it opens the file itself)
	reader = new Mp4FfReader(filename);

	// If not valid
	if (!reader->IsOpen())
	{
		delete reader;
		reader = NULL;
		return Error("Could not open %s\n", filename);
	}

	//Try to open an audio track (native codecs, no transcoding)
	AudioCodec::Type acodecs[] = {
		AudioCodec::PCMU, AudioCodec::PCMA, AudioCodec::AMR,
		AudioCodec::OPUS, AudioCodec::GSM,  AudioCodec::G722
	};
	if (reader->OpenTrack(acodecs, sizeof(acodecs)/sizeof(acodecs[0]), AudioCodec::PCMU, false) > 0)
	{
		AudioCodec::Type ac;
		if (reader->GetCodec(ac))
			audioCodec = ac;
	}

	//Try to open a video track (native codecs, no transcoding)
	VideoCodec::Type vcodecs[] = {
		VideoCodec::H264, VideoCodec::H263_1998,
		VideoCodec::H263_1996, VideoCodec::VP8
	};
	if (reader->OpenTrack(vcodecs, sizeof(vcodecs)/sizeof(vcodecs[0]), VideoCodec::H264, false, false) > 0)
	{
		VideoCodec::Type vc;
		if (reader->GetVideoCodec(vc))
			videoCodec = vc;
	}

	//Try to open a text track (plain T.140, no redundancy)
	reader->OpenTrack(TextCodec::T140, 0, 1);

	//Pre-create the reusable RTP packets used to republish frames
	if (reader->HasAudioTrack())
		audioPacket = new RTPPacket(MediaFrame::Audio, audioCodec, audioCodec);
	if (reader->HasVideoTrack())
		videoPacket = new RTPPacket(MediaFrame::Video, videoCodec, videoCodec);

	//We are opened
	opened = true;

	Log("<MP4Streamer opened [%s] audio:%d video:%d text:%d\n", filename,
		reader->HasAudioTrack(), reader->HasVideoTrack(), reader->HasTextTrack());

	return 1;
}

bool MP4Streamer::HasAudioCodec(AudioCodec::Type codec)
{
	std::lock_guard<std::mutex> lock(lifecycleMutex);
	return reader && reader->HasAudioCodec(codec);
}

bool MP4Streamer::HasVideoCodec(VideoCodec::Type codec)
{
	std::lock_guard<std::mutex> lock(lifecycleMutex);
	return reader && reader->HasVideoCodec(codec);
}

int MP4Streamer::SetAudioCodec(AudioCodec::Type codec)
{
	std::lock_guard<std::mutex> lock(lifecycleMutex);

	if (!opened || !reader)
		return 0;
	if (playing.load())
		return Error("MP4Streamer: cannot change audio codec while playing\n");

	//Exact-match selection: single-codec list so only that codec's track opens
	//(and the current selection is left untouched if the file lacks it).
	AudioCodec::Type c = codec;
	if (reader->OpenTrack(&c, 1, c, false) <= 0)
		return 0;

	AudioCodec::Type ac;
	if (reader->GetCodec(ac))
		audioCodec = ac;

	//Recreate the reusable RTP packet with the new codec
	if (audioPacket)
	{
		delete audioPacket;
		audioPacket = NULL;
	}
	audioPacket = new RTPPacket(MediaFrame::Audio, audioCodec, audioCodec);

	Log("MP4Streamer: audio codec re-selected to %s\n", AudioCodec::GetNameFor((AudioCodec::Type)audioCodec));
	return 1;
}

int MP4Streamer::SetVideoCodec(VideoCodec::Type codec)
{
	std::lock_guard<std::mutex> lock(lifecycleMutex);

	if (!opened || !reader)
		return 0;
	if (playing.load())
		return Error("MP4Streamer: cannot change video codec while playing\n");

	VideoCodec::Type c = codec;
	if (reader->OpenTrack(&c, 1, c, false, false) <= 0)
		return 0;

	VideoCodec::Type vc;
	if (reader->GetVideoCodec(vc))
		videoCodec = vc;

	if (videoPacket)
	{
		delete videoPacket;
		videoPacket = NULL;
	}
	videoPacket = new RTPPacket(MediaFrame::Video, videoCodec, videoCodec);

	Log("MP4Streamer: video codec re-selected to %s\n", VideoCodec::GetNameFor((VideoCodec::Type)videoCodec));
	return 1;
}

int MP4Streamer::SetAudioCodecTranscoded(AudioCodec::Type target)
{
	std::lock_guard<std::mutex> lock(lifecycleMutex);

	if (!opened || !reader)
		return 0;
	if (playing.load())
		return Error("MP4Streamer: cannot enable transcoding while playing\n");

	if (reader->OpenAudioTranscoded(target) <= 0)
		return 0;

	AudioCodec::Type ac;
	if (reader->GetCodec(ac))
		audioCodec = ac;	// = target

	if (audioPacket)
	{
		delete audioPacket;
		audioPacket = NULL;
	}
	audioPacket = new RTPPacket(MediaFrame::Audio, audioCodec, audioCodec);

	Log("MP4Streamer: audio transcoding enabled -> %s\n",
		AudioCodec::GetNameFor((AudioCodec::Type)audioCodec));
	return 1;
}

bool MP4Streamer::HasAudioTrack()	{ return reader && reader->HasAudioTrack();	}
bool MP4Streamer::HasVideoTrack()	{ return reader && reader->HasVideoTrack();	}
bool MP4Streamer::HasTextTrack()	{ return reader && reader->HasTextTrack();	}

double MP4Streamer::GetDuration()	{ return reader ? reader->GetDuration()      : 0;	}
DWORD  MP4Streamer::GetVideoWidth()	{ return reader ? reader->GetVideoWidth()    : 0;	}
DWORD  MP4Streamer::GetVideoHeight()	{ return reader ? reader->GetVideoHeight()   : 0;	}
DWORD  MP4Streamer::GetVideoBitrate()	{ return reader ? reader->GetVideoBitrate()  : 0;	}
double MP4Streamer::GetVideoFramerate()	{ return reader ? reader->GetVideoFramerate(): 0;	}

AVCDescriptor* MP4Streamer::GetAVCDescriptor()
{
	return reader ? reader->GetAVCDescriptor() : NULL;
}

QWORD MP4Streamer::Tell()
{
	return reader ? reader->Tell() : 0;
}

int MP4Streamer::Play()
{
	std::lock_guard<std::mutex> lock(lifecycleMutex);

	Log(">MP4Streamer Play\n");

	//Check we are opened
	if (!opened)
		return Error("MP4Streamer not opened!\n");

	//Stop any current playback
	StopWorkerLocked();

	//From the beginning
	startPos = (QWORD)-1;

	//We are playing
	playing.store(true);

	//Launch the playback thread
	worker = std::thread(&MP4Streamer::PlayLoop, this);

	Log("<MP4Streamer Play\n");

	return 1;
}

QWORD MP4Streamer::PreSeek(QWORD time)
{
	std::lock_guard<std::mutex> lock(lifecycleMutex);

	if (!opened || !reader)
		return time;

	//Reading and seeking share the same demux cursor, so stop the worker
	//before touching the reader (doSeek() stops right after anyway).
	StopWorkerLocked();

	//Get the nearest sync frame without altering the reader state
	return reader->PreSeek(time);
}

int MP4Streamer::Seek(QWORD time)
{
	std::lock_guard<std::mutex> lock(lifecycleMutex);

	Log(">MP4Streamer seek [%llu]\n", (unsigned long long)time);

	//Check we are opened
	if (!opened)
		return Error("MP4Streamer not opened!\n");

	//Stop any current playback
	StopWorkerLocked();

	//Start from the requested position
	startPos = time;

	//We are playing
	playing.store(true);

	//Launch the playback thread
	worker = std::thread(&MP4Streamer::PlayLoop, this);

	Log("<MP4Streamer seeked [%llu]\n", (unsigned long long)time);

	return 1;
}

int MP4Streamer::Stop()
{
	std::lock_guard<std::mutex> lock(lifecycleMutex);

	//Nothing to do
	if (!worker.joinable() && !playing.load())
		return 0;

	Log(">MP4Streamer Stop\n");

	StopWorkerLocked();

	Log("<MP4Streamer Stop\n");

	return 1;
}

int MP4Streamer::Close()
{
	std::lock_guard<std::mutex> lock(lifecycleMutex);

	Log(">MP4Streamer Close\n");

	//Stop playback
	StopWorkerLocked();

	//Change state
	opened = false;

	//Release reusable packets
	if (audioPacket)
	{
		delete audioPacket;
		audioPacket = NULL;
	}
	if (videoPacket)
	{
		delete videoPacket;
		videoPacket = NULL;
	}

	//Release the reader (closes the file via libavformat)
	if (reader)
	{
		delete reader;
		reader = NULL;
	}

	Log("<MP4Streamer Close\n");

	return 1;
}

void MP4Streamer::StopWorkerLocked()
{
	//Nothing running
	if (!worker.joinable())
		return;

	//Signal the loop to stop (lock-free flag)
	playing.store(false);

	//Wake up the interruptible wait
	{
		std::lock_guard<std::mutex> wl(waitMutex);
	}
	waitCv.notify_all();

	if (worker.get_id() == std::this_thread::get_id())
		//Called from within the worker (typically through onEnd()): we cannot
		//join ourselves, so detach. The thread is finishing anyway.
		worker.detach();
	else
		//Called from another thread: wait for the worker to finish.
		worker.join();
}

void MP4Streamer::PlayLoop()
{
	Log(">MP4Streamer::PlayLoop()\n");

	//Position the reader (this also (re)starts its internal playback clock)
	if (startPos == (QWORD)-1)
		reader->Rewind();
	else
		reader->Seek(startPos);

	bool completed = false;

	// The reader is self-timed: GetNextFrame() returns the next frame (if its
	// scheduled time has come) together with the time to wait before the
	// following one. We only pace the loop and republish the frames.
	while (playing.load())
	{
		int errcode = 0;
		unsigned long waittime = 0;

		MediaFrame *frame = reader->GetNextFrame(errcode, waittime);

		//End of file
		if (errcode == -1 || errcode == -2)
		{
			completed = true;
			break;
		}

		//Publish the frame if any
		if (frame)
			Dispatch(frame);

		//Read error on this frame: stop to avoid a busy loop
		if (errcode < 0)
			break;

		//Wait until the next frame is due (interruptible on Stop)
		if (waittime > 0)
		{
			std::unique_lock<std::mutex> wl(waitMutex);
			waitCv.wait_for(wl, std::chrono::milliseconds(waittime),
				[this]{ return !playing.load(); });
		}
	}

	Log("-MP4Streamer::PlayLoop() end [completed:%d]\n", completed);

	//Not playing anymore
	playing.store(false);

	// IMPORTANT: onEnd() may restart playback (looping players call Play()
	// from within onEnd(), on THIS very thread). StopWorkerLocked() then
	// detaches us, so after onEnd() returns we must not touch any shared
	// state — just let the thread unwind.
	if (completed && listener)
		listener->onEnd();

	Log("<MP4Streamer::PlayLoop()\n");
}

void MP4Streamer::Dispatch(MediaFrame *frame)
{
	if (!listener || !frame)
		return;

	//Text is delivered as-is through onTextFrame
	if (frame->GetType() == MediaFrame::Text)
	{
		listener->onTextFrame(*(TextFrame *)frame);
		return;
	}

	//Audio / video: deliver the media frame, then the reconstructed RTP packets
	listener->onMediaFrame(*frame);
	DispatchRtp(frame);
}

void MP4Streamer::DispatchRtp(MediaFrame *frame)
{
	//Nothing to packetize
	if (!frame->HasRtpPacketizationInfo())
		return;

	//Pick the reusable packet for this media
	RTPPacket *packet = NULL;
	if (frame->GetType() == MediaFrame::Audio)
		packet = audioPacket;
	else if (frame->GetType() == MediaFrame::Video)
		packet = videoPacket;

	if (!packet)
		return;

	BYTE *frameData = frame->GetData();
	DWORD frameLen  = frame->GetLength();

	// Each RtpPacketization describes one RTP payload (as it was hinted in the
	// MP4 file). Rebuild the RTP packets one by one, preserving mark bit and
	// timestamp, exactly like the former hint-track reader did.
	MediaFrame::RtpPacketizationInfo &info = frame->GetRtpPacketizationInfo();
	for (MediaFrame::RtpPacketizationInfo::iterator it = info.begin(); it != info.end(); ++it)
	{
		MediaFrame::RtpPacketization *rtp = *it;

		DWORD pos       = rtp->GetPos();
		DWORD size      = rtp->GetSize();
		DWORD prefixLen = rtp->GetPrefixLen();

		//Bounds checks
		if (pos + size > frameLen)
			continue;
		if (prefixLen + size > packet->GetMaxMediaLength())
			continue;

		BYTE *out = packet->GetMediaData();

		//Optional RTP payload prefix (e.g. H.264 headers)
		if (prefixLen)
			memcpy(out, rtp->GetPrefixData(), prefixLen);

		//Payload
		memcpy(out + prefixLen, frameData + pos, size);

		packet->SetMediaLength(prefixLen + size);
		packet->SetMark(rtp->IsMark());
		packet->SetTimestamp(frame->GetTimeStamp());

		//Publish it
		listener->onRTPPacket(*packet);
	}
}
