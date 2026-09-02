/* 
 * File:   VideoEncoderWorker.cpp
 * Author: Sergio
 * 
 * Created on 2 de noviembre de 2011, 23:37
 */

#include <cstdlib>
#include "VideoEncoderWorker.h"
#include "log.h"
#include "tools.h"
#include "RTPMultiplexer.h"
//AV1Encoder::ClampToLevel (écrêtage cadence/taille, phase 5b nego_fmtp)
#include "av1/av1codec.h"

VideoEncoderMultiplexerWorker::VideoEncoderMultiplexerWorker() :
	RTPMultiplexerSmoother(),
	loop(this),
	bitrateAcu(1000),
	fpsAcu(1000)
{
	input = NULL;
	pushed = false;
	encoding = false;
	sendFPU = false;
	lastForcedIntraUs = 0;
	ignoredFPU = 0;
	codec = (VideoCodec::Type)-1;
	mode = 0;
	width = 0;
	height = 0;
	fps = 0;
	intraEffective = 0;
	configuredFps = 0;
	ceilingFps = 0;
	measuredFps = 0;
	//Consigne inconnue tant que SetCodec n'a pas été appelé (GetBitrate = 0)
	bitrate = 0;
	intraPeriod = 0;
	//Aucune limite TMMBR/REMB en vigueur
	videoBitrateLimit = 0;
	senderBweLimit = 0;
	useInputSize = false;
	configDirty = false;
	videoEncoder = NULL;
	current = 0;
	num = 0;
	setZeroTime(&encodeStart);
	pushedWidth = 0;
	pushedHeight = 0;
}

VideoEncoderMultiplexerWorker::~VideoEncoderMultiplexerWorker()
{
	End();
}

int VideoEncoderMultiplexerWorker::Init(VideoInput *input)
{
	//Store it
	this->input = input;
	pushed = false;
	return 0;
}

int VideoEncoderMultiplexerWorker::Init()
{
	//Transcodeur : plus de VideoPipe entre le décodeur et nous. Les images
	//arrivent par EncodePicture, sur le thread de la source (lot 4).
	this->input = NULL;
	pushed = true;
	return 0;
}

int VideoEncoderMultiplexerWorker::SetCodec(VideoCodec::Type codec,int mode,int fps,int bitrate,int intraPeriod, Properties & properties)
{
	Log("-SetVideoCodec [%s=%d, fps:%d, bitrate:%d kbps, intra:%d]\n",VideoCodec::GetNameFor(codec),codec,fps,bitrate,intraPeriod);
	if (fps <= 0)
		return Error("Invalid value for fps:%d.\n", fps);

	//Check size — refusé ICI, tant que le paramétrage n'est pas posé : le chemin
	//des paquets ne peut plus le signaler, il n'a plus d'appelant à qui répondre.
	if (!GetWidth(mode) || !GetHeight(mode))
		//Error
		return Error("Unknown video mode\n");

	{
		std::lock_guard<std::mutex> lock(configLock);

		//Store parameters
		this->codec	  = codec;
		this->mode	  = mode;
		this->bitrate	  = bitrate;
		this->configuredFps = fps;
		this->intraPeriod = intraPeriod;
		//La limite TMMBR/REMB en vigueur (videoBitrateLimit) survit à la
		//renégociation : elle appartient au pair, pas au codec (RFC 5104).
		params = properties;
	}

	//Le chemin des paquets applique au tour suivant : il jette son encodeur et
	//le recrée. Le Stop()/Start() d'avant joignait le thread d'encodage depuis
	//le thread XML-RPC, qui tient le verrou de la MediaSession — c'est ce
	//chemin-là qui a gelé le serveur le 2026-08-13 (§4.4).
	configDirty = true;

	//Check if we are already encoding
	if (!listeners.empty() && !encoding)
	{
		//And start
		Log("-VideoEncoder: started encoder.\n");
		Start();
	}

	//Exit
	return 1;
}

