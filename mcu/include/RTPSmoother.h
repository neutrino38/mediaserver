/* 
 * File:   RTPSmoother.h
 * Author: Sergio
 *
 * Created on 7 de noviembre de 2011, 12:18
 */

#ifndef RTPSMOOTHER_H
#define	RTPSMOOTHER_H

#include "config.h"
#include "worker.h"
#include "waitqueue.h"
#include "rtp.h"
#include "rtpsession.h"

class RTPSmoother : public Worker
{
public:
	RTPSmoother();
	~RTPSmoother();
	int Init(RTPSession *session);
	int SendFrame(MediaFrame* frame,DWORD duration);
	int Cancel();
	int End();

protected:
	//Corps du Worker
	virtual int Run();

private:
	//Pacer a budget : chaque paquet porte SON temps de passage sur le fil
	//(SetSendingTime, en us) et `nextSendUs` est un curseur qui les enchaine.
	//La dette se REPORTE d'une image a l'autre — une trame cle s'etale sur
	//plus que sa propre periode au lieu de partir en rafale. Deux bornes :
	//jamais plus de MaxAheadUs d'avance (c'est de la latence), et pas de
	//rattrapage en rafale apres un silence (le curseur ne reste pas dans le
	//passe). Le lissage par image sans memoire d'avant faisait mesurer nos
	//propres rafales a l'estimateur emetteur (mesure du 2026-08-20 : le debit
	//acquitte variait de 2,7 x a l'interieur d'une meme seconde).
	static const QWORD MaxAheadUs = 100000;
	//Etalement maximal d'UNE image. C'est une borne de LATENCE, pas la periode
	//d'image : les appelants plafonnaient a la periode, ce qui tronquait
	//l'etalement d'une trame cle (2,2 x une trame inter, mesure du 2026-08-20)
	//et la faisait partir en rafale. Une trame cle peut donc s'etaler sur
	//plusieurs periodes, jamais au-dela de cette borne.
	static const QWORD MaxSpreadUs = 200000;

	RTPSession	*session;
	bool		inited;
	QWORD		nextSendUs;
	WaitQueue<RTPPacketSched*> queue;
};

#endif	/* RTPSMOOTHER_H */

