#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
// En-tetes mcu d'abord : ils fixent les gardes d'inclusion (media/audio/video/
// codecs) de sorte que <medkit/mp4writer.h>, inclus ensuite, reutilise ces
// memes definitions (desormais partagees avec libmedkit), exactement comme
// mp4recorder.cpp le fait pour mp4reader.
#include "log.h"
#include "codecs.h"
#include "audio.h"
#include "video.h"
#include "text.h"
#include "media.h"
#include "mp4recorder.h"
// Moteur d'ecriture MP4 de libmedkit.
#include "medkit/mp4writer.h"
#include <mp4v2/mp4v2.h>

MP4Recorder::MP4Recorder()
{
	mp4 = MP4_INVALID_FILE_HANDLE;
	writer = NULL;
	recording = false;
	videoTrackAdded = false;
}

MP4Recorder::~MP4Recorder()
{
	//Close just in case (releases writer + file)
	Close();
}

bool MP4Recorder::Create(const char* filename)
{
	Log("-Opening record [%s]\n",filename);

	//If we are already recording
	if (mp4!=MP4_INVALID_FILE_HANDLE)
		//Close
		Close();

	// Le mcu possede le handle (comme MP4Streamer::Open avec MP4Read).
	mp4 = MP4Create(filename,0);

	// If failed
	if (mp4 == MP4_INVALID_FILE_HANDLE)
		//Error
		return Error("-Error opening mp4 file for recording\n");

	// waitVideo=true : on attend la premiere I-frame (comportement historique).
	// ctxdata=NULL : pas de transcodage video interne (le callback video ne sera
	// pas arme, cf. Mp4RecoderVideoCb qui verifie le pointeur).
	writer = new mp4writer(NULL, mp4, true);
	videoTrackAdded = false;

	//Success
	return true;
}

bool MP4Recorder::Record()
{
	//Check mp4 file is opened
	if (mp4 == MP4_INVALID_FILE_HANDLE)
		//Error
		return Error("No MP4 file opened for recording\n");

	//Recording
	recording = true;

	//Exit
	return recording;
}

bool MP4Recorder::Stop()
{
	//not recording anymore
	recording = false;
	return true;
}

bool MP4Recorder::Close()
{
	//Serialise avec onMediaFrame
	std::lock_guard<std::mutex> lock(mutex);

	//Check mp4 file is opened
	if (mp4 == MP4_INVALID_FILE_HANDLE)
	{
		return false;
	}

	//Stop always
	recording = false;

	//Flush + destruction du moteur (le destructeur ecrit les tags MP4)
	if (writer)
	{
		writer->Flush();
		delete writer;
		writer = NULL;
	}

	// Close file
	MP4Close(mp4);
	mp4 = MP4_INVALID_FILE_HANDLE;
	videoTrackAdded = false;

	//NOthing more
	return true;
}

void MP4Recorder::onMediaFrame(MediaFrame &frame)
{
	//Lock the access to the file
	std::lock_guard<std::mutex> lock(mutex);

	//If not recording
	if (recording && writer)
	{
		// mp4writer auto-cree les pistes audio et texte dans ProcessFrame, mais
		// PAS la piste video (ProcessFrame retourne -3 tant qu'elle n'existe pas).
		// On la cree donc explicitement sur la premiere I-frame, avec le codec et
		// les dimensions reels de la trame (comme l'ancien MP4Recorder le faisait).
		if (frame.GetType()==MediaFrame::Video && !videoTrackAdded)
		{
			VideoFrame &videoFrame = (VideoFrame&)frame;
			if (videoFrame.IsIntra())
			{
				writer->AddTrack(videoFrame.GetCodec(),
						 videoFrame.GetWidth(), videoFrame.GetHeight(),
						 256, "video");
				videoTrackAdded = true;
			}
		}

		//libmedkit gere waitVideo, le timing, les pistes et les hint tracks
		writer->ProcessFrame(&frame);
	}
}
