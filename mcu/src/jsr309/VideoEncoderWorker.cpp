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
	//Consigne inconnue tant que SetCodec n'a pas été appelé (GetBitrate = 0)
	bitrate = 0;
        useInputSize = false;
	negotiatedDirty = false;
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
//donc des bornes qui changent sur un encodeur ouvert exigent de le rouvrir.
//
//Ce qui a changé le 2026-08-13, et pourquoi : la réouverture se faisait ici, par
//un Stop()/Start() synchrone. Or cet appel arrive du thread XML-RPC, qui tient le
//verrou de la MediaSession pendant tout `EndpointStartReceiving` — donc le join du
//thread d'encodage avait lieu SOUS ce verrou. Le jour où ce join n'est pas revenu
//(`svt_av1_enc_deinit_handle` de SVT-AV1 0.9.0 se bloque sur un de ses threads
//internes), la session entière a gelé, les threads de dispatch se sont empilés
//derrière le verrou, la file d'acceptation a saturé, et le serveur a cessé
//d'accepter tout en restant « actif » pour systemd.
//
//La réouverture est donc DÉPORTÉE dans la boucle : on mémorise et on lève un
//drapeau. Le chemin dangereux n'est pas déplacé, il disparaît — plus aucun join
//sous le verrou de session par ce chemin. Prix : un GOP émis avec les bornes
//précédentes, ce qui est sans conséquence pour un profil ou un niveau.
void VideoEncoderMultiplexerWorker::SetNegotiatedCodecProperties(const std::map<int,Properties>& byCodec)
{
	bool encoderRunning;

	{
		std::lock_guard<std::mutex> lock(negotiatedLock);

		//Bornes identiques : ne rien faire (chaque push
		//re-signalisation/attach/StartSending repasse ici).
		if (negotiated == byCodec)
			return;

		negotiated = byCodec;
		encoderRunning = encoding;
	}

	Log("-VideoEncoder: negotiated codec properties updated [%d codec(s)]\n",
	    (int)byCodec.size());

	if (encoderRunning)
	{
		//La boucle s'en charge : elle jettera son encodeur et le recréera avec les
		//nouvelles bornes à la prochaine image. Rien à joindre depuis ici.
		negotiatedDirty = true;
		Log("-VideoEncoder: renegotiation deferred to the encoding loop\n");
		return;
	}

	//Encodeur ARRÊTÉ : il n'y a pas de thread d'encodage à joindre, donc le chemin
	//historique est sûr — et c'est lui qui démarre l'encodeur quand la négociation
	//est ce qui le rend possible.
	Stop();
	if (!listeners.empty() && codec != (VideoCodec::Type)-1)
	{
		Log("-VideoEncoder: started encoder with negotiated properties.\n");
		Start();
	}
}

//Bornes effectives et géométrie : la config du contrôleur, écrasée pour CE codec
//par ce que le pair a déclaré savoir décoder. Repart de la CONFIG à chaque appel,
//pour suivre aussi une renégociation qui ASSOUPLIT la borne.
void VideoEncoderMultiplexerWorker::ComputeEffective(Properties& effective)
{
	effective = params;

	{
		std::lock_guard<std::mutex> lock(negotiatedLock);
		std::map<int,Properties>::const_iterator itNeg = negotiated.find((int)codec);
		if (itNeg != negotiated.end())
		{
			for (Properties::const_iterator it = itNeg->second.begin(); it != itNeg->second.end(); ++it)
				effective[it->first] = it->second;

			Log("-VideoEncoder: opening with negotiated properties for %s [%d key(s)]\n",
			    VideoCodec::GetNameFor(codec), (int)itNeg->second.size());
		}
	}

	width  = GetWidth(mode);
	height = GetHeight(mode);
	fps    = configuredFps;

	//Phase 5b : écrêtage cadence/taille au niveau AV1 déclaré par le pair
	//(annexe A.3 — décidé le 2026-08-06 : écrêter, jamais refuser la vidéo).
	if (codec == VideoCodec::AV1)
		AV1Encoder::ClampToLevel(effective, width, height, fps);
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

	//Bornes négociées de la patte émettrice fusionnées par-dessus la config, et
	//géométrie qui en découle — le pair a déclaré ce qu'il sait décoder (profil
	//H.264, packetization-mode, niveau AV1), et émettre au-dessus produit un flux
	//négocié avec succès et décodé par personne. Calculé AVANT la capture :
	//l'écrêtage AV1 s'applique à elle. Recalculable en cours de boucle, cf. le
	//drapeau negotiatedDirty plus bas.
	Properties effective;
	ComputeEffective(effective);

	//Le drapeau ne concerne que les bornes arrivées PENDANT que nous tournons :
	//celles d'avant viennent d'être prises en compte.
	negotiatedDirty = false;

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

		//La négociation a changé pendant que nous tournions
		//(SetNegotiatedCodecProperties). Les Properties ne sont lues qu'à
		//CreateEncoder, donc on jette l'encodeur : la prochaine image le recrée avec
		//les nouvelles bornes. C'est ce qui remplace le Stop/Start synchrone d'avant
		//— celui-ci joignait CE thread depuis le thread XML-RPC qui tient le verrou
		//de la MediaSession, et il n'en est pas revenu le 2026-08-13.
		if (negotiatedDirty.exchange(false))
		{
			ComputeEffective(effective);

			if (videoEncoder)
			{
				delete videoEncoder;
				videoEncoder = NULL;
			}

			//La géométrie peut avoir bougé (écrêtage AV1 sur un niveau plus bas, ou
			//plus haut) : la capture doit suivre, sinon l'encodeur recréé recevrait
			//des images d'une autre taille que celle qu'il annonce.
			if (!input->StartVideoCapture(width,height,fps))
				Error("-VideoEncoder: failed to restart capture after renegotiation [%dx%d@%d]\n",
				      width, height, fps);
			else
				Log("-VideoEncoder: applied renegotiated properties in-loop [%dx%d@%d]\n",
				    width, height, fps);

			//L'encodeur recréé doit émettre une intra : le puits vient de perdre la
			//référence de son flux.
			sendFPU = true;
		}

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
