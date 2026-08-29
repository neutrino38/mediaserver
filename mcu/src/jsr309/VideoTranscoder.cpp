/* 
 * File:   VideoTranscoder.cpp
 * Author: Sergio
 * 
 * Created on 19 de marzo de 2013, 12:32
 */

#include "VideoTranscoder.h"
#include "tools.h"
#include <cstdlib>

VideoTranscoder::VideoTranscoder(std::wstring &name)
{
	//Store tag
	this->tag = name;

	//Not inited
	inited = false;

	//Pas encore un paquet vu : le mode sera décidé sur le premier
	state = 0;
	recCodec = -1;
	allowBridging = false;
	//Aucune demande d'intra relayée pour l'instant
	setZeroTime(&lastSourceFPU);
	//Aucune cadence mesurée, aucune poussée
	lastPts = 0;
	hasLastPts = false;
	gapCount = 0;
	gapIndex = 0;
	gapSum = 0;
	appliedFps = 0;
	setZeroTime(&lastFpsApply);
	lastEncodedUs = 0;
	decimator.Reset();
	frameIndex = 0;
	lastDecimationLogUs = 0;
}

VideoTranscoder::~VideoTranscoder()
{
	//Check if ended properly
	if (inited)
		//End!!
		End();
}

int VideoTranscoder::Init(bool adaptative, bool allowBridging)
{
	Log("-Init VideoTranscoder [%ls,encoder:%p,decoder:%p,bridging:%d]\n",
	    tag.c_str(),&encoder,&decoder,allowBridging);

	//Encodeur POUSSÉ : plus de VideoPipe entre le décodeur et lui, donc plus de
	//thread d'encodage ni de duplication d'image à cadence constante (§3.3).
	encoder.Init();
	//Le décodeur nous livre ses images : nous sommes son VideoOutput.
	decoder.Init((VideoOutput*)this);
	//Inited
	inited = true;
        encoder.UseInputSize(adaptative);
	//Mode pont autorisé ou non ; le mode reste à décider sur le premier paquet
	this->allowBridging = allowBridging;
	state = 0;
	recCodec = -1;
	//OK
	return 1;
}
int VideoTranscoder::SetCodec(VideoCodec::Type codec,int mode,int fps,int bitrate,int intraPeriod, Properties & properties)
{
	int ret;
	Log("-VideoTranscoder: set codec for transcodeur %ls.\n", tag.c_str());
        if (properties.HasProperty("useInputSize"))
        {
            int adpt =  properties.GetProperty("useInputSize", 0);
            encoder.UseInputSize(adpt != 0);
            properties.erase(std::string("useInputSize"));
        }
	ret = encoder.SetCodec(codec,mode,fps,bitrate,intraPeriod, properties);
	//Nouvelle consigne : la cadence mesurée d'avant ne la borne plus. La fenêtre
	//repart, l'encodeur retourne à la consigne, et la mesure la rabaissera si la
	//source est vraiment plus lente (§3.6).
	if (ret)
	{
		ResetFrameRateWindow();
		appliedFps = 0;
		encoder.SetMeasuredFrameRate(0);
		//Autre codec, autre coût par image : le pas repart de 1.
		decimator.Reset();
		frameIndex = 0;
	}
	//Consigne changée alors que le pont est établi : l'encodeur qui l'aurait
	//appliquée n'est pas dans le chemin, la re-pousser à la source.
	if (ret && state == 2)
		PushSourceBitrateLimit();
	return ret;
}
int VideoTranscoder::End()
{
	Log("-End VideoTranscoder [%ls]\n",tag.c_str());
	//AVANT d'arrêter quoi que ce soit : en mode pont la source tient un
	//Joinable::Listener* sur NOUS. VideoTranscoderDelete appelle End() sans
	//passer par Dettach(), et le shared_ptr détruit l'objet en sortie de portée
	//— la source publierait alors dans un objet libéré. La sûreté mémoire ne
	//doit pas dépendre de l'ordre des appels du contrôleur.
	UnlistenSource();
	//End encoder and decoder
	encoder.End();
	decoder.End();
	//Not inited
	inited = false;
	//OK
	return 1;
}

