/* 
 * File:   VideoMixerResource.h
 * Author: Sergio
 *
 * Created on 2 de noviembre de 2011, 23:38
 */

#ifndef VIDEOMIXERRESOURCE_H
#define	VIDEOMIXERRESOURCE_H

#include "config.h"
#include "mosaic.h"
#include "videomixer.h"
#include "Joinable.h"
#include "VideoEncoderWorker.h"
#include "VideoDecoderWorker.h"
#include <string>

class VideoMixerResource
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
		VideoEncoderMultiplexerWorker encoder;
		VideoDecoderJoinableWorker decoder;
	};

public:
	VideoMixerResource(std::wstring &name);
	virtual ~VideoMixerResource();

	int Init(Mosaic::Type comp,int size);
	int CreatePort(std::wstring &tag,int mosaicId);
	int SetPortCodec(int portId,VideoCodec::Type codec,int mode,int fps,int bitrate,int intraPeriod);
	int DeletePort(int portId);
	int CreateMosaic(Mosaic::Type comp,int size);
	int AddMosaicParticipant(int mosaicId,int portId);
	int RemoveMosaicParticipant(int mosaicId,int portId);
	int SetSlot(int mosaicId,int num,int id);
	int SetCompositionType(int mosaicId,Mosaic::Type comp,int size);
	int SetOverlayPNG(int mosaicId,const char* overlay);
	int ResetOverlay(int mosaicId);
	int DeleteMosaic(int mosaicId);
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
	VideoMixer mixer;
	Ports ports;
	int maxId;
	bool inited;
};

#endif	/* VIDEOMIXERRESOURCE_H */