//Phase 5 (nego_fmtp §6.3) : les bornes que la négociation SDP de la patte
//émettrice impose à l'encodeur. Les Properties ne sont lues qu'à CreateEncoder,
//donc des bornes qui changent sur un encodeur ouvert exigent de le rouvrir —
//par le même drapeau que SetCodec (cf. l'en-tête pour le gel du 2026-08-13).
void VideoEncoderMultiplexerWorker::SetNegotiatedCodecProperties(const std::map<int,Properties>& byCodec)
{
	{
		std::lock_guard<std::mutex> lock(configLock);

		//Bornes identiques : ne rien faire (chaque push
		//re-signalisation/attach/StartSending repasse ici).
		if (negotiated == byCodec)
			return;

		negotiated = byCodec;
	}

	Log("-VideoEncoder: negotiated codec properties updated [%d codec(s)]\n",
	    (int)byCodec.size());

	configDirty = true;

	if (!listeners.empty() && !encoding && codec != (VideoCodec::Type)-1)
	{
		Log("-VideoEncoder: started encoder with negotiated properties.\n");
		Start();
	}
}

//Bornes effectives et géométrie : la config du contrôleur, écrasée pour CE codec
//par ce que le pair a déclaré savoir décoder. Repart de la CONFIG à chaque appel,
//pour suivre aussi une renégociation qui ASSOUPLIT la borne.
void VideoEncoderMultiplexerWorker::ComputeEffective()
{
	{
		std::lock_guard<std::mutex> lock(configLock);

		effective = params;

		std::map<int,Properties>::const_iterator itNeg = negotiated.find((int)codec);
		if (itNeg != negotiated.end())
		{
			for (Properties::const_iterator it = itNeg->second.begin(); it != itNeg->second.end(); ++it)
				effective[it->first] = it->second;

			Log("-VideoEncoder: opening with negotiated properties for %s [%d key(s)]\n",
			    VideoCodec::GetNameFor(codec), (int)itNeg->second.size());
		}

		width  = GetWidth(mode);
		height = GetHeight(mode);

		//useInputSize : la géométrie de la SOURCE prime sur le `mode` du
		//contrôleur. La poser AVANT la création de l'encodeur, et pas seulement
		//par ApplyNativeSize après coup : sinon chaque réouverture crée l'encodeur
		//au `mode` pour le redimensionner à l'image suivante, et la trace
		//« Created ... video encoder [WxH] » annonce une taille qui n'est pas
		//celle qui sort.
		if (useInputSize && !input)
		{
			const DWORD nativeWidth  = pushedWidth.load();
			const DWORD nativeHeight = pushedHeight.load();
			if (nativeWidth && nativeHeight)
			{
				width  = (int)nativeWidth;
				height = (int)nativeHeight;
			}
		}

		ceilingFps = configuredFps;
	}

	//Phase 5b : écrêtage cadence/taille au niveau AV1 déclaré par le pair
	//(annexe A.3 — décidé le 2026-08-06 : écrêter, jamais refuser la vidéo).
	if (codec == VideoCodec::AV1)
		AV1Encoder::ClampToLevel(effective, width, height, ceilingFps);

	//La cadence effective repart du plafond, puis suit la mesure s'il y en a une.
	fps = 0;
	intraEffective = 0;
	RecomputeFrameRate();
}