void VideoTranscoder::AddListener(Joinable::Listener *listener)
{
	encoder.AddListener(listener);
}

void VideoTranscoder::Update()
{
	//Armer l'encodeur dans tous les cas : en transcodage c'est lui qui absorbe
	//la demande (N FIR aval → une seule intra, à la prochaine image), et si le
	//pont retombe en transcodage l'intra en attente servira à la reprise.
	encoder.Update();

	//Hors transcodage établi, l'encodeur n'est pas (ou pas encore) dans le
	//chemin : en pont (state 2) seule la SOURCE peut produire l'intra que le
	//puits réclame, et en probing (state 0) le StartSending du puits arrive
	//avant le premier paquet — demander l'intra à la source sert aux deux modes
	//futurs. En transcodage (state 1) on ne relaie PAS : l'absorption est la
	//valeur ajoutée du transcodeur. En mode transcodage seul (!allowBridging),
	//joined est vide et le relais est un no-op.
	if (state != 1)
		RequestSourceFPU();
}

void VideoTranscoder::SetREMB(DWORD estimation)
{
	//En mode pont, l'encodeur n'est pas dans le chemin : seule la source peut
	//baisser le débit du flux relayé. La demande du puits (TMMBR/REMB, en bps)
	//remonte donc à l'amont, bornée par la consigne négociée de la patte
	//émettrice — le puits ne peut pas « autoriser » plus que sa négociation.
	if (state == 2)
	{
		DWORD cap = ((DWORD)encoder.GetBitrate())*1000;	//kbps -> bps
		if (cap && estimation > cap)
			estimation = cap;

		if (std::shared_ptr<Joinable> j = joined.lock())
			j->SetREMB(estimation);
		return;
	}

	//Transcodage (ou mode encore inconnu) : l'encodeur absorbe la limite.
	encoder.SetREMB(estimation);
}

void VideoTranscoder::RemoveListener(Joinable::Listener *listener)
{
	encoder.RemoveListener(listener);
}

