/* 
 * File:   VideoEncoderWorker.cpp
 * Author: Sergio
 * 
 * Created on 2 de noviembre de 2011, 23:37
 */

#include "VideoEncoderWorker.h"
#include "log.h"
#include "tools.h"
#include "RTPMultiplexer.h"
#include "acumulator.h"
//AV1Encoder::ClampToLevel (écrêtage cadence/taille, phase 5b nego_fmtp)
#include "av1/av1codec.h"

VideoEncoderMultiplexerWorker::VideoEncoderMultiplexerWorker() : RTPMultiplexerSmoother()
{
	//Nothing
	input = NULL;
	encoding = false;
	sendFPU = false;
	codec = (VideoCodec::Type)-1;
        useInputSize = false;
}

VideoEncoderMultiplexerWorker::~VideoEncoderMultiplexerWorker()
{
	End();
}

int VideoEncoderMultiplexerWorker::Init(VideoInput *input)
{
	//Store it
	this->input = input;
	return 0;
}

int VideoEncoderMultiplexerWorker::SetCodec(VideoCodec::Type codec,int mode,int fps,int bitrate,int intraPeriod, Properties & properties)
{
	Log("-SetVideoCodec [%s=%d, fps:%d, bitrate:%d kbps, intra:%d]\n",VideoCodec::GetNameFor(codec),codec,fps,bitrate,intraPeriod);
	if (fps <= 0)
		return Error("Invalid value for fps:%d.\n", fps);

	//Store parameters
	this->codec	  = codec;
	this->mode	  = mode;
	this->bitrate	  = bitrate;
	this->fps	  = fps;
	this->configuredFps = fps;
	this->intraPeriod = intraPeriod;
	//Init limits
	this->videoBitrateLimit		= bitrate;
	this->videoBitrateLimitCount	= fps;
	params = properties;

	Stop();

	//Get width and height
	width = GetWidth(mode);
	height = GetHeight(mode);

	//Check size
	if (!width || !height)
		//Error
		return Error("Unknown video mode\n");

	//Check if we are already encoding
	if (!listeners.empty())
	{
		//And start
		Log("-VideoEncoder: restarted encoder.\n");
		Start();
	}

	//Exit
	return 1;
}

//Phase 5 (nego_fmtp §6.3) : les bornes que la négociation SDP de la patte
//émettrice impose à l'encodeur. Les Properties ne sont lues qu'à CreateEncoder,
//donc des bornes qui changent sur un encodeur ouvert exigent un cycle
//Stop/Start — le même que SetCodec, au prix d'un IDR frais, ce qui est
//précisément ce qu'un changement de profil exige de toute façon.
void VideoEncoderMultiplexerWorker::SetNegotiatedCodecProperties(const std::map<int,Properties>& byCodec)
{
	//Bornes identiques : ne pas redémarrer l'encodeur pour rien (chaque push
	//re-signalisation/attach/StartSending repasse ici).
	if (negotiated == byCodec)
		return;

	negotiated = byCodec;

	Log("-VideoEncoder: negotiated codec properties updated [%d codec(s)]\n",
	    (int)byCodec.size());

	//Même logique de reprise que SetCodec : un encodeur ouvert ré-ouvre avec
	//les bornes à jour, un encodeur pas encore démarré les lira au Start().
	Stop();
	if (!listeners.empty() && codec != (VideoCodec::Type)-1)
	{
		Log("-VideoEncoder: restarted encoder with negotiated properties.\n");
		Start();
	}
}

int VideoEncoderMultiplexerWorker::Start()
{
	//Check
	if (!input)
		//Exit
		return Error("null video input");
	
	//Check if need to restart
	if (encoding)
		//Stop first
		Stop();

	//Start smoother
	RTPMultiplexerSmoother::Start();

	//Start decoding
	encoding = 1;

	//launc thread
	createPriorityThread(&thread,startEncoding,this,0);

	return 1;
}

void * VideoEncoderMultiplexerWorker::startEncoding(void *par)
{
	Log("VideoEncoderMultiplexerWorkerThread [%d]\n",getpid());
	//Get worker
	VideoEncoderMultiplexerWorker *worker = (VideoEncoderMultiplexerWorker *)par;
	//Block all signals
	blocksignals();
	//Run
	worker->Encode();
	//Exit
	return NULL;
}

