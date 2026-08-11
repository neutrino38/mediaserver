/* 
 * File:   RTPMultiplexer.cpp
 * Author: Sergio
 * 
 * Created on 7 de septiembre de 2011, 12:19
 */
#include <sys/time.h>
#include "log.h"
#include "RTPMultiplexer.h"

RTPMultiplexer::RTPMultiplexer()
{
	//Create mutex
	//No "no listener" log emitted yet
	lastNoListenerTs = 0;
	noListenerCount = 0;
}

RTPMultiplexer::~RTPMultiplexer()
{
	//Lock mutexk
	std::unique_lock<std::mutex> mutexLock(mutex);
	//C-13 (lien A) : le lien retour `joined` de chaque listener est désormais un
	//weak_ptr. La destruction de cette source (dernier shared_ptr relâché) fait
	//expirer ces weak_ptr : le Detach ultérieur du listener lock() dans le vide et
	//ne déréférence plus cet objet libéré. Plus besoin de notifier onJoinableEnded.
	listeners.clear();
	//Unlock
	mutexLock.unlock();
	//Destroy mutex
}
void RTPMultiplexer::SetNegotiatedCodecProperties(const std::map<int,Properties>& byCodec)
{
	std::unique_lock<std::mutex> mutexLock(mutex);
	negotiated = byCodec;
	mutexLock.unlock();
}

Properties RTPMultiplexer::GetNegotiatedProperties(int codec)
{
	std::unique_lock<std::mutex> mutexLock(mutex);
	std::map<int,Properties>::const_iterator it = negotiated.find(codec);
	Properties props = (it != negotiated.end()) ? it->second : Properties();
	mutexLock.unlock();
	return props;
}

int  RTPMultiplexer::TryCodec(int codec)
{
	//Lock mutexk
	std::unique_lock<std::mutex> mutexLock(mutex);

	//Aucun endpoint attaché : on ne peut rien affirmer sur le codec.
	if (listeners.empty())
	{
		mutexLock.unlock();
		return -1;
	}

	//Le codec est accepté seulement si TOUS les endpoints listeners l'acceptent
	//(présent dans leur rtpMap de sortie négociée).
	int result = codec;
	for (Listeners::iterator it = listeners.begin(); it!=listeners.end(); ++it)
	{
		if ((*it)->TryCheckCodec(codec) != codec)
		{
			result = -1;
			break;
		}
	}

	//Unlock
	mutexLock.unlock();

	return result;
}

void RTPMultiplexer::Multiplex(RTPPacket &packet)
{
	//Lock mutexk
	std::unique_lock<std::mutex> mutexLock(mutex);
	//Iterate
	for (Listeners::iterator it = listeners.begin(); it!=listeners.end(); ++it)
		//Update
		(*it)->onRTPPacket(packet);
	if (listeners.size() == 0)
	{
		//Compte les paquets ignorés et limite le log à 1/s pour éviter le flood
		noListenerCount++;
		struct timeval tv;
		gettimeofday(&tv,0);
		QWORD nowMs = (QWORD)tv.tv_sec*1000 + tv.tv_usec/1000;
		if (nowMs - lastNoListenerTs >= 1000)
		{
			Log("-RTPMultiplexer: no listener (%u packets ignored).\n", noListenerCount);
			lastNoListenerTs = nowMs;
			noListenerCount = 0;
		}
	}
	//Unlock
	mutexLock.unlock();
}

void RTPMultiplexer::ResetStream()
{
	//Lock mutexk
	std::unique_lock<std::mutex> mutexLock(mutex);
	//Iterate
	for (Listeners::iterator it = listeners.begin(); it!=listeners.end(); ++it)
		//Update
		(*it)->onResetStream();
	//Unlock
	mutexLock.unlock();
}

void RTPMultiplexer::EndStream()
{
	//Lock mutexk
	std::unique_lock<std::mutex> mutexLock(mutex);
	//Iterate
	for (Listeners::iterator it = listeners.begin(); it!=listeners.end(); ++it)
		//Update
		(*it)->onEndStream();
	//Unlock
	mutexLock.unlock();
}

void RTPMultiplexer::AddListener(Listener *listener)
{
	//reset it
	listener->onResetStream();
	//Lock mutexk
	std::unique_lock<std::mutex> mutexLock(mutex);
	//Apend
	listeners.insert(listener);
	//Unlock
	mutexLock.unlock();
}

void RTPMultiplexer::RemoveListener(Listener *listener)
{
	//Lock mutexk
	std::unique_lock<std::mutex> mutexLock(mutex);
	//Find it
	Listeners::iterator it = listeners.find(listener);
	//If present
	if (it!=listeners.end())
		//erase it
		listeners.erase(it);
	//Unlock
	mutexLock.unlock();
}

void RTPMultiplexer::Update()
{
	//Should be overriden
}

void RTPMultiplexer::SetREMB(DWORD estimation)
{
	//Should be overriden
}
