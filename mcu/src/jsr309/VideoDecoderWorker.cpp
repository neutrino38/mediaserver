/* 
 * File:   VideoDecoderWorker.cpp
 * Author: Sergio
 * 
 * Created on 2 de noviembre de 2011, 23:38
 */

#include "VideoDecoderWorker.h"
#include "rtp.h"
#include "log.h"

VideoDecoderJoinableWorker::VideoDecoderJoinableWorker()
{
	//Nothing
	output = NULL;
	input = NULL;
	decoding = false;
        videoDecoder = NULL;
}

VideoDecoderJoinableWorker::~VideoDecoderJoinableWorker()
{
	End();
}

int VideoDecoderJoinableWorker::Init(VideoOutput *output)
{
	//Store it
	this->output = output;
	return 0;
}

int VideoDecoderJoinableWorker::End()
{
	//Dettach
	Dettach();

	//Set null
	output = NULL;
	return 0;
}

int VideoDecoderJoinableWorker::Start()
{
	Log("-StartVideoDecoderJoinableWorker\n");

	//Check
	if (!output)
		//Exit
		return Error("null video output");

	//Check if need to restart
	if (decoding)
		//Stop first
		Stop();

        setZeroTime(&lastFPURequest);
	lostCount=0;
	frameSeqNum = RTPPacket::MaxExtSeqNum;
	lastSeq = RTPPacket::MaxExtSeqNum;
	waitIntra = false;

	//Ouvrir le chemin : desormais onRTPPacket decode lui-meme, sur le thread
	//de la source.
	decoding = 1;

	return 1;
}


int  VideoDecoderJoinableWorker::Stop()
{
	Log(">StopVideoDecoderJoinableWorker\n");

	//If we were started
	if (decoding)
	{
		//Stop
		decoding=0;

		//Borramos el decoder
		if (videoDecoder)
		{
			delete videoDecoder;
			videoDecoder = NULL;
		}
	}

	Log("<StopVideoDecoderJoinableWorker\n");

	return 1;
}

void VideoDecoderJoinableWorker::DecodePacket(RTPPacket &packet)
{
        //Get extended sequence number
        DWORD seq = packet.GetExtSeqNum();

        //Get packet data
        BYTE* buffer = packet.GetMediaData();
        DWORD size = packet.GetMediaLength();

        //Get type
        type = (VideoCodec::Type)packet.GetCodec();

        //Lost packets since last
        DWORD lost = 0;
        //If not first
        if (lastSeq!=RTPPacket::MaxExtSeqNum)
                //Calculate losts
                lost = seq-lastSeq-1;

        //Increase total lost count
        lostCount += lost;

        //Update last sequence number
        lastSeq = seq;

        //Si hemos perdido un paquete or still have not got an iframe
        if(lostCount>1 || waitIntra)
        {
                //Check if we got listener and more than two seconds have elapsed from last request
                if (getDifTime(&lastFPURequest)>1000000)
                {
                        //lock() : source encore vivante ?
                        if (std::shared_ptr<Joinable> j = joined.lock())
                        {
                                //Debug
                                Log("-Requesting FPU lost %d\n",lostCount);
                                //Reset count
                                lostCount = 0;
                                //Request also over rtp
                                j->Update();
                                //Update time
                                getUpdDifTime(&lastFPURequest);
                                //Waiting for refresh
                                waitIntra = true;
                        }
                }
        }

        //Check if it is a redundant packet
        if (type==VideoCodec::RED)
        {
                //Get redundant packet
                RTPRedundantPacket* red = (RTPRedundantPacket*)&packet;
                //Get primary codec
                type = (VideoCodec::Type)red->GetPrimaryCodec();
                //Check it is not ULPFEC redundant packet
                if (type==VideoCodec::ULPFEC)
                        //Skip
                        return;
                //Update primary redundant payload
                buffer = red->GetPrimaryPayloadData();
                size = red->GetPrimaryPayloadSize();
        }

        //Comprobamos el tipo
        if ((videoDecoder==NULL) || (type!=videoDecoder->type))
        {
                //Si habia uno nos lo cargamos
                if (videoDecoder!=NULL)
                        delete videoDecoder;

                //Creamos uno dependiendo del tipo
                videoDecoder = VideoCodecFactory::CreateDecoder(type);

                //Si es nulo
                if (videoDecoder==NULL)
                {
                        Error("Error creando nuevo decodificador de video [%p,%d]\n",this,type);
                        //Next
                        return;
                }
        }

        //Check if we have lost the last packet from the previous frame
        if (seq>frameSeqNum)
        {
                //Try to decode what is in the buffer
                videoDecoder->DecodePacket(NULL,0,1,1);
                //Get picture
                PictPtr frame = videoDecoder->GetFrame();
                DWORD width = videoDecoder->GetWidth();
                DWORD height = videoDecoder->GetHeight();
                //Check values
                if (frame && width && height)
                {
                        //Set frame size
                        output->SetVideoSize(width,height);
                        //Horodatage RTP de la source porte par l'image : c'est sur
                        //lui, et non sur l'heure d'arrivee, que le transcodeur mesure
                        //la cadence reelle (§3.6). FfVideoDecoder::GetFrame ne le
                        //renseigne pas.
                        frame->GetAVFrame()->pts = packet.GetTimestamp();
                        //Send it
                        output->NextFrame(frame);
                }
        }


        //Lo decodificamos
        if(!videoDecoder->DecodePacket(buffer,size,lost,packet.GetMark()))
        {
                //Check if we got listener and more than two seconds have elapsed from last request
                if (getDifTime(&lastFPURequest)>1000000)
                {
                    if (std::shared_ptr<Joinable> j = joined.lock())
                    {
                        //Debug
                        Log("-Requesting FPU decoder error\n");
                        //Reset count
                        lostCount = 0;
                        //Request also over rtp
                        j->Update();
                        //Update time
                        getUpdDifTime(&lastFPURequest);
                        //Waiting for refresh
                        waitIntra = true;
                    }
                }
                //Next frame
                return;
        }

	//if ( (lastSeq % 20) == 0 ) Log("VideoDecoder: decoded frame codec %d.\n", videoDecoder->type );
        //Si es el ultimo
        if(packet.GetMark())
        {
                if (videoDecoder->IsKeyFrame())
                        Debug("-Got Intra\n");

                //No seq number for frame
                frameSeqNum = RTPPacket::MaxExtSeqNum;

                //Get picture
                PictPtr frame = videoDecoder->GetFrame();
                DWORD width = videoDecoder->GetWidth();
                DWORD height = videoDecoder->GetHeight();
                //Check values
                if (frame && width && height)
                {
                        //Set frame size
                        output->SetVideoSize(width,height);

                        //Horodatage RTP de la source (cf. plus haut)
                        frame->GetAVFrame()->pts = packet.GetTimestamp();

                        //Send it
                        output->NextFrame(frame);
                }
                //Check if we got the waiting refresh
                if (waitIntra && videoDecoder->IsKeyFrame())
                        //Do not wait anymore
                        waitIntra = false;
        }
}

