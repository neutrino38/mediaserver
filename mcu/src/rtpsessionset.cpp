/*
 * File:   rtpsessionset.cpp
 *
 * Conception : docs/conception/RTP-REACTOR/SPEC.md §3.3 et §3.4.
 */

#include "rtpsessionset.h"
#include "log.h"
#include "tools.h"

#include <algorithm>
#include <errno.h>
#include <fcntl.h>
#include <string.h>

RtpSessionSet& RtpSessionSet::Default()
{
	//Fuite deliberee : cf. la declaration.
	static RtpSessionSet* def = [] {
		RtpSessionSet* set = new RtpSessionSet("default");
		set->Start();
		return set;
	}();
	return *def;
}

RtpSessionSet::RtpSessionSet(const char* name)
	: name(name ? name : "unnamed")
	, reactorRunning(false)
	, longestTurnUs(0)
	, lastLongTurnLogMs(0)
{
	//Le Wait cree son eventfd paresseusement. L'obtenir ici, une fois pour
	//toutes, evite que le reveil ne depende de l'ordre dans lequel le thread du
	//reacteur et un appelant de Wake() y touchent en premier.
	wakeFd = wait.GetPollFd();
}

RtpSessionSet::~RtpSessionSet()
{
	//Contrat Worker : le destructeur DERIVE arrete le thread.
	StopThread();

	std::lock_guard<std::mutex> lock(mutex);
	if (!handlers.empty())
		Error("-RtpSessionSet [%s] detruit avec %zu handler(s) encore inscrit(s)\n",
			name.c_str(), handlers.size());
}

bool RtpSessionSet::IsLiveLocked(PollHandler* handler) const
{
	for (std::vector<Entry>::const_iterator it = inUse.begin(); it != inUse.end(); ++it)
		if (it->handler == handler && it->alive)
			return true;
	return false;
}

void RtpSessionSet::KillInUseLocked(PollHandler* handler)
{
	//On ABAISSE sans effacer : le reacteur itere sur `inUse`, et lui retirer une
	//entree sous les pieds invaliderait son parcours.
	for (std::vector<Entry>::iterator it = inUse.begin(); it != inUse.end(); ++it)
		if (it->handler == handler)
			it->alive = false;
}

void RtpSessionSet::ForgetLocked(PollHandler* handler)
{
	handlers.erase(std::remove(handlers.begin(), handlers.end(), handler), handlers.end());
}

void RtpSessionSet::Add(PollHandler* handler)
{
	if (!handler)
		return;

	{
		std::lock_guard<std::mutex> lock(mutex);
		if (std::find(handlers.begin(), handlers.end(), handler) != handlers.end())
		{
			Error("-RtpSessionSet [%s] handler [%p] deja inscrit\n", name.c_str(), handler);
			return;
		}
		handlers.push_back(handler);
	}

	//Le reacteur dort peut-etre dans un poll sans echeance : sans reveil, le
	//nouveau handler ne serait servi qu'au prochain paquet d'un autre.
	Wake();
}

void RtpSessionSet::Remove(PollHandler* handler)
{
	if (!handler)
		return;

	std::unique_lock<std::mutex> lock(mutex);

	//Des maintenant, aucun tour suivant ne le prendra dans son instantane.
	ForgetLocked(handler);

	if (reactorId == std::this_thread::get_id() || !reactorRunning)
	{
		KillInUseLocked(handler);
		quiesced.notify_all();
		return;
	}

	//Le reacteur peut etre dans son poll : le reveiller, sinon l'attente durerait
	//jusqu'au prochain paquet. Hors de NOTRE verrou, pour ne jamais imbriquer
	//celui du Wait dans le notre.
	lock.unlock();
	Wake();
	lock.lock();

	quiesced.wait(lock, [this, handler] { return !IsLiveLocked(handler); });
}

DWORD RtpSessionSet::GetHandlerCount()
{
	std::lock_guard<std::mutex> lock(mutex);
	return handlers.size();
}

QWORD RtpSessionSet::GetLongestTurnUs()
{
	std::lock_guard<std::mutex> lock(mutex);
	return longestTurnUs;
}

