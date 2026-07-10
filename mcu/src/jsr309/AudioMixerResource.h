/* 
 * File:   AudioMixerResource.h
 * Author: Sergio
 *
 * Created on 13 de septiembre de 2011, 1:07
 */

#ifndef AUDIOMIXERRESOURCE_H
#define	AUDIOMIXERRESOURCE_H

#include "config.h"
#include "audiomixer.h"
#include "Joinable.h"
#include "AudioEncoderWorker.h"
#include "AudioDecoderWorker.h"
#include <string>

class AudioMixerResource
{
public:
	class Port
	{
	public:
		Port(std::wstring &tag)
		{
			this->tag = tag;
		}
		std::wstring	tag;
		AudioEncoderMultiplexerWorker encoder;
		AudioDecoderJoinableWorker decoder;
	};

public:
	AudioMixerResource(std::wstring &name);
	virtual ~AudioMixerResource();

	int Init();
	int CreatePort(std::wstring &tag);
	int SetPortCodec(int portId,AudioCodec::Type codec);
	int DeletePort(int portId);
	int End();
	//Get joinables
	std::shared_ptr<Joinable> GetJoinable(int portId);
	//Port Attach  to
	int Attach(int portId,const std::shared_ptr<Joinable> &);
	int Dettach(int portId);

private:
	//Port détenu par shared_ptr : GetJoinable rend une vue aliasing sur son
	//`encoder`, ce qui laisse le weak_ptr `joined` du listener attaché expirer
	//proprement quand le port est supprimé (C-13, lien A).
	typedef std::map<int,std::shared_ptr<Port>> Ports;

private:
	std::wstring tag;
	AudioMixer mixer;
	Ports ports;
	int maxId;
	bool inited;
	
};

#endif	/* AUDIOMIXERRESOURCE_H */

