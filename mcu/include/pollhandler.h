/*
 * File:   pollhandler.h
 *
 * Interface des objets battus par un RtpSessionSet.
 * Conception : docs/conception/RTP-REACTOR/SPEC.md §3.2.
 */

#ifndef POLLHANDLER_H
#define	POLLHANDLER_H

#include "config.h"

#include <poll.h>

class PollHandler
{
public:
	//Nombre maximal de descripteurs qu'un handler peut demander. Deux
	//aujourd'hui (RTP, RTCP) ; la marge evite de retoucher le reacteur pour
	//une jambe qui en voudrait un troisieme.
	static const int MaxPollFds = 4;

	virtual ~PollHandler() = default;

	//Descripteurs a surveiller. Remplit `fds` (au plus `max`, evenements
	//compris), rend le nombre pose. Interroge a CHAQUE tour : un handler qui
	//change de descripteur n'a rien a signaler.
	virtual int  GetPollFds(pollfd* fds, int max) = 0;

	//Dans combien de ms ce handler veut-il la main, meme sans evenement ?
	//-1 = jamais. Le reacteur prend le minimum sur tout le groupe.
	virtual int  GetNextTimeoutMs(QWORD nowUs) = 0;

	//Les `revents` des descripteurs rendus par GetPollFds, dans le MEME ordre.
	//Appele SEULEMENT lorsqu'au moins un evenement est pose : un tour sans rien
	//ne coute pas un appel par handler.
	virtual void OnPollEvents(const pollfd* fds, int count, QWORD nowUs) = 0;

	//Travail periodique, appele a CHAQUE tour apres OnPollEvents — y compris
	//quand le reveil venait d'un autre handler du groupe. Chaque travail
	//garde sa propre horloge, donc un appel en trop ne coute que le test.
	virtual void OnPeriodic(QWORD nowUs) = 0;

	//Transport mort (POLLERR/POLLHUP/POLLNVAL). Le reacteur retire le handler
	//APRES cet appel ; il ne s'arrete pas lui-meme.
	virtual void OnPollError(short revents) = 0;
};

#endif	/* POLLHANDLER_H */