//Même politique que AudioTranscoder::onRTPPacket : le mode est décidé sur le
//codec RÉELLEMENT reçu, pas sur ce que le plan de contrôle a annoncé, et il est
//rejugé dès que ce codec change. `RTPMultiplexer::TryCodec` interroge tous les
//puits attachés — et `RTPEndpoint::TryCheckCodec` bascule au passage le codec
//d'émission du puits — donc un « oui » signifie que le paquet peut sortir tel
//quel.
void VideoTranscoder::onRTPPacket(RTPPacket &packet)
{
	if (!allowBridging)
	{
		decoder.onRTPPacket(packet);
		return;
	}

	if (recCodec != packet.GetCodec() || state == 0)
	{
		int previous = state;
		int ret = encoder.TryCodec(packet.GetCodec());

		if (ret == packet.GetCodec())
		{
			state = 2;
			Log("-VideoTranscoder: switched to bridged mode for codec %s.\n",
			    VideoCodec::GetNameFor((VideoCodec::Type) packet.GetCodec()));

			//Le puits recevait le flux de l'encodeur (ou rien du tout) : il
			//voit maintenant la continuation d'un flux qu'il n'a jamais vu,
			//illisible avant la prochaine intra périodique de la source — la
			//demander tout de suite. L'anti-tempête déduplique si le
			//StartSending du puits vient de le faire (probing).
			RequestSourceFPU();

			//En pont, plus personne n'applique la consigne négociée de la
			//patte émettrice : l'encodeur qui la tenait n'est plus dans le
			//chemin. La pousser à la source en TMMBR (kbps -> bps) — c'est
			//désormais à ELLE de s'y tenir. Sans quoi une source à 2 Mbps
			//arrose un puits qui n'en a négocié que 512.
			PushSourceBitrateLimit();
		}
		else
		{
			state = 1;
			auto outCodec = encoder.GetCodec();
			//Sens de lecture : (codec reçu) -> (codec émis). L'ordre inverse
			//historique a fait re-dériver le sens du flux depuis les rtpMaps
			//en recette (2026-08-14).
			Log("-VideoTranscoder: transcoding %s -> %s .\n",
			    VideoCodec::GetNameFor((VideoCodec::Type) packet.GetCodec()),
			    VideoCodec::GetNameFor(outCodec));

			//LA différence avec l'audio. Reprendre l'encodage en cours de flux ne
			//suffit pas pour de la vidéo : le puits vient de recevoir des paquets
			//relayés et attend la suite d'un flux qui change de source. Sans image
			//clé il affiche du bruit jusqu'à la prochaine intra périodique du
			//codeur — le gel classique. On force donc la FPU dès que l'encodeur
			//reprend la main, ce que fait déjà VideoTranscoderFPU par XML-RPC.
			//
			//Reste hors de notre portée : le DÉCODEUR a lui aussi besoin d'une
			//intra, mais de la SOURCE, et c'est l'endpoint amont qui la demande
			//(RTCP FIR/PLI). Sur un vrai changement de codec le pair en émet une
			//de lui-même ; la transition est loggée pour que le contraire se voie.
			if (previous != 0)
				encoder.Update();
		}

		recCodec = packet.GetCodec();
	}

	switch (state)
	{
		case 2: // pont : ni décodeur ni encodeur dans le chemin
			encoder.Multiplex(packet);
			break;

		case 1:
		default:
			decoder.onRTPPacket(packet);
			break;
	}
}
//── VideoOutput : la sortie du décodeur EST l'entrée de l'encodeur ───────────
//Le thread qui a livré le paquet RTP porte toute la chaîne : NextFrame s'exécute
//sous le verrou de multiplexage du port source, décodage compris.
int VideoTranscoder::NextFrame(PictPtr pic)
{
	if (!pic || !pic->GetAVFrame())
		return 0;

	//Cadence RÉELLE, mesurée sur l'horodatage RTP que le décodeur a posé sur
	//l'image — pas sur son heure d'arrivée (§3.6).
	MeasureFrameRate((DWORD)pic->GetAVFrame()->pts);

	//Décimation : une image sur k quand l'encodeur ne tient pas la cadence de
	//la source. Comptée sur TOUTES les images reçues, pour que les images
	//gardées soient régulièrement espacées.
	frameIndex++;
	const int step = decimator.GetStep();
	if (step > 1 && (frameIndex % (DWORD)step) != 0)
		return 1;

	//Cadence de SORTIE bornée par la consigne : c'est ce que faisait le
	//GrabFrame(1/fps) du thread supprimé.
	if (!DueForEncoding())
		return 1;

	//Coût de l'encodage, mesuré sur le thread qui le paie : c'est ce que ce
	//transcodeur ajoute au thread de démux de la source, et ce qu'il peut sauter.
	const QWORD before = getTime();
	const int encoded = encoder.EncodePicture(pic);
	const QWORD after = getTime();

	if (encoded)
	{
		const QWORD budget = SourceFrameBudgetUs();
		if (budget)
		{
			const bool changed = decimator.Observe(after - before, budget, after);
			if (changed)
				ApplyDecimatedFrameRate();
			LogDecimation(changed);
		}
	}
	return 1;
}

void VideoTranscoder::ApplyDecimatedFrameRate()
{
	const QWORD budget = decimator.GetBudgetUs();
	if (!budget)
		return;

	//Cadence réellement livrée à l'encodeur : celle de la source, divisée par
	//le pas. Un encodeur réglé à 20 im/s qui n'en reçoit que 7 émet 7/20 de son
	//débit : c'est le débit par image qu'il faut recaler, donc la cadence.
	int output = (int)(1000000ULL/budget) / decimator.GetStep();
	if (output < 1)
		output = 1;

	if (output == appliedFps)
		return;

	appliedFps = output;
	getUpdDifTime(&lastFpsApply);
	encoder.SetMeasuredFrameRate(output);
}

