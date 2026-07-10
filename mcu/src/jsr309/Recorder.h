/* 
 * File:   Recorder.h
 * Author: Sergio
 *
 * Created on 26 de febrero de 2012, 16:50
 */

#ifndef RECORDER_H
#define	RECORDER_H

#include "config.h"
#include <string>
#include <memory>
#include "Joinable.h"
#include "mp4recorder.h"
#include <map>

class Recorder :
	public MP4Recorder,
	public Joinable::Listener
{
public:
	Recorder(std::wstring tag);
	virtual ~Recorder();

	//Joinable::Listener
	virtual void onRTPPacket(RTPPacket &packet);
	virtual void onResetStream();
	virtual void onEndStream();

	//Attach
	int Attach(MediaFrame::Type media, const std::shared_ptr<Joinable> & join);
	int Dettach(MediaFrame::Type media);

	std::wstring& GetTag() { return tag; }
private:
	//Liens retour NON possédants vers les sources : weak_ptr → lock() au site
	//d'usage. Une source détruite avant nous fait échouer le lock() (le Dettach/
	//~Recorder ultérieur ne déréférence pas d'objet libéré) — C-13, lien A.
	typedef std::map<MediaFrame::Type,std::weak_ptr<Joinable>> JoinedMap;
private:
	std::wstring tag;
	RTPDepacketizer* video;
	RTPDepacketizer* audio;
	JoinedMap	 joined;
};

#endif	/* RECORDER_H */

