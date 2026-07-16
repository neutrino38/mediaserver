#ifndef _MP4RECORDER_H_
#define _MP4RECORDER_H_

#include <mutex>
#include <mp4v2/mp4v2.h>

#include "config.h"
#include "medkit/codecs.h"
#include "audio.h"
#include "video.h"
#include "text.h"
#include "media.h"
#include "recordercontrol.h"

// Moteur d'ecriture MP4 de libmedkit. Declaration en avant pour ne pas imposer
// <medkit/mp4writer.h> (ni <mp4v2/...> au-dela de ce qui precede) a tous les
// consommateurs de cet en-tete, exactement comme mp4streamer.h le fait pour
// mp4reader.
class mp4writer;

// MP4Recorder est desormais une fine coquille : elle implemente les interfaces
// mcu (RecorderControl + MediaFrame::Listener) et delegue toute l'ecriture (
// creation des pistes, timing, attente d'I-frame, hint tracks, sous-titres,
// tags) au moteur mp4writer de libmedkit. Le mcu possede le MP4FileHandle
// (ouvert/ferme ici), comme MP4Streamer possede le sien.
class MP4Recorder :
	public RecorderControl,
	public MediaFrame::Listener
{
public:
	MP4Recorder();
	~MP4Recorder();

	//Recorder interface
	virtual bool Create(const char *filename);
	virtual bool Record();
	virtual bool Stop();
	virtual bool Close();

	virtual RecorderControl::Type GetType()	{ return RecorderControl::MP4;	}

	virtual void onMediaFrame(MediaFrame &frame);

	// A appeler avant Create() : si false, l'audio/texte s'enregistre sans
	// attendre la premiere I-frame video (appels sans video notamment).
	void SetWaitVideo(bool wait)	{ waitVideo = wait; }
private:

	MP4FileHandle	mp4;
	mp4writer*	writer;		// moteur d'ecriture libmedkit
	bool		recording;
	bool		waitVideo = true;	// attendre la 1re I-frame avant d'ecrire
	bool		videoTrackAdded;	// piste video creee (non auto-creee par mp4writer)
	std::mutex	mutex;		// serialise onMediaFrame vs Close
};
#endif