//§3.6 : fpsEffectif = min(consigne, cadence mesurée), plancher à 1 ; période
//intra CONSTANTE EN SECONDES. `intraPeriod` est un nombre d'IMAGES dans l'API
//XML-RPC : la garder telle quelle quand la cadence est divisée par deux
//doublerait l'intervalle entre trames clés en secondes — et c'est la reprise
//après perte qui en souffre, là où les FIR/PLI passent par le transcodeur.
bool VideoEncoderMultiplexerWorker::RecomputeFrameRate()
{
	int wanted = ceilingFps;

	const int measured = measuredFps.load();
	//La mesure ne peut que BAISSER la consigne : celle-ci est une borne du
	//contrôleur (et du niveau AV1).
	if (measured > 0 && measured < wanted)
		wanted = measured;
	if (wanted < 1)
		wanted = 1;

	//intraPeriod <= 0 : le contrôleur n'en impose pas, l'encodeur garde son
	//défaut. Rien à mettre à l'échelle.
	int intra = intraPeriod;
	if (intraPeriod > 0 && ceilingFps > 0)
	{
		intra = intraPeriod * wanted / ceilingFps;
		if (intra < 1)
			intra = 1;
	}

	if (wanted == fps && intra == intraEffective)
		return false;

	Log("-VideoEncoder: cadence effective %d -> %d im/s, periode intra %d -> %d images [consigne %d im/s]\n",
	    fps, wanted, intraEffective, intra, ceilingFps);

	fps = wanted;
	intraEffective = intra;
	return true;
}

int VideoEncoderMultiplexerWorker::GetEffectiveFps()
{
	std::lock_guard<std::mutex> lock(encodeLock);
	return fps;
}

int VideoEncoderMultiplexerWorker::GetEffectiveWidth()
{
	std::lock_guard<std::mutex> lock(encodeLock);
	return width;
}

int VideoEncoderMultiplexerWorker::GetEffectiveHeight()
{
	std::lock_guard<std::mutex> lock(encodeLock);
	return height;
}

int VideoEncoderMultiplexerWorker::GetEffectiveIntraPeriod()
{
	std::lock_guard<std::mutex> lock(encodeLock);
	return intraEffective;
}

void VideoEncoderMultiplexerWorker::SetMeasuredFrameRate(int measured)
{
	if (measured < 0)
		measured = 0;
	measuredFps = measured;
}

void VideoEncoderMultiplexerWorker::SetNativeSize(DWORD w,DWORD h)
{
	if (!w || !h)
		return;
	if (w == pushedWidth.load() && h == pushedHeight.load())
		return;

	pushedWidth = w;
	pushedHeight = h;

	//Journalisé même quand useInputSize est éteint : c'est la seule trace qui dit
	//quelle FORME la source envoie. Sans elle, une image écrasée par une géométrie
	//de sortie imposée ne se diagnostique pas dans le log (appel du 2026-08-28).
	Log("-VideoEncoder: taille native de la source %u x %u [useInputSize %d]\n",
	    w, h, useInputSize ? 1 : 0);
}

int VideoEncoderMultiplexerWorker::Start()
{
	//Check
	if (!pushed && !input)
		//Exit
		return Error("null video input");

	//Postcondition de Stop() : aucune poignée en vol, aucun encodeur ouvert.
	//Appelé sans condition — Encode() peut être sorti de lui-même sans que
	//`encoding` le dise.
	Stop();

	{
		std::lock_guard<std::mutex> lock(encodeLock);

		ComputeEffective();
		//Le drapeau ne concerne que ce qui arrive PENDANT que nous tournons :
		//ce qui précède vient d'être pris en compte.
		configDirty = false;

		//Start at the negotiated bitrate
		current = bitrate;
		num = 0;
		gettimeofday(&encodeStart,NULL);
		bitrateAcu.Reset(0);
		fpsAcu.Reset(0);
	}

	//Comprobamos que tengamos video de entrada
	if (!pushed && !input->StartVideoCapture(width,height,fps))
		return Error("Couldn't set video capture\n");

	//Start smoother (tire un SSRC neuf pour ce run)
	RTPMultiplexerSmoother::Start();

	//Start encoding
	encoding = true;

	//Mode poussé : rien à lancer, c'est la source qui appelle EncodePicture.
	if (!pushed)
		loop.StartThread();

	return 1;
}

