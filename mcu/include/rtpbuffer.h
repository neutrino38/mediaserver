/*
 * File:   rtpbuffer.h
 * Author: Sergio
 *
 * Created on 24 de diciembre de 2012, 10:27
 *
 * Jitter buffer réordonnanceur, réécrit sur std::mutex /
 * std::condition_variable (chantier modernisation n°2). Sémantique historique
 * PRÉSERVÉE (figée par mcu/tests/test_wait_primitives.cpp) : livraison en
 * séquence étendue (cycles<<16|seq), trou livré après maxWaitTime compté
 * depuis l'ARRIVÉE du candidat, retardataire livré dès son arrivée pendant un
 * Wait bloqué, paquet plus vieux que `next` détruit (Add()==false), resynchro
 * au 21e hors-séquence consécutif, HurryUp() court-circuite l'attente,
 * Cancel() collant jusqu'à Reset().
 * Corrections vs l'implémentation pthread d'origine :
 *   - l'attente de comblement d'un trou est PASSIVE (l'ancienne échéance
 *     mélangeait ms et µs → timedwait toujours expiré → attente active) ;
 *   - un doublon de numéro de séquence est détruit (l'ancien écrasait le
 *     pointeur en map sans delete = fuite) — l'original, arrivé plus tôt,
 *     est conservé ;
 *   - le changement de SSRC est détecté en mémorisant le dernier SSRC VU
 *     (l'ancien lisait le dernier paquet EN FILE : tout changement survenant
 *     file vide passait par le drop « tardif ») ;
 *   - le destructeur annule puis DRAINE un éventuel Wait avant de libérer
 *     quoi que ce soit ; Length() et HurryUp() sont verrouillés.
 */

#ifndef RTPBUFFER_H
#define	RTPBUFFER_H
#include "rtp.h"

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>

class RTPBuffer
{
public:
	RTPBuffer() = default;
	RTPBuffer(const RTPBuffer&) = delete;
	RTPBuffer& operator=(const RTPBuffer&) = delete;

	virtual ~RTPBuffer()
	{
		//Annule puis draine un éventuel Wait avant de libérer les paquets
		{
			std::unique_lock<std::mutex> lock(mutex);
			cancel = true;
			cond.notify_all();
			drained.wait(lock, [this] { return waiters == 0; });
		}
		Clear();
	}

	virtual bool Add(RTPTimedPacket *rtp)
	{
		//Get seq num
		DWORD extseqn = rtp->GetExtSeqNum();

		std::unique_lock<std::mutex> lock(mutex);

		//Changement de source : dernier SSRC VU (pas le dernier en file, qui
		//manquait tout changement survenant file vide)
		if (hasSsrc && rtp->GetSSRC() != lastSsrc)
		{
			next = (DWORD)-1;
			bigJumps = 0;
		}
		lastSsrc = rtp->GetSSRC();
		hasSsrc = true;

		//If already past
		if (next != (DWORD)-1 && extseqn < next)
		{
			if (++bigJumps > 20)
			{
				Log("Too many out of sequence packet. Resyncing.\n");
				next = (DWORD)-1;
				bigJumps = 0;
			}
			else
			{
				WORD cycl = rtp->GetSeqCycles();
				WORD seqn = rtp->GetSeqNum();

				//Skip it and lost forever
				lock.unlock();
				delete rtp;
				return Error("-Out of order non recoverable packet [next:%d, seq:%d, maxWaitTime=%d,%d,%d]\n"
					, next
					, extseqn
					, maxWaitTime
					, cycl
					, seqn
					);
			}
		}

		//Doublon : conserver l'original (arrivé plus tôt), détruire l'entrant
		//(l'historique écrasait le pointeur sans delete = fuite)
		if (!packets.emplace(extseqn, rtp).second)
		{
			lock.unlock();
			delete rtp;
			return true;
		}

		//Signal
		cond.notify_one();

		return true;
	}

	void Cancel()
	{
		std::lock_guard<std::mutex> lock(mutex);
		cancel = true;
		cond.notify_all();
	}

	RTPPacket* Wait()
	{
		//NO packet
		RTPTimedPacket* rtp = NULL;

		std::unique_lock<std::mutex> lock(mutex);

		WaiterScope scope(*this);

		//While we have to wait
		while (!cancel)
		{
			//Check if we have somethin in queue
			if (!packets.empty())
			{
				//Get first
				RTPOrderedPackets::iterator it = packets.begin();
				//Get first seq num
				DWORD seq = it->first;
				//Get packet
				RTPTimedPacket* candidate = it->second;
				if (!candidate)
					break;
				//Get time of the packet and now, in ms
				QWORD time = candidate->GetTime();
				QWORD now  = getTime() / 1000;

				//Check if first is the one expected or wait if not
				if (next == (DWORD)-1 || seq == next || now >= time + maxWaitTime || hurryUp)
				{
					//We have it!
					rtp = candidate;
					//Update next
					next = seq + 1;
					//Remove it
					packets.erase(it);
					//Return it!
					break;
				}

				//Attente PASSIVE jusqu'à l'échéance du candidat (réveillée
				//avant si le paquet manquant arrive)
				cond.wait_for(lock, std::chrono::milliseconds(time + maxWaitTime - now));
			}
			else
			{
				//Not hurryUp more
				hurryUp = false;
				//Wait until we have a new rtp packet
				cond.wait(lock);
			}
		}

		//canceled
		return rtp;
	}

	void Clear()
	{
		std::lock_guard<std::mutex> lock(mutex);
		ClearPackets();
	}

	void HurryUp()
	{
		std::lock_guard<std::mutex> lock(mutex);
		hurryUp = true;
		cond.notify_all();
	}

	void Reset(bool clear = true)
	{
		std::lock_guard<std::mutex> lock(mutex);

		//And remove cancel
		cancel = false;

		//And remove all from queue
		if (clear)
		{
			ClearPackets();
			//No next
			next = (DWORD)-1;
			//Oublier la source suivie
			hasSsrc = false;
		}

		bigJumps = 0;
	}

	DWORD Length()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return packets.size();
	}

	void SetMaxWaitTime(DWORD maxWaitTime)
	{
		std::lock_guard<std::mutex> lock(mutex);
		this->maxWaitTime = maxWaitTime;
	}

private:
	//Appelé sous le verrou
	void ClearPackets()
	{
		//For each item, list shall be locked before
		for (RTPOrderedPackets::iterator it = packets.begin(); it != packets.end(); ++it)
			//Delete rtp
			delete(it->second);
		//Clear all list
		packets.clear();
	}

	//Comptage RAII des waiters : garantit le drain du destructeur même sur
	//sortie anticipée. Construit/détruit sous le verrou de la méthode.
	struct WaiterScope
	{
		RTPBuffer& b;
		WaiterScope(RTPBuffer& b) : b(b) { ++b.waiters; }
		~WaiterScope()
		{
			if (--b.waiters == 0)
				b.drained.notify_all();
		}
	};

	typedef std::map<DWORD,RTPTimedPacket*> RTPOrderedPackets;

	//The event list
	RTPOrderedPackets	packets;
	bool			cancel	 = false;
	bool			hurryUp	 = false;
	int			waiters	 = 0;
	std::mutex		mutex;
	std::condition_variable	cond;
	std::condition_variable	drained;
	DWORD			next	 = (DWORD)-1;
	DWORD			maxWaitTime = 0;
	int			bigJumps = 0;
	DWORD			lastSsrc = 0;
	bool			hasSsrc	 = false;
};

#endif	/* RTPBUFFER_H */
