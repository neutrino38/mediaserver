/* 
 * File:   Recorder.cpp
 * Author: Sergio
 * 
 * Created on 26 de febrero de 2012, 16:50
 */

#include "Recorder.h"
#include "log.h"

Recorder::Recorder(std::wstring tag)
{
	//Store tag
	this->tag = tag;
	//NO audio or video
	audio = NULL;
	video = NULL;
}

Recorder::~Recorder()
{
	//On se désinscrit des sources encore attachées AVANT destruction : sinon
	//chaque source garderait un Listener* pendouillant dans son set et
	//planterait (« pure virtual method called ») au Multiplex/destruction
	//suivant (C-13, sens inverse — RecorderDelete ne détache pas avant de
	//libérer le recorder). Le lock() ignore les sources déjà détruites : leur
	//weak_ptr a expiré, elles ne sont plus dans aucun set (lien A).
	for (JoinedMap::iterator it = joined.begin(); it!=joined.end(); ++it)
		if (std::shared_ptr<Joinable> j = it->second.lock())
			j->RemoveListener(this);
	joined.clear();

	//If we have an audio depacketizer
	if (audio)
		//Delete it
		delete(audio);
	//If we have an video depacketizer
	if (video)
		//Delete it
		delete(video);
}

void Recorder::onRTPPacket(RTPPacket &packet)
{
	//Check type
	switch(packet.GetMedia())
	{
		case MediaFrame::Audio:
			break;
		case MediaFrame::Video:
			//Do we have video depacketizer
			if (!video)
				//Create new
				video = RTPDepacketizer::Create(packet.GetMedia(),packet.GetCodec());
			//Check again
			if (video)
			{
				//Append to frame
				VideoFrame *frame = (VideoFrame*)video->AddPacket(&packet);
				//Is it last
				if (packet.GetMark())
				{
					//If got frame
					if (frame)
						//Record frame
						onMediaFrame(*frame);
					//Clear frame
					video->ResetFrame();
				}
			}
			break;
		case MediaFrame::Text:
			break;
	}

}

void Recorder::onResetStream()
{
	//Do nothing by now
}

void Recorder::onEndStream()
{
	//Do nothing by now
}

//Attach
int Recorder::Attach(MediaFrame::Type media, const std::shared_ptr<Joinable> & join)
{
	Log("-Endpoint attaching [media:%d]\n",media);

	//Get joined
	JoinedMap::iterator it = joined.find(media);

	//Detach if joined — lock() : source encore vivante ?
	if (it!=joined.end())
	{
		//Remove ourself as listeners
		if (std::shared_ptr<Joinable> j = it->second.lock())
			j->RemoveListener(this);
		//Remove from map
		joined.erase(it);
	}

	//If it is not null
	if (join)
	{
		//Set in map (lien retour non possédant)
		joined[media] = join,
		//Join to the new one
		join->AddListener(this);
	}

	return 1;
}

int Recorder::Dettach(MediaFrame::Type media)
{
	Log("-Endpoint detaching [media:%d]\n",media);

	//Get joined
	JoinedMap::iterator it = joined.find(media);

	//Detach if joined — lock() : ne déréférence pas si la source a disparu
	if (it!=joined.end())
	{
		//Remove ourself as listeners
		if (std::shared_ptr<Joinable> j = it->second.lock())
			j->RemoveListener(this);
		//Remove from map
		joined.erase(it);
	}

	return 1;
}