int VideoEncoderMultiplexerWorker::Stop()
{
	Log(">Stop VideoEncoderMultiplexerWorker\n");

	//If we were started
	if (encoding)
	{
		//Stop
		encoding=0;

		//Cancel and frame grabbing
		input->CancelGrabFrame();

		//Cancel sending
		pacer.Signal();

		//Esperamos
		pthread_join(thread,NULL);
	}

	//Stop smoother
	RTPMultiplexerSmoother::Stop();

	Log("<Stop VideoEncoderMultiplexerWorker\n");

	return 1;
}

int VideoEncoderMultiplexerWorker::End()
{
	//Check if already decoding
	if (encoding)
		//Stop
		Stop();

	//Set null
	input = NULL;
	return 0;
}

void VideoEncoderMultiplexerWorker::AddListener(Listener *listener)
{
	//Check if we were already encoding
	if (listener && !encoding && codec != -1)
		//Start encoding;
		Start();
	//Add the listener
	RTPMultiplexer::AddListener(listener);
}

void VideoEncoderMultiplexerWorker::RemoveListener(Listener *listener)
{
	//Remove the listener
	RTPMultiplexer::RemoveListener(listener);
	//If there are no more
	if (listeners.empty())
		//Stop encoding
		Stop();
}

void VideoEncoderMultiplexerWorker::Update()
{
	//Sedn FPU
	sendFPU = true;
}

void VideoEncoderMultiplexerWorker::SetREMB(int estimation)
{
	//Set bitrate limit
	videoBitrateLimit = estimation/1000;
	//Set limit of bitrate to 1 second;
	videoBitrateLimitCount = fps;
}

