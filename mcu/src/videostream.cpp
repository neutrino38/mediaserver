#include <sstream>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <signal.h>
#include <math.h>
#include "videostream.h"
#include "h264/h264encoder.h"
#include "log.h"
#include "tools.h"
#include "acumulator.h"
#include "RTPSmoother.h"

std::string GetThreadId(std::thread & thread)
{
	std::ostringstream oss;
	oss << thread.get_id();;
	return oss.str();
}

/**********************************
* VideoStream
*	Constructor
***********************************/
VideoStream::VideoStream(Listener* listener, PictPtr & muteLogo, MediaFrame::MediaRole role) :
	rtp(MediaFrame::Video,listener,role), logo(muteLogo)
{
	//Inicializamos a cero todo
	sendingVideo = TaskIdle;
	receivingVideo = TaskIdle;
	videoInput	= nullptr;
	videoOutput	= nullptr;
	//rtpSession est un weak_ptr : vide par défaut, lié dans RTPParticipant::Init
	
	videoCodec=VideoCodec::H263_1996;
	videoCaptureMode=0;
	videoGrabWidth=0;
	videoGrabHeight=0;
	videoFPS=0;
	videoBitrate=0;
	videoIntraPeriod=0;
	videoBitrateLimit=0;
	senderBweLimit=0;
	sendFPU = false;
	this->listener = listener;
	mediaListener = NULL;
	muted = false;
	mediaRole = role;
	
	recSSRC = 0;
	//mutex/cond : std::mutex / std::condition_variable (RAII, rien à initialiser)
}

/*******************************
* ~VideoStream
*	Destructor. Cierra los dispositivos
********************************/
VideoStream::~VideoStream()
{
	//Défense en profondeur (H-5) : garantit l'arrêt/join de tous les threads
	//même si l'appelant a oublié d'appeler End(). End() est idempotent.
	End();
	//mutex/cond : std::mutex / std::condition_variable (RAII, rien à détruire)
}

/**********************************************
* SetVideoCodec
*	Fija el modo de envio de video 
**********************************************/
int VideoStream::SetVideoCodec(VideoCodec::Type codec,int mode,int fps,int bitrate,int intraPeriod,const Properties& properties)
{
	Log("-SetVideoCodec [%s,%dfps,%dkbps,intra:%d]\n",VideoCodec::GetNameFor(codec),fps,bitrate,intraPeriod);

	//LO guardamos
	videoCodec=codec;

	//Guardamos el bitrate
	videoBitrate=bitrate;
	
	//Store properties
	videoProperties = properties;
	
	if (mediaRole == MediaFrame::VIDEO_SLIDES)
	{
		videoProperties.SetProperty("h264.qpel", "6");
	}
	
	//The intra period
	if (intraPeriod>0)
		videoIntraPeriod = intraPeriod;

	//Get width and height
	videoGrabWidth = GetWidth(mode);
	videoGrabHeight = GetHeight(mode);

	//Check size
	if (!videoGrabWidth || !videoGrabHeight)
		//Error
		return Error("Unknown video mode\n");

	//Almacenamos el modo de captura
	videoCaptureMode=mode;

	//Y los fps
	videoFPS=fps;

	return 1;
}

int VideoStream::SetTemporalBitrateLimit(int estimation)
{




	//RFC 5104 : la limite (TMMBR/REMB, en bps) reste en vigueur jusqu'à ce
	//qu'une nouvelle valeur la remplace — zéro la lève. L'ancienne
	//« quarantaine » d'une seconde laissait le débit remonter à la consigne
	//pleine dès que le pair cessait de répéter son TMMBR ; or il cesse
	//précisément quand on lui répond TMMBN, ce que la session fait désormais.
	videoBitrateLimit = estimation/1000;
	//Exit
	return 1;
}

int VideoStream::SetSenderEstimatedBitrate(int estimation)
{
	//Cible du BWE émetteur local (lot 6.3), deuxième champ à côté de la
	//limite du pair : la boucle d'encodage prend le min des deux.
	senderBweLimit = estimation/1000;
	return 1;
}