int RtpSessionSet::Run()
{
	Log(">RtpSessionSet [%s]\n", name.c_str());

	{
		std::lock_guard<std::mutex> lock(mutex);
		reactorId	= std::this_thread::get_id();
		reactorRunning	= true;
	}

	std::vector<pollfd> fds;

	while (IsThreadRunning())
	{
		//Instantane du debut de tour. Le reacteur ne travaille QUE sur `inUse` :
		//c'est ce qui permet a un callback d'inscrire ou de retirer une session
		//sans casser le parcours en cours.
		{
			std::lock_guard<std::mutex> lock(mutex);
			inUse.clear();
			for (std::vector<PollHandler*>::iterator it = handlers.begin(); it != handlers.end(); ++it)
			{
				Entry e;
				e.handler	= *it;
				e.offset	= 0;
				e.count		= 0;
				e.alive		= true;
				inUse.push_back(e);
			}
			//Un Remove en attente peut etre satisfait par cet instantane.
			quiesced.notify_all();
		}

		QWORD now = getTime();

		//Le descripteur de reveil est toujours le premier : Wake(), et l'arret du
		//Worker, passent par lui.
		fds.clear();
		pollfd wake;
		wake.fd		= wakeFd;
		wake.events	= POLLIN;
		wake.revents	= 0;
		fds.push_back(wake);

		//Les descripteurs sont redemandes a chaque tour : un handler qui change
		//de socket n'a donc rien a signaler au reacteur.
		int timeout = -1;
		for (std::vector<Entry>::iterator it = inUse.begin(); it != inUse.end(); ++it)
		{
			pollfd mine[PollHandler::MaxPollFds];
			memset(mine, 0, sizeof(mine));
			int n = it->handler->GetPollFds(mine, PollHandler::MaxPollFds);
			if (n < 0)
				n = 0;
			if (n > PollHandler::MaxPollFds)
				n = PollHandler::MaxPollFds;

			it->offset = (int)fds.size();
			it->count  = n;
			for (int i = 0; i < n; ++i)
			{
				mine[i].revents = 0;
				fds.push_back(mine[i]);
			}

			int t = it->handler->GetNextTimeoutMs(now);
			if (t >= 0 && (timeout < 0 || t < timeout))
				timeout = t;
		}

		int nready = poll(&fds[0], fds.size(), timeout);
		if (nready < 0)
		{
			if (errno == EINTR || errno == EAGAIN)
				continue;

			//Un descripteur invalide vient d'un handler qui a ferme son socket sans
			//passer par Remove. Le retirer plutot que sortir : un handler ne doit
			//jamais rendre sourd tout son groupe.
			int removed = 0;
			if (errno == EBADF)
			{
				for (std::vector<Entry>::iterator it = inUse.begin(); it != inUse.end(); ++it)
				{
					bool bad = false;
					for (int i = 0; i < it->count; ++i)
						if (fcntl(fds[it->offset + i].fd, F_GETFD) < 0)
							bad = true;
					if (!bad || !it->alive)
						continue;
					Error("-RtpSessionSet [%s] descripteur invalide, retrait du handler [%p]\n",
						name.c_str(), it->handler);
					it->handler->OnPollError(POLLNVAL);
					std::lock_guard<std::mutex> lock(mutex);
					ForgetLocked(it->handler);
					it->alive = false;
					++removed;
					quiesced.notify_all();
				}
			}

			if (removed)
				continue;

			//Erreur qui ne vient d'aucun handler (le descripteur de reveil, une
			//limite systeme). Rien a retirer, donc rien qui changera au tour
			//suivant : sans cette garde, la boucle tournerait a plein coeur.
			Error("-RtpSessionSet [%s] poll error: errno=%d (%s)\n",
				name.c_str(), errno, strerror(errno));
			msleep(10000);
			continue;
		}

		QWORD turnStart = getTime();

		//Purger l'eventfd, SINON il reste lisible et chaque poll suivant rend la
		//main immediatement : la boucle tourne alors a 100 % d'un coeur.
		if (fds[0].revents & POLLIN)
			wait.Drain();

		now = getTime();

		for (std::vector<Entry>::iterator it = inUse.begin(); it != inUse.end(); ++it)
		{
			if (!it->alive)
				continue;

			short revents = 0;
			for (int i = 0; i < it->count; ++i)
				revents |= fds[it->offset + i].revents;

			//Seulement s'il s'est passe quelque chose : avec beaucoup de jambes,
			//un tour sans evenement ne doit pas couter N appels a vide.
			if (revents)
				it->handler->OnPollEvents(&fds[it->offset], it->count, now);

			//Un callback a pu se retirer lui-meme.
			if (!it->alive)
				continue;
			it->handler->OnPeriodic(now);
			if (!it->alive)
				continue;

			short bad = revents & (POLLERR | POLLHUP | POLLNVAL);
			if (!bad)
				continue;

			Log("-RtpSessionSet [%s] evenement socket 0x%x, retrait du handler [%p]\n",
				name.c_str(), bad, it->handler);
			it->handler->OnPollError(bad);

			std::lock_guard<std::mutex> lock(mutex);
			ForgetLocked(it->handler);
			it->alive = false;
			quiesced.notify_all();
		}

		QWORD turnUs = getTime() - turnStart;

		{
			std::lock_guard<std::mutex> lock(mutex);
			inUse.clear();
			if (turnUs > longestTurnUs)
				longestTurnUs = turnUs;
			quiesced.notify_all();
		}

		if (turnUs >= LongTurnUs)
		{
			QWORD nowMs = getTime() / 1000;
			if (nowMs >= lastLongTurnLogMs + 1000)
			{
				lastLongTurnLogMs = nowMs;
				Log("-RtpSessionSet [%s] tour long : %llu ms — les autres jambes du groupe ont attendu\n",
					name.c_str(), (unsigned long long)(turnUs / 1000));
			}
		}
	}

	{
		std::lock_guard<std::mutex> lock(mutex);
		reactorRunning = false;
		inUse.clear();
		quiesced.notify_all();
	}

	Log("<RtpSessionSet [%s]\n", name.c_str());
	return 0;
}