void VideoDecoderJoinableWorker::onRTPPacket(RTPPacket &packet)
{
	//Décoder ici même : on est sur le thread de la source, sous son verrou de
	//multiplexage. Plus de copie du paquet, plus de file, plus de thread.
	if (decoding)
		DecodePacket(packet);
}

void VideoDecoderJoinableWorker::onResetStream()
{
	//Plus rien en attente à jeter : le paquet est consommé au retour de
	//onRTPPacket.
}

void VideoDecoderJoinableWorker::onEndStream()
{
	//Stop decoding
	Stop();
	//Not joined anymore
	joined.reset();
}

int VideoDecoderJoinableWorker::Attach(const std::shared_ptr<Joinable> & join)
{
	//Detach if joined — lock() : source encore vivante ?
	if (std::shared_ptr<Joinable> j = joined.lock())
	{
		//Retrait AVANT arrêt : c'est RemoveListener qui est la barrière. Il ne
		//rend la main que le Multiplex en cours terminé, donc plus aucun
		//onRTPPacket n'est en vol quand Stop() touche au décodeur.
		j->RemoveListener(this);
		//Stop
		Stop();
	}
	//Store new one (lien retour non possédant)
	joined = join;
	//If it is not null
	if (join)
	{
		//Start
		Start();
		//Join to the new one
		join->AddListener(this);
	}
	//OK
	return 1;
}

int VideoDecoderJoinableWorker::Dettach()
{
        //Detach if joined — lock() : ne déréférence pas si la source a disparu
	if (std::shared_ptr<Joinable> j = joined.lock())
	{
		//Même barrière qu'Attach : le retrait précède l'arrêt, sinon Stop()
		//détruirait le décodeur pendant qu'un onRTPPacket en vol l'utilise.
		//En mode pont le décodeur n'est pas inscrit (SetSource seul) : le
		//retrait ne fait rien, mais prendre le verrou de la source reste la
		//barrière — c'est UnlistenSource du transcodeur qui a déjà coupé.
		j->RemoveListener(this);
		//Stop decoding
		Stop();
	}
	else
		//Stop decoding même si la source est déjà partie
		Stop();

	//Not joined anymore
	joined.reset();
	return 0;
}