void VideoStream::SetRemoteRateEstimator(RemoteRateEstimator* estimator)
{
	//Set it in the rtp session
	rtp.SetRemoteRateEstimator(estimator);
}

/***************************************
* Init
*	Inicializa los devices 
***************************************/
int VideoStream::Init(VideoInput *input,VideoOutput *output)
{
	Log(">Init video stream\n");
	
	//Guardamos los objetos. Cette surcharge brute (appelée avec (NULL,NULL) par
	//RTPParticipant::Init) n'écrase donc pas ce que SetVideoInput/Output
	//possédant vient de poser. Chemin "emprunté" : shared_ptr non possédant.
	if (input != NULL)
		videoInput  = std::shared_ptr<VideoInput>(input, [](VideoInput*){});
	if (output != NULL)
		videoOutput = std::shared_ptr<VideoOutput>(output, [](VideoOutput*){});
	
	//No estamos haciendo nada
	sendingVideo = TaskIdle;
	receivingVideo = TaskIdle;
	
	//Iniciamos el rtp
	if(!rtp.Init())
		return Error("No hemos podido abrir el rtp\n");

	//Init smoother
	smoother.Init(&rtp);

	Log("<Init video stream\n");

	return 1;
}

int VideoStream::SetLocalCryptoSDES(const char* suite, const char* key64)
{
	return rtp.SetLocalCryptoSDES(suite,key64);
}

int VideoStream::SetRemoteCryptoSDES(const char* suite, const char* key64, int keyRank)
{
	
	return rtp.SetRemoteCryptoSDES(suite,key64,keyRank);
}

int VideoStream::SetRemoteCryptoDTLS(const char *setup,const char *hash,const char *fingerprint)
{
	return rtp.SetRemoteCryptoDTLS(setup, hash, fingerprint);
}

int VideoStream::SetLocalSTUNCredentials(const char* username, const char* pwd)
{
	return rtp.SetLocalSTUNCredentials(username,pwd);
}

int VideoStream::SetRemoteSTUNCredentials(const char* username, const char* pwd)
{
	return rtp.SetRemoteSTUNCredentials(username,pwd);
}
int VideoStream::SetRTPProperties(const Properties& properties)
{
	//For each property
	for (Properties::const_iterator it=properties.begin();it!=properties.end();++it)
	{
		if (it->first.compare(0, 6, "codec.")==0)
		{
			std::string key = it->first.substr(6, std::string::npos);
			videoProperties[key] = it->second; 
		}
	}
	return rtp.SetProperties(properties);
}


/***************************************
* StartSending
*	Comienza a mandar a la ip y puertos especificados
***************************************/
int VideoStream::StartSending(char *sendVideoIp,int sendVideoPort,RTPMap& rtpMap)
{
	if (sendVideoIp) 
		Log(">StartSendingVideo [%s,%d]\n",sendVideoIp,sendVideoPort);
	else
		Log(">StartSendingVideo with previous configuration\n");

	//Y esperamos que salga
	StopSending();

    if (sendingVideo != TaskIdle)
		return Error("Cannot start sending video: bad state.\n");

	//Si tenemos video
	if (sendVideoPort==0)
		return Error("No video\n");

	if (sendVideoIp)
	{
		//Iniciamos las sesiones rtp de envio
		if(!rtp.SetRemotePort(sendVideoIp,sendVideoPort))
			return Error("Error abriendo puerto rtp\n");
	}

	//Set sending map
	if (!rtpMap.empty()) rtp.SetSendingRTPMap(rtpMap);
	
	//Set video codec
	if (!rtp.SetSendingCodec(videoCodec))
		//Error
		return Error("%s video codec not supported by peer\n",VideoCodec::GetNameFor(videoCodec));

	//Estamos mandando
	sendingVideo = TaskStarting;

	//Arrancamos los procesos
	sendVideoThread = std::thread(&VideoStream::SendVideo, this);

	//LOgeamos
	Log("<StartSending video in thread [%s]\n",GetThreadId(sendVideoThread).c_str());

	return 1;
}