int VideoEncoderMultiplexerWorker::Encode()
{
	timeval first;
	timeval prev;
	DWORD num = 0;
	QWORD overslept = 0;

	Acumulator bitrateAcu(1000);
	Acumulator fpsAcu(1000);
	VideoEncoder* videoEncoder = NULL;

	//Phase 5 (nego_fmtp §6.3) : les bornes négociées de la patte émettrice
	//écrasent la config du contrôleur pour CE codec — le pair a déclaré ce
	//qu'il sait décoder (profil H.264, packetization-mode, niveau AV1), et
	//émettre au-dessus produit un flux négocié avec succès et décodé par
	//personne. Fusionné AVANT la capture : l'écrêtage AV1 s'applique à elle.
	Properties effective = params;
	std::map<int,Properties>::const_iterator itNeg = negotiated.find((int)codec);
	if (itNeg != negotiated.end())
	{
		for (Properties::const_iterator it = itNeg->second.begin(); it != itNeg->second.end(); ++it)
			effective[it->first] = it->second;

		Log("-VideoEncoder: opening with negotiated properties for %s [%d key(s)]\n",
		    VideoCodec::GetNameFor(codec), (int)itNeg->second.size());
	}

	//Phase 5b : écrêtage cadence/taille au niveau AV1 déclaré par le pair
	//(annexe A.3 — décidé le 2026-08-06 : écrêter, jamais refuser la vidéo).
	//Repart de la CONFIG à chaque (ré)ouverture, pour suivre aussi une
	//re-négociation qui assouplit la borne.
	width  = GetWidth(mode);
	height = GetHeight(mode);
	fps    = configuredFps;

	if (codec == VideoCodec::AV1)
		AV1Encoder::ClampToLevel(effective, width, height, fps);

	Log(">SendVideo [width:%d,size:%d,bitrate:%d,fps:%d,intra:%d]\n",width,height,bitrate,fps,intraPeriod);

	//Comrpobamos que tengamos video de entrada
	if (input == NULL)
		return Error("No video input");

	//Iniciamos el tama�o del video
	if (!input->StartVideoCapture(width,height,fps))
		return Error("Couldn't set video capture\n");

	//Start at 80%
	int current = bitrate;

	//No wait for first
	QWORD frameTime = 0;

	//L'encodeur n'est PAS créé ici, mais à la première image réellement
	//capturée (plus bas dans la boucle).
	//
	//En mode pont (VideoTranscoder::onRTPPacket, state == 2) les paquets sont
	//relayés tels quels : le décodeur n'est jamais appelé, le pipe ne reçoit
	//donc aucune image, et GrabFrame(0) — attente infinie — gare ce thread
	//sans consommer de CPU. Ouvrir l'encodeur d'avance revenait à instancier
	//un codec que ce mode n'utilisera jamais. Sur un appel AV1 <-> AV1, le cas
	//précis où les deux pattes pontent, c'étaient deux encodeurs SVT-AV1
	//ouverts pour rien — et le crash de libSvtAv1Enc 0.9.0 avec, puisque leurs
	//init/deinit se percutent (cf. libmedikit medkit/ffcodeclock.h).
	//
	//Le mode n'est arbitré qu'au PREMIER PAQUET RTP reçu, donc après le
	//démarrage de ce thread : impossible de le consulter ici. La présence
	//d'une image dans le pipe est le signal juste, et il est déjà disponible.

	//The time of the first one
	gettimeofday(&first,NULL);
	prev = first;

	//Started
	Log("-Sending video\n");

	//Mientras tengamos que capturar
	while (encoding)
	{
		//Nos quedamos con el puntero antes de que lo cambien
		PictPtr pic;

                //`videoEncoder` peut être nul : il n'est créé qu'à la première
                //image (cf. plus haut), et ce bloc le déréférence.
                if (videoEncoder && useInputSize && input->HasNativeSizeChanged() )
                {
                    DWORD nativeWidth = input->GetNativeWidth();
                    DWORD nativeHeight = input->GetNativeHeight();

                    if ( nativeWidth > 0 && nativeHeight > 0
                            &&
                         (height != nativeHeight || width != nativeWidth ))
                    {
                        // If native size has changed, try to reconfigure the codec first
                        if ( videoEncoder->SetSize(nativeWidth,nativeHeight) )
                        {
                            // Ok - the codec accepted the change. Now change the capture ..
                            if ( input->StartVideoCapture(nativeWidth,nativeHeight,fps) )
                            {
                                width = nativeWidth;
                                height = nativeHeight;
                                Log("-VideoEncoder: adjusted to new native size %u x %u.\n", width, height);
                            }
                            else
                            {

                                // We could not start the capture - revert to previous codec settings
                                Error("-VideoEncoder: failed to restart capture with new size %d x %d .\n",
                                    nativeWidth, nativeHeight);
                                videoEncoder->SetSize(width,height);
                            }
                        }
                        else
                        {
                            // Ok - the codec did not accept the change - we keep the old picture
                            // @TODO: video encoder should return the "best supported format"
                             Error("-VideoEncoder: codec did not support size change. Keeping the old one.\n");
                        }
                    }
                }

                pic = input->GrabFrame(frameTime/1000);

		//Check picture
		if (!pic || codec == -1)
			//Exit
			continue;

		//Une image est arrivée : le pont est écarté, il faut vraiment encoder.
		if (!videoEncoder)
		{
			videoEncoder = VideoCodecFactory::CreateEncoder(codec, effective);

			//Comprobamos que se haya creado correctamente
			if (videoEncoder == NULL)
			{
				//error
				Error("Can't create video encoder\n");
				encoding = false;
				break;
			}

			//Send at higher bitrate first frame, but skip frames after that so sending bitrate is kept
			// DIsabled by IVES - to check later
			videoEncoder->SetFrameRate(fps,current,intraPeriod);
			//Iniciamos el tamama�o del encoder
			videoEncoder->SetSize(width,height);

			Log("-Created %s video encoder.\n", VideoCodec::GetNameFor(codec));

			//Cette image-ci est abandonnée : elle a servi à décider qu'un
			//encodeur était nécessaire. Repasser par le haut de la boucle rend
			//au tour suivant l'ordre d'origine — ajustement à la taille native
			//(useInputSize) AVANT l'encodage — au lieu de le dupliquer ici.
			//Une image perdue à l'établissement est sans conséquence : le flux
			//démarre de toute façon sur l'IDR que produit le premier encodage.
			continue;
		}

		//Check if we need to send intra
		if (sendFPU)
		{
			//Log
			Log("-FastPictureUpdate\n");
			//Set it
			videoEncoder->FastPictureUpdate();
			//Do not send anymore
			sendFPU = false;
		}

		//Calculate target bitrate
		int target = current;

		//Check temporal limits for estimations
		if (bitrateAcu.IsInWindow())
		{
			//Get real sent bitrate during last second and convert to kbits
			DWORD instant = bitrateAcu.GetInstantAvg()/1000;
			//If we are in quarentine
			if (videoBitrateLimitCount)
				//Limit sending bitrate
				target = videoBitrateLimit;
			//Check if sending below limits
			else if (instant<bitrate)
				//Increase a 8% each second or fps kbps
				target += (DWORD)(target*0.08/fps)+1;
		}

		//Check target bitrate agains max conf bitrate
		if (target>bitrate*1.2)
			//Set limit to max bitrate allowing a 20% overflow so instant bitrate can get closer to target
			target = bitrate*1.2;

		//Check limits counter
		if (videoBitrateLimitCount>0)
			//One frame less of limit
			videoBitrateLimitCount--;

		//Check if we have a new bitrate
		if (target && target!=current)
		{
			//Reset bitrate
			videoEncoder->SetFrameRate(fps,target,intraPeriod);
			//Upate current
			current = target;
		}

		//Procesamos el frame
		VideoFrame *videoFrame = videoEncoder->EncodeFrame(pic);

		//If was failed
		if (!videoFrame)
			//Next
			continue;

		//Check
		if (frameTime)
		{
			//Calculate slept time
			QWORD sleep = frameTime;
			//Remove extra sleep from prev
			if (overslept<sleep)
				//Remove it
				sleep -= overslept;
			else
				//Do not overflow
				sleep = 1;
			//Dormir jusqu'à l'échéance prev+sleep (µs, réveillé par Stop)
			QWORD elapsed = getDifTime(&prev);
			int canceled = (sleep > elapsed)
				&& pacer.WaitSignal(std::chrono::microseconds(sleep - elapsed));
			//Check if we have been canceled
			if (canceled)
				//Exit
				break;
			//Get differencence
			QWORD diff = getDifTime(&prev);
			//If it is biffer
			if (diff>frameTime)
				//Get what we have slept more
				overslept = diff-frameTime;
			else
				//No oversletp (shoulddn't be possible)
				overslept = 0;
		}
		
		//If first
		if (!frameTime)
		{
			//Set frame time, slower
			frameTime = 1000000/fps;
		} else {
			//Set frame time
			frameTime = 1000000/fps;
		}

		//Add frame size in bits to bitrate calculator
	        bitrateAcu.Update(getDifTime(&first)/1000,videoFrame->GetLength()*8);

		//Update fps count
		fpsAcu.Update(getDifTime(&first)/1000,1);

		//Set frame timestamp
		videoFrame->SetTimestamp(getDifTime(&first)/1000);

		//Set sending time of previous frame
		getUpdDifTime(&prev);
		
		//Calculate sending times based on bitrate
		DWORD sendingTime = videoFrame->GetLength()*8/current;

		//Adjust to maximum time
		if (sendingTime>frameTime/1000)
			//Cap it
			sendingTime = frameTime/1000;

		//Send it smoothly
		SmoothFrame(videoFrame,sendingTime);

		//if ((num%20) == 0)Log("-Send encoded frame codec %d.\n", videoEncoder->type);

		//Dump statistics
		if (num && ((num%fps*10)==0))
		{
			Debug("-Send bitrate current=%d avg=%llf rate=[%llf,%llf] fps=[%llf,%llf] limit=%d\n",
				current,bitrateAcu.GetInstantAvg()/1000,bitrateAcu.GetMinAvg()/1000,bitrateAcu.GetMaxAvg()/1000,
				fpsAcu.GetMinAvg(),fpsAcu.GetMaxAvg(),videoBitrateLimit);
			bitrateAcu.ResetMinMax();
			fpsAcu.ResetMinMax();
		}
		num++;
	}

	Log("-SendVideo out of loop\n");

	//Terminamos de capturar
	input->StopVideoCapture();

	//Check
	if (videoEncoder)
		//Borramos el encoder
		delete videoEncoder;

	//Salimos
	Log("<SendVideo [%d]\n",encoding);
	return 0;
}
