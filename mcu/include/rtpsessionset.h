/*
 * File:   rtpsessionset.h
 *
 * Reacteur : un thread et un poll() pour un GROUPE de PollHandler.
 * Conception : docs/conception/RTP-REACTOR/SPEC.md §3.3 et §3.4.
 */

#ifndef RTPSESSIONSET_H
#define	RTPSESSIONSET_H

#include "config.h"
#include "pollhandler.h"
#include "worker.h"

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class RtpSessionSet : public Worker
{
public:
	//Au-dela, un tour de boucle est trace (au plus une trace par seconde et par
	//groupe). Sans ce garde-fou, une regression de gigue introduite par un
	//callback trop long serait indiagnosticable.
	static const QWORD LongTurnUs = 50000;

	//Reacteur du processus, pour toute session dont personne n'a fixe le groupe :
	//il n'existe donc AUCUNE session portant son propre thread. Cree et demarre a
	//la premiere demande, et JAMAIS detruit — l'ordre de destruction des statiques
	//ne garantit pas qu'il meurt apres les sessions qui s'y sont inscrites.
	static RtpSessionSet& Default();

	explicit RtpSessionSet(const char* name);
	~RtpSessionSet() override;

	bool Start()	{ return StartThread(); }
	void Stop()	{ StopThread(); }

	//Inscription. A appeler APRES ouverture des descripteurs. Une seconde
	//inscription du meme handler est refusee et tracee.
	void Add(PollHandler* handler);

	//Retrait SYNCHRONE : au retour, le reacteur ne poll plus les descripteurs du
	//handler et n'appellera plus aucune de ses methodes — l'appelant peut alors
	//les fermer. Appele DEPUIS le reacteur, retire sans attendre : s'attendre
	//soi-meme ne se terminerait jamais.
	void Remove(PollHandler* handler);

	//Reveille le poll sans attendre d'evenement reseau.
	void Wake()	{ wait.Signal(); }

	//Introspection (statut, tests). Le tour le plus long est une PLUS HAUTE EAU
	//depuis le demarrage : il ne redescend pas.
	DWORD GetHandlerCount();
	QWORD GetLongestTurnUs();

protected:
	int Run() override;

private:
	struct Entry
	{
		PollHandler*	handler;
		int		offset;		//index de son 1er fd dans le tableau de poll
		int		count;
		bool		alive;		//abaissee par un retrait pendant le tour
	};

	//Toutes trois sous `mutex`.
	bool IsLiveLocked(PollHandler* handler) const;
	void KillInUseLocked(PollHandler* handler);
	void ForgetLocked(PollHandler* handler);

	std::string			name;
	std::mutex			mutex;
	std::condition_variable		quiesced;
	std::vector<PollHandler*>	handlers;	//le jeu inscrit, seule verite
	std::vector<Entry>		inUse;		//le tour en cours, vide entre deux
	std::thread::id			reactorId;
	int				wakeFd;
	bool				reactorRunning;
	QWORD				longestTurnUs;
	QWORD				lastLongTurnLogMs;
};

#endif	/* RTPSESSIONSET_H */
