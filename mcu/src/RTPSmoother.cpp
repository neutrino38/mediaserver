/* 
 * File:   RTPSmoother.cpp
 * Author: Sergio
 * 
 * Created on 7 de noviembre de 2011, 12:18
 */

#include "RTPSmoother.h"
#include "audio.h"
#include "video.h"
#include "text.h"
#include "log.h"
#include "tools.h"

RTPSmoother::RTPSmoother()
{
	//NO session
	session = NULL;
	inited = false;
	nextSendUs = 0;
}

RTPSmoother::~RTPSmoother()
{
	//If still inited
	if (inited)
		//End
		End();

	//Clear memory
	while(queue.Length()>0)
		//Delete first
		delete(queue.Pop());
}


int RTPSmoother::Init(RTPSession *session)
{
	//Check if we are already inited
	if (inited)
		//End first
		End();

	//Store it
	this->session = session;

	//We are inited
	inited = true;

	//Réarmer la file après un éventuel End (Cancel collant —
	//l'historique ne le faisait pas : re-Init = boucle folle sur file annulée)
	queue.Reset();

	//Run (réarme le Wait du Worker)
	StartThread();

	return 1;
}

int RTPSmoother::SendFrame(MediaFrame* frame,DWORD duration)
{
	//Check
	if (!frame || !frame->HasRtpPacketizationInfo())
		//Error
		return Error("Frame do not has packetization info");

	if (!inited)
	{
	    // drop packet if not running
	    return 0;
	}
	
	//Get info
	MediaFrame::RtpPacketizationInfo& info = frame->GetRtpPacketizationInfo();

	DWORD codec = 0;
	BYTE *frameData = NULL;
	DWORD frameSize = 0;
	WORD  rate = 1;

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
			//Set default rate
			rate = 8;
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
			//Set default rate
			rate = 90;
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

	DWORD current = 0;
	
	//For each one
	for (int i=0;i<info.size();i++)
	{
		//Get packet
		MediaFrame::RtpPacketization* rtp = info[i];

		//Create rtp packet
		RTPPacketSched *packet = new RTPPacketSched(frame->GetType(),codec);

		//Make sure it is enought length
		if (rtp->GetTotalLength()>packet->GetMaxMediaLength())
		{
			Error("RTP payload too big [%d,%d]\n",rtp->GetTotalLength(),packet->GetMaxMediaLength());
			//Error
			continue;
		}
		
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
		//Set other values
		packet->SetTimestamp(frame->GetTimeStamp()*rate);
		//Check
		if (i+1==info.size())
			//last
			packet->SetMark(true);
		else
			//No last
			packet->SetMark(false);
		//Temps de passage de CE paquet sur le fil, en us : sa part du budget
		//de l'image. La somme sur l'image vaut `duration`, mais c'est le pacer
		//qui les enchaine, donc un depassement se reporte au lieu d'etre perdu.
		packet->SetSendingTime(frameLength ? (DWORD)((QWORD)len*duration*1000/frameLength) : 0);
		//Calculate partial lenght
		current += len;
		//Append it
		queue.Add(packet);
	}

	return 1;
}

int RTPSmoother::Cancel()
{
	//Cancel any pending operation
	queue.Cancel();

	//Cancel waiting
	wait.Cancel();

	//exit
	return 1;
}

int RTPSmoother::End()
{
	//Check
	if (!inited)
		return 0;
	
	//Not inited
	inited = false;

	//Cancel any pending send
	Cancel();

	//Wait
	StopThread();

	return 1;
}

int RTPSmoother::Run()
{
	Log(">RTPSmoother run\n");

	//Curseur du pacer : instant auquel le prochain paquet peut partir
	nextSendUs = getTime();
	QWORD lastWarnUs = 0;
	
	while(inited)
	{
		//Wait for new frame
		if (!queue.Wait(0))
			//Check again
			continue;

		//Get it
		RTPPacketSched *sched = queue.Pop();

		//Check it
		if (!sched)
			//Exit
			continue;

		QWORD now = getTime();

		//Pas de rattrapage en rafale : apres un silence, le curseur ne traine
		//pas dans le passe (sinon toute une image partirait d'un coup).
		if (nextSendUs < now)
			nextSendUs = now;

		//Attendre son tour (annulable). Le curseur garde la verite en us, donc
		//l'arrondi a la milliseconde ne s'accumule pas.
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

		//Send it
		session->SendPacket(*sched,sched->GetTimestamp());

		//Avancer le curseur du temps de passage de ce paquet — c'est ici que la
		//dette se reporte d'une image a l'autre.
		nextSendUs += sched->GetSendingTime();

		//Borne d'avance : au-dela, c'est de la latence pure, on preferre la
		//rafale (et le detecteur de delai la verra, ce qui est correct).
		if (nextSendUs > now + MaxAheadUs)
		{
			nextSendUs = now + MaxAheadUs;
			//La source produit plus vite que le debit de pacing : l'avance est
			//ecretee, donc on emet en rafale. C'est LE signal a lire pour
			//savoir si le budget suffit — au plus une trace par seconde.
			if (now - lastWarnUs > 1000000)
			{
				lastWarnUs = now;
				Log("-RTPSmoother: avance ecretee a %llu ms, la source depasse le debit de pacing [enfiles:%d]\n",
					(QWORD)(MaxAheadUs/1000), queue.Length());
			}
		}

		//DElete it
		delete(sched);
	}

	Log("<RTPSmoother run\n");

	return 1;
}