int VideoEncoderMultiplexerWorker::Stop()
{
	Log(">Stop VideoEncoderMultiplexerWorker\n");

	//L'arrêt est demandé d'abord et sans condition : même si Encode() est déjà
	//sorti, le drapeau doit retomber avant le join.
	encoding = false;

	if (!pushed)
	{
		//Cancel any frame grabbing, puis joindre. Le pacer est réveillé par
		//StopThread (il annule le Wait du Worker).
		if (input)
			input->CancelGrabFrame();

		loop.StopThread();

		//Terminamos de capturar
		if (input)
			input->StopVideoCapture();
	}

	{
		//Le thread est joint (mode tiré) ou la source est déjà retirée (mode
		//poussé, cf. §4.3 : RemoveListener est la barrière). Ce verrou couvre le
		//cas où le plan de contrôle arrive alors qu'une image est en vol.
		//
		//Le deinit de l'encodeur a désormais lieu sur ce thread-ci : s'il se
		//bloque (cas SVT-AV1 0.9.0 du 2026-08-13, contourné dans libmedikit par
		//medkit/ffcodeclock.h), c'est ce verrou qui retiendra le suivant.
		std::lock_guard<std::mutex> lock(encodeLock);

		delete videoEncoder;
		videoEncoder = NULL;
	}

	//Stop smoother
	RTPMultiplexerSmoother::Stop();

	Log("<Stop VideoEncoderMultiplexerWorker\n");

	return 1;
}

int VideoEncoderMultiplexerWorker::End()
{
	Stop();

	//Set null
	input = NULL;
	return 0;
}

void VideoEncoderMultiplexerWorker::AddListener(Listener *listener)
{
	//Check if we were already encoding
	if (listener && !encoding && codec != (VideoCodec::Type)-1)
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
	//Send FPU
	sendFPU = true;
}

void VideoEncoderMultiplexerWorker::SetREMB(int estimation)
{
	//RFC 5104 : la limite (TMMBR/REMB, en bps) reste en vigueur jusqu'à ce
	//qu'une nouvelle valeur la remplace — zéro la lève. L'ancienne
	//« quarantaine » d'une seconde laissait le débit remonter à la consigne
	//pleine dès que le pair cessait de répéter son TMMBR ; or il cesse
	//précisément quand on lui répond TMMBN, ce que la session fait désormais.
	videoBitrateLimit = estimation/1000;
}

void VideoEncoderMultiplexerWorker::SetSenderEstimate(DWORD estimation)
{
	//Cible du BWE émetteur local (lot 6.3), deuxième champ à côté de la
	//limite du pair : le chemin des paquets prend le min des deux.
	senderBweLimit = estimation/1000;
}

bool VideoEncoderMultiplexerWorker::EnsureEncoder()
{
	//La configuration a changé pendant que nous tournions (SetCodec ou bornes
	//négociées). Les Properties ne sont lues qu'à CreateEncoder, donc on jette
	//l'encodeur : cette image-ci le recrée avec les nouvelles bornes. C'est ce
	//qui remplace le Stop/Start synchrone d'avant — celui-ci joignait le thread
	//d'encodage depuis le thread XML-RPC, et il n'en est pas revenu le
	//2026-08-13.
	if (configDirty.exchange(false))
	{
		ComputeEffective();

		if (videoEncoder)
		{
			delete videoEncoder;
			videoEncoder = NULL;
		}

		//La géométrie peut avoir bougé (écrêtage AV1 sur un niveau plus bas, ou
		//plus haut) : la capture doit suivre, sinon l'encodeur recréé recevrait
		//des images d'une autre taille que celle qu'il annonce.
		if (input && !input->StartVideoCapture(width,height,fps))
			Error("-VideoEncoder: failed to restart capture after renegotiation [%dx%d@%d]\n",
			      width, height, fps);
		else
			Log("-VideoEncoder: applied renegotiated properties [%dx%d@%d]\n",
			    width, height, fps);

		//Nouveau run d'encodage : base de temps neuve, donc SSRC neuf (RFC 3550).
		//C'est le Stop/Start du lisseur qui le faisait auparavant.
		RenewSSRC();

		//L'encodeur recréé doit émettre une intra : le puits vient de perdre la
		//référence de son flux.
		sendFPU = true;
	}

	if (codec == (VideoCodec::Type)-1)
		return false;

	if (videoEncoder)
		return true;

	//Cadence effective connue avant l'ouverture : `time_base`, `rc_buffer_size`
	//et `gop_size` en dépendent tous (§3.6).
	RecomputeFrameRate();

	videoEncoder = VideoCodecFactory::CreateEncoder(codec, effective);
	if (videoEncoder == NULL)
	{
		Error("Can't create video encoder\n");
		//Refermer le chemin plutôt que de rejouer l'échec à chaque image.
		encoding = false;
		return false;
	}

	//Send at higher bitrate first frame, but skip frames after that so sending bitrate is kept
	videoEncoder->SetFrameRate(fps,current,intraEffective);
	//Iniciamos el tamamaño del encoder
	videoEncoder->SetSize(width,height);

	Log("-Created %s video encoder [%dx%d@%d, %d kbps, intra %d].\n",
	    VideoCodec::GetNameFor(codec), width, height, fps, current, intraEffective);

	return true;
}