QWORD VideoTranscoder::SourceFrameBudgetUs() const
{
	if (gapCount < BudgetMinGaps || !gapSum)
		return 0;
	//Écart moyen en ticks 90 kHz → µs.
	return gapSum*1000000ULL/(90000ULL*(QWORD)gapCount);
}

void VideoTranscoder::LogDecimation(bool changed)
{
	const int step = decimator.GetStep();
	const QWORD now = getTime();

	if (!changed)
	{
		//Rappel périodique tant que des images sont sautées : un appel long
		//doit le montrer dans le log, pas seulement à l'instant du changement.
		if (step == 1 || !lastDecimationLogUs || now - lastDecimationLogUs < DecimationLogPeriodUs)
			return;
	}
	lastDecimationLogUs = now;

	const unsigned costMs = (unsigned)(decimator.GetCostUs()/1000);
	const unsigned budgetMs = (unsigned)(decimator.GetBudgetUs()/1000);
	const int sourceFps = decimator.GetBudgetUs() ? (int)(1000000ULL/decimator.GetBudgetUs()) : 0;

	if (step == 1)
	{
		Log("-VideoTranscoder: encodeur de nouveau dans les temps [%ls] : %u ms par image pour un budget de %u ms (source %d im/s) -> toutes les images encodees\n",
		    tag.c_str(), costMs, budgetMs, sourceFps);
		return;
	}

	Log("-VideoTranscoder: encodeur trop lent [%ls] : %u ms par image pour un budget de %u ms (source %d im/s) -> 1 image sur %d encodee, encodeur recale a %d im/s%s\n",
	    tag.c_str(), costMs, budgetMs, sourceFps, step, appliedFps ? appliedFps : sourceFps/step,
	    decimator.IsSaturated() ? " (PAS MAXIMAL : l'encodeur lui-meme est trop lent)" : "");
}

//Taille native du flux entrant : l'encodeur la suit quand useInputSize est armé.
//En mode tiré c'est le VideoInput qui la porte ; ici, c'est nous.
int VideoTranscoder::SetVideoSize(int width,int height)
{
	encoder.SetNativeSize((DWORD)width,(DWORD)height);
	return 0;
}

void VideoTranscoder::ResetFrameRateWindow()
{
	hasLastPts = false;
	gapCount = 0;
	gapIndex = 0;
	gapSum = 0;
}

//§3.6 — estime `fpsMesure = 90000 x (nombre d'écarts) / (somme des écarts)` sur
//les 30 derniers écarts, et ne pousse la valeur à l'encodeur que si la fenêtre
//est PLEINE, si elle s'écarte de plus de 25 % de celle en vigueur, et au plus
//une fois toutes les 5 s — chaque application coûte une trame clé.
void VideoTranscoder::MeasureFrameRate(DWORD pts)
{
	if (!hasLastPts)
	{
		lastPts = pts;
		hasLastPts = true;
		return;
	}

	//Arithmétique modulo 2^32 : le rebouclage normal de l'horodatage RTP passe
	//sans bruit, un vrai saut arrière donne un écart énorme et vide la fenêtre.
	const DWORD gap = pts - lastPts;
	lastPts = pts;

	//Une PAUSE n'est pas une cadence. Un mute vidéo laisse passer plusieurs
	//secondes sans image : compter ce temps ferait tomber la mesure vers 0,
	//rouvrir l'encodeur à 1 im/s, et la reprise serait encodée à 1 im/s. Un saut
	//non monotone (changement de SSRC, onResetStream) est traité pareil : ce qui
	//précède ne dit rien de ce qui suit.
	if (gap == 0 || gap > FpsPauseTicks)
	{
		ResetFrameRateWindow();
		//L'encodeur GARDE son fps d'avant la pause : c'est la seule valeur connue.
		return;
	}

	if (gapCount == FpsWindow)
		gapSum -= gaps[gapIndex];
	else
		gapCount++;

	gaps[gapIndex] = gap;
	gapSum += gap;
	gapIndex = (gapIndex + 1) % FpsWindow;

	//Fenêtre incomplète : rien n'est appliqué. Après une pause, il faut donc
	//30 images à la NOUVELLE cadence avant qu'elle produise le moindre effet.
	if (gapCount < FpsWindow || gapSum == 0)
		return;

	int measured = (int)((90000ULL*(QWORD)FpsWindow + gapSum/2) / gapSum);
	//Ce que l'encodeur reçoit vraiment : la cadence de la source divisée par
	//le pas de décimation. C'est sur cette cadence-là que son débit par image
	//et sa période intra doivent être calés.
	measured /= decimator.GetStep();
	if (measured < 1)
		measured = 1;

	//Hystérésis : la valeur en vigueur est celle déjà poussée, à défaut la
	//consigne (l'encodeur y tourne tant que rien n'a été mesuré).
	const int inforce = appliedFps ? appliedFps : encoder.GetConfiguredFps();
	if (inforce <= 0)
		return;
	if (abs(measured - inforce)*4 <= inforce)
		return;

	//Au plus une application toutes les 5 s.
	if (getDifTime(&lastFpsApply) < 5000000)
		return;

	Log("-VideoTranscoder: cadence source mesuree %d im/s, appliquee a l'encodeur [%ls, en vigueur %d im/s]\n",
	    measured, tag.c_str(), inforce);

	appliedFps = measured;
	getUpdDifTime(&lastFpsApply);
	encoder.SetMeasuredFrameRate(measured);
}