int VideoStream::StartSending()
{
	RTPMap dummy;

	return StartSending(nullptr, 0, dummy);
}

/***************************************
* StartReceiving
*	Abre los sockets y empieza la recetpcion
****************************************/
int VideoStream::StartReceiving(RTPMap& rtpMap)
{
	//Si estabamos reciviendo tenemos que parar
	StopReceiving();	

	if (receivingVideo != TaskIdle)
		return Error("Failed to start receiving video. Task in bad state.\n");

	//Iniciamos las sesiones rtp de recepcion
	int recVideoPort= rtp.GetLocalPort();

	//Set receving map
	if (!rtpMap.empty()) rtp.SetReceivingRTPMap(rtpMap);

	//P7/S2 : nouveau cycle de reception => on rearme la notification « premier
	//paquet RTP/SRTP recu », pour que « media etabli » soit signale une fois PAR
	//cycle (un StopReceiving/StartReceiving le redeclenche) et pas une seule fois
	//dans la vie du participant.
	rtp.ArmRTPReceivedNotification();

	//Estamos recibiendo
	receivingVideo= TaskStarting;

	//Arrancamos los procesos
	recVideoThread = std::thread(&VideoStream::RecVideo, this);

	//Logeamos
	Log("-StartReceiving Video [%d]\n",recVideoPort);

	return recVideoPort;
}

int VideoStream::StartReceiving()
{
	RTPMap dummy;

	return StartReceiving(dummy);
}


/***************************************
* End
*	Termina la conferencia activa
***************************************/
int VideoStream::End()
{
	int ret;
	Log(">End\n");

	//Close smoother
	smoother.End();

	//Cerramos la session de rtp
	rtp.End();

	//Cerramos la session de rtp observée (peut être celle de MAIN si SLIDES).
	//RTPSession::End() est idempotent ; RTPParticipant::End() (H-3) termine
	//SLIDES avant MAIN, donc pas de fermeture prématurée de la session lue.
	if (auto session = rtpSession.lock())
		session->End();


	ret = StopReceiving();
	ret &= StopSending();

	rtpSession.reset();
	
	
	Log("<End\n");

	return ret;
}



/***************************************
* StopSending
*     Manu paranoid version.
***************************************/
int VideoStream::StopSending()
{
	// save thread ID
	auto threadID = GetThreadId(sendVideoThread);

	Log(">StopSending thread=[%s]\n",threadID.c_str());

	//Esperamos a que se cierren las threads de envio
	if (sendingVideo == TaskRunning || sendingVideo == TaskStarting)
	{
	    for (int i = 0; i < 10; i++)
	    {
		//Paramos el envio
		sendingVideo = TaskStopping;

		//Check we have video
		if (videoInput)
			//Cencel video grab
			videoInput->CancelGrabFrame();

		//Cancel sending (réveille l'attente de pacing dans SendVideo)
		cond.notify_all();
		msleep(100000);
		
		if ( sendingVideo == TaskIdle )
		{
		    //Y esperamos
		    if (sendVideoThread.joinable()) sendVideoThread.join();
		    Log("<StopSending thread=[%s] after %d attempt(s).\n", threadID.c_str(), i);
		    return 1;
		}
	     }
	     //Le thread ne s'est pas arrêté dans les temps : on trace l'anomalie mais on
	     //joint quand même — le laisser vivre provoquerait un use-after-free des pipes
	     //détruits juste après par DestroyParticipant (plan smart pointers, étape 0.8).
	     Error("-StopSending: thread=[%s] still running after 10 attempts, joining anyway\n",
	           threadID.c_str());
	     if (sendVideoThread.joinable()) sendVideoThread.join();
	     Log("<StopSending thread=[%s] joined after forced wait\n", threadID.c_str());
	     return 1;
	}

	//Le thread d'envoi n'était ni Running ni Starting : il est sorti de lui-même
	//(SendVideo rend la main sur une erreur d'initialisation ; il ne sort plus
	//sur un mixeur à sec, cf. sa boucle). L'objet std::thread reste JOINABLE
	//pour autant, et
	//StartSending lui réaffecte un thread neuf : réaffecter un std::thread
	//joinable appelle std::terminate(), donc abort(). C'est ce qui tuait le
	//serveur à la reprise d'un appel mis en attente.
	//
	//La postcondition de StopSending est donc inconditionnelle : au retour,
	//sendVideoThread n'est plus joignable. C'est déjà ce que StartSending
	//suppose, lui qui exige TaskIdle juste après nous avoir appelés.
	if (sendVideoThread.joinable()) sendVideoThread.join();

	Log("<StopSending\n");

	return 1;
}