//Taille NATIVE du producteur (useInputSize) : le mixeur la porte sur son
//VideoInput, le transcodeur la relaie depuis son décodeur (SetNativeSize).
void VideoEncoderMultiplexerWorker::ApplyNativeSize()
{
	if (!videoEncoder || !useInputSize)
		return;

	DWORD nativeWidth = 0;
	DWORD nativeHeight = 0;

	if (input)
	{
		if (!input->HasNativeSizeChanged())
			return;
		nativeWidth  = input->GetNativeWidth();
		nativeHeight = input->GetNativeHeight();
	}
	else
	{
		//Pas de drapeau « la taille a changé » : la comparaison à la géométrie
		//COURANTE ci-dessous suffit, et elle est la seule correcte après une
		//réouverture. ComputeEffective() y remet `width`/`height` au `mode`
		//configuré ; un drapeau déjà consommé laissait alors l'encodeur à cette
		//géométrie-là jusqu'au prochain changement de résolution de la source.
		nativeWidth  = pushedWidth.load();
		nativeHeight = pushedHeight.load();
	}

	if (!nativeWidth || !nativeHeight)
		return;
	if ((int)nativeWidth == width && (int)nativeHeight == height)
		return;

	// If native size has changed, try to reconfigure the codec first
	if (!videoEncoder->SetSize(nativeWidth,nativeHeight))
	{
		// Ok - the codec did not accept the change - we keep the old picture
		// @TODO: video encoder should return the "best supported format"
		Error("-VideoEncoder: codec did not support size change. Keeping the old one.\n");
		return;
	}

	// Ok - the codec accepted the change. Now change the capture ..
	if (input && !input->StartVideoCapture(nativeWidth,nativeHeight,fps))
	{
		// We could not start the capture - revert to previous codec settings
		Error("-VideoEncoder: failed to restart capture with new size %d x %d .\n",
		      nativeWidth, nativeHeight);
		videoEncoder->SetSize(width,height);
		return;
	}

	width = nativeWidth;
	height = nativeHeight;
	Log("-VideoEncoder: adjusted to new native size %u x %u.\n", width, height);
}

