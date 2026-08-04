/* 
 * File:   participant.h
 * Author: Sergio
 *
 * Created on 19 de enero de 2012, 18:29
 */

#ifndef PARTICIPANT_H
#define	PARTICIPANT_H

#include <memory>
#include "video.h"
#include "audio.h"
#include "text.h"
#include "rtpsession.h"
#include "medkit/logo.h"
#include "dtmfmessage.h"

class Participant
{
public:
	enum Type { RTP=0,RTMP=1 };
	enum DocSharingMode { NONE=0,BFCP_TCP=1,BFCP_UDP=2};

public:
	class Listener
	{
	public:
		virtual void onRequestFPU(Participant *part) = 0;
		virtual void onDTMF(Participant *part,DTMFMessage* dtmf) = 0;
	};
public:
	Participant(Type type,int partId)
	{
		this->type = type;
		this->partId = partId;
		this->docSharingMode = NONE ;
	}

	virtual ~Participant()
	{
	}
	
	Type GetType()
	{
		return type;
	}
	
	void SetListener(Listener *listener)
	{
		//Store listener
		this->listener = listener;
	}

        virtual int DumpInfo(std::string & info)
        {
            char partInfo[200];
            MediaStatistics s = GetStatistics(MediaFrame::Audio, MediaFrame::VIDEO_MAIN);

            sprintf(partInfo, 
                    "  Type=%s, DocSharing=%s.\n"
                    "  Audio: nb packets rcved %d, nb packets sent %d\n",
                    type == RTP ? "RTP" : "RTMP",
                    (docSharingMode == BFCP_TCP || docSharingMode == BFCP_UDP ) ? "BFCP" : "NONE",
                    s.numRecvPackets, s.numSendPackets);

            info += partInfo;
            return 200;
        }

	DWORD GetPartId()
	{
		return partId;
	}

	virtual int SetVideoCodec(VideoCodec::Type codec,int mode,int fps,int bitrate,int intraPeriod,const Properties &properties, MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN) = 0;
	virtual int SetAudioCodec(AudioCodec::Type codec,const Properties &properties) = 0;
	virtual int SetTextCodec(TextCodec::Type codec) = 0;
	
	
	//Video : DEUX chemins (Point 1 / C-4) — chemin "emprunté" (pointeur brut,
	//pour SharedDocMixer qui passe un objet non alloué par new) et chemin
	//"possédant" (shared_ptr, co-propriété du pipe du mixer). Ne jamais unifier :
	//un shared_ptr possédant sur le SharedDocMixer ferait un delete illégal.
	virtual int SetVideoInput(VideoInput* input,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN) = 0;
	virtual int SetVideoOutput(VideoOutput* output,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN) = 0;
	virtual int SetVideoInput(std::shared_ptr<VideoInput> input,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN) = 0;
	virtual int SetVideoOutput(std::shared_ptr<VideoOutput> output,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN) = 0;
	virtual VideoOutput*  GetVideoOutput(MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN) = 0;
	//Audio/Text : uniquement alimentés par les mixers → shared_ptr direct.
	virtual int SetAudioInput(std::shared_ptr<AudioInput> input) = 0;
	virtual int SetAudioOutput(std::shared_ptr<AudioOutput> output) = 0;
	virtual int SetTextInput(std::shared_ptr<TextInput> input) = 0;
	virtual int SetTextOutput(std::shared_ptr<TextOutput> output) = 0;

	virtual MediaStatistics GetStatistics(MediaFrame::Type media,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN) = 0;
	virtual int SetMute(MediaFrame::Type media, bool isMuted,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN) = 0;
	virtual int SendVideoFPU(MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN) = 0;
	virtual int SendDTMF(DTMFMessage* dtmf) = 0;
	
	virtual int Init() = 0;
	virtual int End() = 0;
	
	virtual int AcceptDocSharingRequest(int confId,int partId) 	{ return 0; };
	virtual int RefuseDocSharingRequest(int confId,int partId)	{ return 0; };
	virtual int StopDocSharing(int confId,int partId)			{ return 0; };
	
	int LoadLogo(const char * filename) { logo = Pict::Load(filename); return logo ? 1 : 0; }
	void SetDocSharingMode(DocSharingMode mode) { docSharingMode = mode; }
	DocSharingMode GetDocSharingMode() { return docSharingMode; }

	Use		use;
protected:
	Type type;
	DocSharingMode docSharingMode;
	Listener *listener;
	DWORD partId;
	PictPtr logo;
	//Use		use;
};

typedef std::shared_ptr<Participant> ParticipantPtr;

#endif	/* PARTICIPANT_H */