//Borne la cadence de SORTIE à la consigne, et à elle seule : la cadence MESURÉE
//décrit ce que la source envoie, écarter des images sur elle jetterait
//précisément celles qui la produisent. Tolérance de 10 % pour absorber la gigue
//d'une source qui émet déjà à la consigne.
bool VideoTranscoder::DueForEncoding()
{
	const int cap = encoder.GetConfiguredFps();
	if (cap <= 0)
		return true;

	const QWORD periodUs = 1000000/(QWORD)cap;
	const QWORD now = getTime();

	if (lastEncodedUs && now > lastEncodedUs && now - lastEncodedUs < periodUs - periodUs/10)
		return false;

	lastEncodedUs = now;
	return true;
}

void VideoTranscoder::onResetStream()
{
	//Nouveau flux : les écarts d'avant ne disent rien de la cadence qui suit.
	ResetFrameRateWindow();
	decoder.onResetStream();
}
void VideoTranscoder::onEndStream()
{
	decoder.onEndStream();
}

//Phase 5 (nego_fmtp §6.3) : l'endpoint écoute le transcodeur, mais c'est son
//encodeur qui produit — les bornes descendent d'un cran.
void VideoTranscoder::SetNegotiatedCodecProperties(const std::map<int,Properties>& byCodec)
{
	encoder.SetNegotiatedCodecProperties(byCodec);
}

void VideoTranscoder::UnlistenSource()
{
	//lock() : la source est-elle encore vivante ?
	if (std::shared_ptr<Joinable> j = joined.lock())
		j->RemoveListener(this);

	joined.reset();
}

void VideoTranscoder::PushSourceBitrateLimit()
{
	//Consigne inconnue tant que SetCodec n'a pas été appelé : rien à imposer.
	int kbps = encoder.GetBitrate();
	if (kbps <= 0)
		return;

	//lock() : la source est-elle encore vivante ?
	if (std::shared_ptr<Joinable> j = joined.lock())
	{
		Log("-VideoTranscoder: pushing negotiated bitrate limit to source [%ls,%d kbps]\n",
		    tag.c_str(), kbps);
		j->SetREMB(((DWORD)kbps)*1000);
	}
}

void VideoTranscoder::RequestSourceFPU()
{
	//Borne : une demande par seconde, même cadence que lastFPURequest du
	//décodeur. Le compteur n'avance que si la demande part réellement.
	if (getDifTime(&lastSourceFPU)<1000000)
		return;

	//lock() : la source est-elle encore vivante ?
	if (std::shared_ptr<Joinable> j = joined.lock())
	{
		Log("-VideoTranscoder: requesting FPU from source [%ls]\n",tag.c_str());
		getUpdDifTime(&lastSourceFPU);
		j->Update();
	}
}