/***************************************
* StopReceiving
*	Termina la recepcion
***************************************/
int VideoStream::StopReceiving()
{
	auto threadID = GetThreadId(recVideoThread);
	Log(">StopReceiving thread ID=%s\n", threadID.c_str());
	
	//Y esperamos a que se cierren las threads de recepcion
	if (receivingVideo == TaskRunning || receivingVideo == TaskStarting)
	{
	    for (int i = 0; i < 10; i++)
	    {

		//Dejamos de recivir
		receivingVideo = TaskStopping;

		if (auto session = rtpSession.lock())
			//Cancel rtp
			session->CancelGetPacket(recSSRC);
		
		msleep(100000);
		if (receivingVideo == TaskIdle)
		{
		    //Esperamos
		    if (recVideoThread.joinable()) recVideoThread.join();
		    Log("<StopReceiving\n");
		    return 1;
		}
	    }
	    //Le thread ne s'est pas arrêté dans les temps : on trace l'anomalie mais on
	    //joint quand même — le laisser vivre provoquerait un use-after-free des pipes
	    //détruits juste après par DestroyParticipant (plan smart pointers, étape 0.8).
	    Error("-StopReceiving: thread=[%s] still running after 10 attempts, joining anyway\n",
	          threadID.c_str());
	    if (recVideoThread.joinable()) recVideoThread.join();
	    Log("<StopReceiving thread=[%s] joined after forced wait\n", threadID.c_str());
	}

	//Même postcondition inconditionnelle que StopSending, et pour la même raison :
	//StartReceiving réaffecte recVideoThread, et un std::thread joinable réaffecté
	//appelle std::terminate(). Le chemin n'a pas encore tué le serveur — RecVideo
	//reste Running tant que la session RTP vit — mais rien ne le garantit, et
	//chaque renégociation rappelle StartReceiving.
	if (recVideoThread.joinable()) recVideoThread.join();

	Log("<StopReceiving\n");

	return 1;
}