/*******************************************
* EncodePicture
*	Encode UNE image et l'étale dans le lisseur. Corps commun au port de mixeur
*	(appelé par la boucle cadencée) et au transcodeur (appelé par le décodeur,
*	sur le thread de la source).
*******************************************/
int VideoEncoderMultiplexerWorker::EncodePicture(PictPtr pic)
{
	if (!pic)
		return 0;

	std::lock_guard<std::mutex> lock(encodeLock);

	if (!encoding)
		return 0;

	if (!EnsureEncoder())
		return 0;

	ApplyNativeSize();

	//La cadence mesurée a pu bouger depuis l'image précédente : une seule trame
	//clé porte le changement de `fps` ET de `gop_size` (§3.6).
	if (RecomputeFrameRate())
		videoEncoder->SetFrameRate(fps,current,intraEffective);

	//Check if we need to send intra
	if (sendFPU.exchange(false))
	{
		const QWORD now = getTime();
		if (lastForcedIntraUs && now - lastForcedIntraUs < MinForcedIntraUs)
		{
			//Au plus une intra forcée par seconde (cf. lastForcedIntraUs).
			ignoredFPU++;
		}
		else
		{
			Log("-FastPictureUpdate%s\n", ignoredFPU ? " (demandes ignorees depuis la precedente)" : "");
			if (ignoredFPU)
				Log("-VideoEncoder: %u demandes d'intra ignorees en moins d'une seconde\n", ignoredFPU);
			ignoredFPU = 0;
			lastForcedIntraUs = now;
			videoEncoder->FastPictureUpdate();
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
		if (instant<(DWORD)bitrate)
			//Increase a 8% each second or fps kbps
			target += (DWORD)(target*0.08/fps)+1;
	}

	//Plafond : la consigne négociée (b=AS de la patte émettrice), sans
	//marge. L'ancien ×1.2 autorisait 20 % au-dessus de ce que le SDP
	//annonce — un pair ou un SBC qui police sa bande passante jette
	//l'excédent. L'encodeur sous-atteint sa cible, donc le débit réel
	//reste sous la consigne : c'est le bon côté de la barrière.
	if (target>bitrate)
		target = bitrate;

	//Limite TMMBR/REMB en vigueur : STRICTE (pas de marge ×1.2 — c'est le
	//plafond déclaré du pair, pas notre consigne) et PERSISTANTE (levée par
	//une nouvelle valeur, jamais par le temps — cf. SetREMB).
	const int peerLimit = videoBitrateLimit.load();
	if (peerLimit>0 && target>peerLimit)
		target = peerLimit;

	//Cible du BWE émetteur local (lot 6.3) : min() avec la limite du pair
	const int bweLimit = senderBweLimit.load();
	if (bweLimit>0 && target>bweLimit)
		target = bweLimit;

	//Sonde : dépasser la limite du pair, bornée et réversible, pour qu'il
	//remesure (cf. BitrateProbe.h). Ne joue que si c'est lui qui borne.
	target = probe.Apply(target, peerLimit, bweLimit, bitrate, fps, getTime());
	//Débit réellement émis sur la dernière seconde : c'est lui que le pair
	//mesure, pas la consigne.
	const unsigned emitted = bitrateAcu.IsInWindow() ? (unsigned)(bitrateAcu.GetInstantAvg()/1000) : 0;
	switch (probe.LastEvent())
	{
		case BitrateProbe::Start:
			Log("-VideoEncoder: sonde au-dessus de la limite du pair %d -> %d kb/s (estimateur %d, consigne %d, emis %u)\n",
			    peerLimit, probe.GetProbeKbps(), bweLimit, bitrate, emitted);
			break;
		case BitrateProbe::End:
			Log("-VideoEncoder: sonde terminee, le pair n'a pas suivi (limite %d kb/s, emis %u kb/s), prochaine dans %u s\n",
			    peerLimit, emitted, (unsigned)(probe.GetIntervalUs()/1000000));
			break;
		case BitrateProbe::Abort:
			Log("-VideoEncoder: sonde interrompue (pair %d, estimateur %d, emis %u kb/s), prochaine dans %u s\n",
			    peerLimit, bweLimit, emitted, (unsigned)(probe.GetIntervalUs()/1000000));
			break;
		case BitrateProbe::Followed:
			Log("-VideoEncoder: sonde : le pair a suivi, limite %d -> %d kb/s, prochaine dans %u s\n",
			    probe.GetPeerAtStart(), peerLimit, (unsigned)(probe.GetIntervalUs()/1000000));
			break;
		default:
			break;
	}
	//Pendant la sonde l'encodeur doit émettre la cible, pas la qualité qui lui
	//suffit : c'est le débit émis que le pair mesure. Rappelé à chaque image,
	//l'encodeur pouvant avoir été recréé entre-temps.
	videoEncoder->SetFillBudget(probe.IsProbing());

	//Check if we have a new bitrate
	if (target && target!=current)
	{
		//Reset bitrate
		videoEncoder->SetFrameRate(fps,target,intraEffective);
		//Upate current
		current = target;
	}

	//Mise à l'échelle vers la géométrie de sortie. C'est VideoPipe::NextFrame qui
	//la faisait au dépôt ; le mixeur passe toujours par lui, l'image est déjà à
	//la bonne taille et le rescaler rend alors un partage zéro-copie.
	PictPtr scaled = resizer.Rescale(pic, width, height, false);
	if (!scaled)
		return 0;

	//Procesamos el frame
	VideoFramePtr videoFrame = videoEncoder->EncodeFrame(scaled);

	//If was failed
	if (!videoFrame)
		//Next
		return 0;

	//Add frame size in bits to bitrate calculator
	bitrateAcu.Update(getDifTime(&encodeStart)/1000,videoFrame->GetLength()*8);

	//Update fps count
	fpsAcu.Update(getDifTime(&encodeStart)/1000,1);

	//Set frame timestamp
	videoFrame->SetTimestamp(getDifTime(&encodeStart)/1000);

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
	SmoothFrame(videoFrame.get(),sendingTime);

	//Dump statistics
	if (num && ((num%fps*10)==0))
	{
		Debug("-Send bitrate current=%d avg=%.0f rate=[%.0f,%.0f] fps=[%.0f,%.0f] limit=%d\n",
			current,(double)(bitrateAcu.GetInstantAvg()/1000),(double)(bitrateAcu.GetMinAvg()/1000),(double)(bitrateAcu.GetMaxAvg()/1000),
			(double)fpsAcu.GetMinAvg(),(double)fpsAcu.GetMaxAvg(),peerLimit);
		bitrateAcu.ResetMinMax();
		fpsAcu.ResetMinMax();
	}
	num++;

	return 1;
}

/*******************************************
* Encode
*	Boucle CADENCÉE du port de mixeur : le mixeur produit à son rythme, c'est
*	elle qui échantillonne à `fps` — et qui relivre la dernière image quand rien
*	de neuf n'arrive avant l'échéance (sémantique de VideoPipe::GrabFrame). Le
*	transcodeur, lui, n'a plus de thread ici : sa cadence est celle de sa source.
*******************************************/
int VideoEncoderMultiplexerWorker::Encode()
{
	timeval prev;
	QWORD overslept = 0;
	QWORD frameTime = 0;

	Log(">SendVideo [width:%d,size:%d,bitrate:%d,fps:%d,intra:%d]\n",width,height,bitrate,fps,intraPeriod);

	//The time of the first one
	gettimeofday(&prev,NULL);

	//Started
	Log("-Sending video\n");

	//Mientras tengamos que capturar
	while (encoding && loop.IsThreadRunning())
	{
		//Nos quedamos con el puntero antes de que lo cambien
		PictPtr pic = input->GrabFrame(frameTime/1000);

		//Check picture
		if (!pic)
			//Exit
			continue;

		//Attendre l'échéance de l'image AVANT de l'encoder : c'est cette attente
		//qui impose `fps` en sortie, le mixeur produisant à son propre rythme.
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
			//Dormir jusqu'à l'échéance prev+sleep (µs, réveillé par StopThread)
			QWORD elapsed = getDifTime(&prev);
			if (sleep > elapsed)
				loop.Pacer().WaitSignal(std::chrono::microseconds(sleep - elapsed));
			//Arrêt demandé pendant l'attente : ne pas émettre une image de plus
			if (!encoding || !loop.IsThreadRunning())
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

		if (EncodePicture(pic) <= 0)
			continue;

		//Set sending time of previous frame
		getUpdDifTime(&prev);

		//Set frame time
		frameTime = 1000000/fps;
	}

	Log("<SendVideo [%d]\n",(int)encoding);
	return 0;
}