//Returning 0 here made every VideoTranscoderAttachToEndpoint/Dettach XML-RPC
//call answer an error while the attach had in fact happened — a controller that
//checks the status tears the call down over a success.
//
//Même forme qu'AudioTranscoder::Attach, et pour la même raison : en mode pont
//c'est le TRANSCODEUR qui doit voir chaque paquet, puisque c'est onRTPPacket qui
//arbitre relais ou transcodage sur le codec réellement reçu. Brancher la source
//directement sur le décodeur, comme le faisait cette fonction, ne « désactive »
//pas le pont — il rend l'arbitrage inatteignable : onRTPPacket n'est jamais
//appelé, TryCodec jamais interrogé, `state` reste à 0, et tout le chemin pont
//est du code mort. Le symptôme n'est pas une perte de performance mais une perte
//de média : le 2026-08-12, un appel AV1 ↔ AV1 (les deux pattes s'accordant sur
//AV1, donc relayable tel quel) a décodé en libdav1d un flux qu'aucun
//dépaquetiseur AV1 ne préparait — « Unknown OBU type 11 », pas une image
//décodée, pas une image ré-encodée, appel établi et écran noir des deux côtés,
//avec en prime deux encodeurs SVT-AV1 ouverts pour rien. L'audio, lui, passait :
//AudioTranscoder::Attach honore allowBridging depuis toujours.
int VideoTranscoder::Attach(const std::shared_ptr<Joinable> & join)
{
	//Transcodage seul : la source alimente le décodeur, comme avant.
	if (!allowBridging)
	{
		decoder.Attach(join);
		return 1;
	}

	//Une source précédente ne doit pas continuer à nous publier des paquets.
	UnlistenSource();

	joined = join;

	//Le mode se rejuge sur le premier paquet de la NOUVELLE source : son codec
	//n'a aucune raison d'être celui de la précédente.
	state = 0;
	recCodec = -1;
	//Nouvelle source = nouveau flux : ne pas bloquer sa première demande
	//d'intra sur le compteur de la précédente.
	setZeroTime(&lastSourceFPU);
	//... ni la juger sur la cadence de la précédente : l'encodeur repart de la
	//consigne, la mesure la rabaissera si besoin (§3.6).
	ResetFrameRateWindow();
	appliedFps = 0;
	setZeroTime(&lastFpsApply);
	lastEncodedUs = 0;
	decimator.Reset();
	frameIndex = 0;
	lastDecimationLogUs = 0;
	encoder.SetMeasuredFrameRate(0);

	//Le décodeur n'est plus alimenté par la source mais à la main, depuis
	//onRTPPacket, quand l'arbitrage retombe sur le transcodage. Il faut donc
	//démarrer son worker sans l'attacher (exactement ce que fait l'audio).
	decoder.Start();

	//... mais il doit connaître la source : c'est à ELLE que ses demandes de
	//FPU s'adressent (perte de paquets, erreur de décodage). Sans ce lien,
	//joined restait vide côté décodeur et les demandes échouaient en silence —
	//plus aucune FIR vers l'amont en mode transcodage, gel jusqu'à l'intra
	//périodique de la source.
	decoder.SetSource(join);

	if (join)
		join->AddListener(this);

	return 1;
}

int VideoTranscoder::Dettach()
{
	//En mode pont, c'est nous qui sommes inscrit auprès de la source : sans ce
	//retrait elle garderait un pointeur sur cet objet, et continuerait à publier
	//dedans après le détachement.
	UnlistenSource();

	//En mode transcodage seul, c'est le décodeur qui était inscrit et qui se
	//retire ; en mode pont, il n'était pas attaché et Dettach() se réduit à
	//l'arrêt de son worker — ce qu'on veut dans les deux cas.
	decoder.Dettach();
	return 1;
}