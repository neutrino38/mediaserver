/* 
 * File:   RTPMultiplexerSmoother.cpp
 * Author: Sergio
 * 
 * Created on 7 de noviembre de 2011, 12:18
 */

#include "RTPMultiplexerSmoother.h"
#include "audio.h"
#include "video.h"
#include "text.h"
#include "log.h"
#include "tools.h"

RTPMultiplexerSmoother::RTPMultiplexerSmoother() : RTPMultiplexer()
{
	//NO session
	inited = false;
	nextSendUs = 0;
	//Un SSRC dès la construction : SmoothFrame peut précéder Start()
	ssrc = random();
}

RTPMultiplexerSmoother::~RTPMultiplexerSmoother()
{
	//End
	Stop();

	//Clear memory
	while(queue.Length()>0)
		//Delete first
		delete(queue.Pop());
}


int RTPMultiplexerSmoother::Start()
{
	Log("-RTPMultiplexerSmoother start\n");
	
	//Check if we are already inited
	if (inited)
		//End first
		Stop();
	
	//We are inited
	inited = true;
	//Nouveau run d'encodage = nouvelle base de temps : SSRC neuf (cf. .h)
	ssrc = random();
	//Réarmer la file après un éventuel Stop (Cancel collant)
	queue.Reset();
	//Run (réarme le Wait du Worker)
	StartThread();

	return 1;
}

int RTPMultiplexerSmoother::SmoothFrame(MediaFrame* frame,DWORD duration)
{
	//Check
	if (!frame || !frame->HasRtpPacketizationInfo())
		//Error
		return Error("Frame do not has packetization info");

	//Get info
	MediaFrame::RtpPacketizationInfo& info = frame->GetRtpPacketizationInfo();

	DWORD codec = 0;
	BYTE *frameData = NULL;
	DWORD frameSize = 0;

	//Depending on the type
	switch(frame->GetType())
	{
		case MediaFrame::Audio:
		{
			//get audio frame
			AudioFrame * audio = (AudioFrame*)frame;
			//Get codec
			codec = audio->GetCodec();
			//Get data
			frameData = audio->GetData();
			//Get size
			frameSize = audio->GetLength();
		}
			break;
		case MediaFrame::Video:
		{
			//get Video frame
			VideoFrame * video = (VideoFrame*)frame;
			//Get codec
			codec = video->GetCodec();
			//Get data
			frameData = video->GetData();
			//Get size
			frameSize = video->GetLength();
		}
			break;
		default:
			return Error("No smoother for frame");
	}

	DWORD frameLength = 0;
	//Calculate total length
	for (int i=0;i<info.size();i++)
		//Get total length
		frameLength += info[i]->GetTotalLength();

	//Borne de latence sur l'etalement de cette image (cf. MaxSpreadUs)
	if ((QWORD)duration*1000 > MaxSpreadUs)
		duration = (DWORD)(MaxSpreadUs/1000);

	//Calculate bitrate for frame
	DWORD current = 0;
	
	//For each one
	for (int i=0;i<info.size();i++)
	{
		//Get packet
		MediaFrame::RtpPacketization* rtp = info[i];

		//Create rtp packet
		RTPPacketSched *packet = new RTPPacketSched(frame->GetType(),codec);
		//L'identité de source du run d'encodage courant (cf. .h)
		packet->SetSSRC(ssrc);

		//Make sure it is enought length
		if (rtp->GetPrefixLen()+rtp->GetSize()>packet->GetMaxMediaLength())
			//Error
			continue;
		
		//Get pointer to media data
		BYTE* out = packet->GetMediaData();
		//Copy prefic
		memcpy(out,rtp->GetPrefixData(),rtp->GetPrefixLen());
		//Copy data
		memcpy(out+rtp->GetPrefixLen(),frameData+rtp->GetPos(),rtp->GetSize());
		//Set length
		DWORD len = rtp->GetPrefixLen()+rtp->GetSize();
		//Set length
		packet->SetMediaLength(len);
		switch(packet->GetMedia())
		{
			case MediaFrame::Video:
				//Set timestamp
				packet->SetTimestamp(frame->GetTimeStamp()*90);
				break;
			case MediaFrame::Audio:
				//Set timestamp
				packet->SetTimestamp(frame->GetTimeStamp()*8);
				break;
			default:
				//Set timestamp
				packet->SetTimestamp(frame->GetTimeStamp());
		}
		//Check
		if (i+1==info.size())
			//last
			packet->SetMark(true);
		else
			//No last
			packet->SetMark(false);
		//Temps de passage de CE paquet sur le fil, en us (cf. RTPSmoother)
		packet->SetSendingTime(frameLength ? (DWORD)((QWORD)len*duration*1000/frameLength) : 0);
		//Calculate partial lenght
		current += len;
		//Append it
		queue.Add(packet);
	}

	return 1;
}

int RTPMultiplexerSmoother::Cancel()
{
	//Cancel any pending operation
	queue.Cancel();

	//Cancel waiting
	wait.Cancel();

	//exit
	return 1;
}

int RTPMultiplexerSmoother::Stop()
{
	//Check
	if (!inited)
		return 0;

	Log(">RTPMultiplexerSmoother stop\n");
	
	//Not inited
	inited = false;

	//Cancel any pending send
	Cancel();

	//Wait
	StopThread();

	Log("<RTPMultiplexerSmoother stopped\n");

	return 1;
}

int RTPMultiplexerSmoother::Run()
{
	Log(">RTPMultiplexerSmoother run\n");

	//Curseur du pacer : instant auquel le prochain paquet peut partir
	nextSendUs = getTime();
	QWORD lastWarnUs = 0;
	
	while(inited)
	{
		//Wait for new frame
		if (!queue.Wait(0))
		{
			msleep(200);
			//Check again
			continue;
		}
		//Get it
		RTPPacketSched *sched = queue.Pop();

		//Check it
		if (!sched)
		{
			msleep(200);
			//Exit
			continue;
		}
		QWORD now = getTime();

		//Pas de rattrapage en rafale apres un silence
		if (nextSendUs < now)
			nextSendUs = now;

		//Attendre son tour (annulable) ; le curseur garde la verite en us
		if (nextSendUs > now)
		{
			QWORD waitMs = (nextSendUs - now)/1000;
			if (waitMs)
				wait.WaitSignal(waitMs);
			//Wait annulé : ne pas émettre sur une file en cours d'arrêt
			if (!inited)
			{
				delete(sched);
				break;
			}
			now = getTime();
		}

		//Multiplex
		Multiplex(*(RTPPacket*)sched);

		//Avancer le curseur : c'est ici que la dette se reporte
		nextSendUs += sched->GetSendingTime();

		//Borne d'avance : au-dela c'est de la latence pure
		if (nextSendUs > now + MaxAheadUs)
		{
			nextSendUs = now + MaxAheadUs;
			//Meme signal que dans RTPSmoother, au plus une trace par seconde
			if (now - lastWarnUs > 1000000)
			{
				lastWarnUs = now;
				Log("-RTPMultiplexerSmoother: avance ecretee a %llu ms, la source depasse le debit de pacing [enfiles:%d]\n",
					(QWORD)(MaxAheadUs/1000), queue.Length());
			}
		}

		//DElete it
		delete(sched);
	}

	Log("<RTPMultiplexerSmoother run\n");

	return 1;
}