/*******************************************
* SendVideo
*	Capturamos el video y lo mandamos
*******************************************/
int VideoStream::SendVideo()
{
	timeval first;
	timeval prev;
	timeval lastFPU;
	timeval statstimer;
	
	DWORD num = 0;
	QWORD overslept = 0;

	Acumulator bitrateAcu(1000);
	DWORD fpsOut = 0;
	
	Log(">SendVideo [width:%d,size:%d,bitrate:%d,fps:%d,intra:%d]\n",videoGrabWidth,videoGrabHeight,videoBitrate,videoFPS,videoIntraPeriod);
	blocksignals();

	//Creamos el encoder
	VideoEncoder* videoEncoder = VideoCodecFactory::CreateEncoder(videoCodec,videoProperties);

	//Comprobamos que se haya creado correctamente
	if (videoEncoder == NULL)
		//error
		return Error("Can't create video encoder\n");

	//Comrpobamos que tengamos video de entrada
	if (videoInput == NULL)
		return Error("No video input");

	//Iniciamos el tama�o del video
	if (!videoInput->StartVideoCapture(videoGrabWidth,videoGrabHeight,videoFPS))
		return Error("Couldn't set video capture\n");

	//Start at 80%
	int current = videoBitrate*0.8;

	//Send at higher bitrate first frame, but skip frames after that so sending bitrate is kept
	videoEncoder->SetFrameRate(videoFPS,current*5,videoIntraPeriod);

	//No wait for first
	QWORD frameTime = 0;

	//Iniciamos el tamama�o del encoder
 	videoEncoder->SetSize(videoGrabWidth,videoGrabHeight);

	//The time of the first one
	gettimeofday(&first,NULL);
	gettimeofday(&statstimer,NULL);

	//The time of the previos one
	gettimeofday(&prev,NULL);

	//Fist FPU
	gettimeofday(&lastFPU,NULL);
	
	//Started
	Log("-Sending video\n");

	// Mark task as running

	int intputErrCount = 0;
	if ( sendingVideo == TaskStarting) sendingVideo = TaskRunning;
	//Mientras tengamos que capturar
	while (sendingVideo == TaskRunning)
	{
		//Nos quedamos con el puntero antes de que lo cambien
		PictPtr pic = videoInput->GrabFrame(frameTime/1000);

		//Check picture
		if (!pic)
		{
			//Mixeur à sec. C'est un transitoire NORMAL tant que la session RTP
			//vit : reprise d'une mise en attente, décodeur du participant en
			//resynchronisation, changement de résolution en cours de flux. La
			//fin de l'émission est la décision de StopSending, qui pose
			//sendingVideo, annule le grab et réveille la condition — le while
			//ci-dessus est donc la seule sortie de cette boucle.
			//
			//Sortir sur un compteur gelait la vidéo pour tout le reste de
			//l'appel, rien ne relançant SendVideo hors d'une renégociation.
			//Capture du 2026-08-23 : sortie à 10:51:52.956, image clé du
			//participant à 10:51:53.099 — perdu de 143 ms. Et la fenêtre se
			//lisait « 10 × 1 s » alors que msleep prend des MICROsecondes : deux
			//secondes en pratique, contre 1,6 à 2,6 s pour qu'un Linphone
			//produise une image clé après un FPU. La course était imperdable.
			msleep(1000);
			if (intputErrCount++ == 10)
				Log("-videostream: no frame from the mixer, holding the send loop\n");
			continue;
		}

		if (intputErrCount > 10)
			Log("-videostream: mixer feeding again after %d dry grab(s)\n", intputErrCount);

		intputErrCount = 0;
		//Check if we need to send intra
		if (sendFPU)
		{
			//Do not send anymore
			sendFPU = false;
			//Do not send if just sent one (10ms). getDifTime() is in
			//microseconds. The flag is cleared above, so a request landing
			//inside this window is DROPPED, not deferred.
			if (getDifTime(&lastFPU)/100>100)
			{
				//Set it
				videoEncoder->FastPictureUpdate();
				//Update last FPU
				getUpdDifTime(&lastFPU);
			}

		}

		//Calculate target bitrate
		int target = current;

		//Check temporal limits for estimations
		if (bitrateAcu.IsInWindow())
		{
			//Get real sent bitrate during last second and convert to kbits
			DWORD instant = bitrateAcu.GetInstantAvg()/1000;
			//Check if sending below limits
			if (instant<videoBitrate)
				//Increase a 8% each second or fps kbps
				target += (DWORD)(target*0.08/videoFPS)+1;
		}

		//Plafond : la consigne négociée (b=AS de la patte émettrice), sans
		//marge. L'ancien ×1.2 autorisait 20 % au-dessus de ce que le SDP
		//annonce — un pair ou un SBC qui police sa bande passante jette
		//l'excédent. L'encodeur sous-atteint sa cible, donc le débit réel
		//reste sous la consigne : c'est le bon côté de la barrière.
		if (target>videoBitrate)
			target = videoBitrate;

		//Limite TMMBR/REMB en vigueur : STRICTE (pas de marge ×1.2 — c'est le
		//plafond déclaré du pair, pas notre consigne) et PERSISTANTE (levée par
		//une nouvelle valeur, jamais par le temps — cf. SetTemporalBitrateLimit).
		if (videoBitrateLimit>0 && target>videoBitrateLimit)
			target = videoBitrateLimit;

		//Cible du BWE émetteur local (lot 6.3) : min() avec la limite du pair
		if (senderBweLimit>0 && target>senderBweLimit)
			target = senderBweLimit;

		//Check if we have a new bitrate
		if (target && target!=current)
		{
			//Reset bitrate
			videoEncoder->SetFrameRate(videoFPS,target,videoIntraPeriod);
			//Upate current
			current = target;
		}
		
		//Procesamos el frame
		VideoFramePtr videoFrame = videoEncoder->EncodeFrame(pic);

		//If was failed
		if (!videoFrame)
			//Next
			continue;

		//Increase frame counter
		fpsOut++;
		
		//Check
		if (frameTime)
		{
			timespec ts;
			//Calculate slept time
			QWORD sleep = frameTime;
			//Remove extra sleep from prev
			if (overslept<sleep)
				//Remove it
				sleep -= overslept;
			else
				//Do not overflow
				sleep = 1;
			//Calculate timeout (deadline absolue CLOCK_REALTIME = prev + sleep µs)
			calcAbsTimeoutNS(&ts,&prev,sleep);
			//Convertit en time_point system_clock (= CLOCK_REALTIME) pour wait_until
			auto deadline = std::chrono::system_clock::from_time_t(ts.tv_sec)
				+ std::chrono::nanoseconds(ts.tv_nsec);
			//Wait next or stopped : le prédicat rend true si on doit s'arrêter
			//(réveil par notify_all de StopSending) → canceled ; false au timeout.
			std::unique_lock<std::mutex> lock(mutex);
			bool canceled = cond.wait_until(lock, deadline,
				[this]{ return sendingVideo != TaskRunning; });
			lock.unlock();
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
			//Set frame time, slower
			frameTime = 5*1000000/videoFPS;
		else
			//Set frame time
			frameTime = 1000000/videoFPS;

		//Add frame size in bits to bitrate calculator
		bitrateAcu.Update(getDifTime(&first)/1000,videoFrame->GetLength()*8);

		//Set frame timestamp
		videoFrame->SetTimestamp(getDifTime(&first)/1000);

		//Check if we have mediaListener
		if (mediaListener)
			//Call it
			mediaListener->onMediaFrame(*videoFrame);

		//Set sending time of previous frame
		getUpdDifTime(&prev);

		//Calculate sending times based on bitrate.
		//Debit de pacing = 1,1 x la cible (temoin, pacing factor des que
		//l'estimation depend des temps d'arrivee) : lisser tout juste A la
		//cible transforme chaque image en sa propre file d'attente, et le
		//BWE emetteur (lot 6) mesurerait nos rafales au lieu du reseau.
		DWORD sendingTime = videoFrame->GetLength()*8/(current+current/10);

		//PAS de plafonnement a la periode d'image : c'est lui qui tronquait
		//l'etalement d'une trame cle (2,2 x une trame inter, mesure du
		//2026-08-20) et la faisait partir en rafale. Le pacer du lisseur
		//reporte le depassement sur l'image suivante, borne par MaxAheadUs.

		//Send it smoothly
		smoother.SendFrame(videoFrame.get(),sendingTime);

		//Restore bitrate after the first frame, once it is on its way: a
		//SetFrameRate may reopen the codec, better not while an image waits.
		if (!num)
			videoEncoder->SetFrameRate(videoFPS,current,videoIntraPeriod);

		//Dump statistics
		DWORD statstime2 = (DWORD) (getDifTime(&statstimer) / 1000);
		if ( statstime2 >= 20000)
		{
			Log("-Send video stats for participant codec = %s.\n", VideoCodec::GetNameFor(videoCodec));
			Log("                  current bitrate=%d kbit/s  avg=%8.2f kbit/s  limit=%d kbit/s\n",
                            current,(double)(bitrateAcu.GetInstantAvg()/1000),videoBitrateLimit);
			Log("                  fps=[%d]\n",
                            (fpsOut*1000)/statstime2);
			bitrateAcu.ResetMinMax();
			getUpdDifTime(&statstimer);
			fpsOut = 0;
		}
		num++;
	}

	Log("-SendVideo out of loop\n");
	sendingVideo = TaskIdle;
	//Terminamos de capturar
	videoInput->StopVideoCapture();

	//Check
	if (videoEncoder)
		//Borramos el encoder
		delete videoEncoder;

	//Salimos
	Log("<SendVideo [%d]\n",sendingVideo.load());

	return 0;
}

/****************************************
* RecVideo
*	Obtiene los packetes y los muestra
*****************************************/
int VideoStream::RecVideo()
{
	VideoDecoder*	videoDecoder = NULL;
	VideoCodec::Type type;
	timeval 	before;
	timeval		lastFPURequest;
	DWORD		lostCount=0, width=0, height=0;
	DWORD		frameSeqNum = RTPPacket::MaxExtSeqNum;
	DWORD		lastSeq = RTPPacket::MaxExtSeqNum;
	bool		waitIntra = false;
	Log(">RecVideo\n");
	blocksignals();

	if ( receivingVideo == TaskStarting ) receivingVideo = TaskRunning;

	//Session observée (liée par RTPParticipant::Init ; SLIDES peut observer la
	//session de MAIN). keepAlive maintient le participant observé vivant pendant
	//toute la durée de ce thread (cas SLIDES). Repli : sa propre session `rtp`
	//(cas MAIN, ou weak_ptr non lié) — toujours valide tant que ce thread tourne.
	std::shared_ptr<RTPSession> keepAlive = rtpSession.lock();
	RTPSession* session = keepAlive ? keepAlive.get() : &rtp;

	//Inicializamos el tiempo
	gettimeofday(&before,NULL);

	//Not sent FPU yet
	setZeroTime(&lastFPURequest);

	//Mientras tengamos que capturar
	session->ResetPacket(recSSRC, false);
	

	while (receivingVideo == TaskRunning)
	{

		//Obtenemos el paquete
		RTPPacket* packet = session->GetPacket(recSSRC,RTPSession::ConsumerPollMs);

		//Check
		if (!packet)
                {
			//GetPacket a deja attendu : relire le drapeau et repartir.
		    continue;
                }

		//Get extended sequence number
		DWORD seq = packet->GetExtSeqNum();

		//Get packet data
		BYTE* buffer = packet->GetMediaData();
		DWORD size = packet->GetMediaLength();

		//Get type
		type = (VideoCodec::Type)packet->GetCodec();

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
			//Check if we got listener and more than ten seconds have elapsed from last request
			if (listener && getDifTime(&lastFPURequest)>10000000)
			{
				//Debug
				Log("-Requesting FPU lost %d ssrc= %08x\n",lostCount,recSSRC);
				//Reset count
				lostCount = 0;
				//Request it
				listener->onRequestFPU();
				//Request also over rtp
				session->RequestFPU(recSSRC);
				//Update time
				getUpdDifTime(&lastFPURequest);
				//Waiting for refresh
				waitIntra = true;
			}
		}

		//Check if it is a redundant packet
		if (type==VideoCodec::RED)
		{
			//Get redundant packet
			RTPRedundantPacket* red = (RTPRedundantPacket*)packet;
			//Get primary codec
			type = (VideoCodec::Type)red->GetPrimaryCodec();
			//Check it is not ULPFEC redundant packet
			if (type==VideoCodec::ULPFEC)
			{
				//Delete packet
				delete(packet);
				//Skip
				continue;
			}
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
				Error("Error creando nuevo decodificador de video [%d]\n",type);
				//Delete packet
				delete(packet);
				//Next
				continue;
			}
		}

		//Check if we have lost the last packet from the previous frame
		if (seq>frameSeqNum)
		{
			//Try to decode what is in the buffer
			videoDecoder->DecodePacket(NULL,0,1,1);
			//Get picture
			PictPtr frame = videoDecoder->GetFrame();
			width = videoDecoder->GetWidth();
			height = videoDecoder->GetHeight();
			//Check values
			if (frame && width && height)
			{
				//Set frame size
				if (videoOutput != NULL)
				{
					videoOutput->SetVideoSize(width,height);
					if (!muted) videoOutput->NextFrame(frame);
				}
			}
		}

		
		//Lo decodificamos
		if(!videoDecoder->DecodePacket(buffer,size,lost,packet->GetMark()))
		{
			//Check if we got listener and more than one second has elapsed from last request
			if (listener && getDifTime(&lastFPURequest)>1000000)
			{
				//Debug
				Log("-Requesting FPU decoder error\n");
				//Reset count
				lostCount = 0;
				//Request it
				listener->onRequestFPU();
				//Request also over rtp
				session->RequestFPU(recSSRC);
				//Update time
				getUpdDifTime(&lastFPURequest);
				//Waiting for refresh
				waitIntra = true;
			}
			//Delete packet
			delete(packet);
			//Next frame
			continue;
		}

						
		//Si es el ultimo
		if(packet->GetMark())
		{
			if (videoDecoder->IsKeyFrame())
				Log("-Got Intra\n");

			//Acquitter la trame de référence décodée (RPSI) : sans lui, un
			//émetteur msvp8 force une trame clé toutes les 3 s
			WORD refPictureId;
			if (videoDecoder->GetReferencePictureId(refPictureId))
				session->SendReferencePictureSelectionIndication(recSSRC,refPictureId);

			//No seq number for frame
			frameSeqNum = RTPPacket::MaxExtSeqNum;

			//Get picture
			PictPtr frame = videoDecoder->GetFrame();
			//DWORD width = videoDecoder->GetWidth();
			//If it is muted
			if (muted)
			{
				frame = logo;
				//Check size
				if (frame && (logo->GetWidth()!=(DWORD)width || logo->GetHeight()!=(DWORD)height))
				{
					//Get dimension
					width = logo->GetWidth();
					height = logo->GetHeight();
				if (videoOutput != NULL)
					//Set them in the encoder
					videoOutput->SetVideoSize(width,height);
				}
				waitIntra = false;
			}
			else
			{
				frame = videoDecoder->GetFrame();
				//Check size
				if (frame /*&& (videoDecoder->GetWidth()!=width || videoDecoder->GetHeight()!=height)*/)
				{
					//Get dimension
					width = videoDecoder->GetWidth();
					height = videoDecoder->GetHeight();
				if (videoOutput != NULL)
					//Set them in the encoder
					videoOutput->SetVideoSize(width,height);
				}
				
				//Check if we got the waiting refresh
				if (waitIntra && videoDecoder->IsKeyFrame())
					//Do not wait anymore
					waitIntra = false;

			} 
			if (frame )
			{
			
				//Send
				if (videoOutput != NULL ) videoOutput->NextFrame(frame);
			}
		
		}
		//Delete packet
		delete(packet);
	}

	//Borramos el encoder
	delete videoDecoder;
	receivingVideo = TaskIdle;

	Log("<RecVideo\n");
	return 0;
}

int VideoStream::SetMediaListener(MediaFrame::Listener *listener)
{
	//Set it
	this->mediaListener = listener;
	return 0;
}

int VideoStream::SendFPU()
{
	//Next shall be an intra
	sendFPU = true;
	
	return 1;
}

MediaStatistics VideoStream::GetStatistics()
{
	MediaStatistics stats;

	if (auto session = rtpSession.lock())
	{
            session->GetStatistics(recSSRC, stats);
	}
	//Fill stats
	stats.isReceiving	= IsReceiving();
	stats.isSending		= IsSending();

	//Return it
	return stats;
}

int VideoStream::SetMute(bool isMuted)
{
	//Set it
	muted = isMuted;
	if (muted)
	{
		//Push the avatar logo
		PictPtr frame	= logo;
		//Check size
		if (frame && videoOutput != NULL)
		{
			//Set them in the encoder
			videoOutput->SetVideoSize(frame->GetWidth(),frame->GetHeight());
			videoOutput->NextFrame(frame);
		}
	}
	if (videoOutput != NULL)
		videoOutput->KeepAspectRatio(!isMuted);
	//Exit
	return 1;
}
