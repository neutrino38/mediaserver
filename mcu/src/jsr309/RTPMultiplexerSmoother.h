/* 
 * File:   RTPSmoother.h
 * Author: Sergio
 *
 * Created on 7 de noviembre de 2011, 12:18
 */

#ifndef RTPMULTIPLEXERSMOOTHER_H
#define	RTPMULTIPLEXERSMOOTHER_H

#include "config.h"
#include "worker.h"
#include "waitqueue.h"
#include "rtp.h"
#include "RTPMultiplexer.h"


class RTPMultiplexerSmoother :
	public RTPMultiplexer,
	public Worker
{
public:
	RTPMultiplexerSmoother();
	virtual ~RTPMultiplexerSmoother();
	int Start();
	int SmoothFrame(MediaFrame* frame,DWORD duration);
	int Cancel();
	int Wait();
	int Stop();

protected:
	//Corps du Worker
	virtual int Run();

	//Nouveau run d'encodage SANS redémarrer le lisseur : la base de temps de
	//l'encodeur repart de zéro, donc la RFC 3550 veut une nouvelle identité de
	//source. Depuis le lot 4, une renégociation ne passe plus par Stop/Start —
	//c'est le chemin des paquets qui rouvre l'encodeur, et c'est lui qui doit
	//annoncer le nouveau run.
	void RenewSSRC() { ssrc = random(); }

private:
	//Pacer a budget, meme mecanique que RTPSmoother (cf. son en-tete) : chaque
	//paquet porte son temps de passage sur le fil et `nextSendUs` les enchaine,
	//donc la dette d'une trame cle se reporte au lieu de partir en rafale.
	static const QWORD MaxAheadUs = 100000;
	//Etalement maximal d'UNE image : borne de LATENCE (cf. RTPSmoother)
	static const QWORD MaxSpreadUs = 200000;

	bool		inited;
	QWORD		nextSendUs;
	WaitQueue<RTPPacketSched*> queue;
	//SSRC du run d'encodage courant, posé sur chaque paquet produit. Tiré à
	//neuf à chaque Start() : un encodeur relancé (SetCodec d'une renégociation)
	//repart d'une base de temps à lui, et la RFC 3550 veut que cette nouvelle
	//source s'annonce par un SSRC neuf — le pair resynchronise alors proprement
	//au lieu de voir la base sauter dans un flux continu.
	DWORD		ssrc;
};

#endif	/* RTPMULTIPLEXERSMOOTHER_H */

