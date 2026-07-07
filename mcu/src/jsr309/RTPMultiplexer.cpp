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
	pthread_mutex_init(&mutex,NULL);
	//No "no listener" log emitted yet
	lastNoListenerTs = 0;
	noListenerCount = 0;
}

RTPMultiplexer::~RTPMultiplexer()
{
	//Lock mutexk
	pthread_mutex_lock(&mutex);
	//Iterate
	/*for (Listeners::iterator it = listeners.begin(); it!=listeners.end(); ++it)
		//Remove stream
		(*it)->onEndStream();
	*/
	//Clean listeners
	listeners.clear();
	//Unlock
	pthread_mutex_unlock(&mutex);
	//Destroy mutex
	pthread_mutex_destroy(&mutex);
}
int  RTPMultiplexer::TryCodec(int codec)
{
    int rez1, rez2;
	//Lock mutexk
	pthread_mutex_lock(&mutex);
	//Iterate
	for (Listeners::iterator it = listeners.begin(); it!=listeners.end(); ++it)
        {
		//Update
		rez1 = (*it)->TryCheckCodec(codec);
                if (rez1 != codec) break;
        }

        for (Listeners::iterator it = listeners.begin(); it!=listeners.end(); ++it)
        {
		//Update
		rez2 = (*it)->TryCheckCodec(codec);
                if (rez2 != rez1) 
                {
                    rez2 = -1;
                    break;
                }
        }

	//Unlock
	pthread_mutex_unlock(&mutex);

        return rez2;
}

void RTPMultiplexer::Multiplex(RTPPacket &packet)
{
	//Lock mutexk
	pthread_mutex_lock(&mutex);
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
	pthread_mutex_unlock(&mutex);
}

void RTPMultiplexer::ResetStream()
{
	//Lock mutexk
	pthread_mutex_lock(&mutex);
	//Iterate
	for (Listeners::iterator it = listeners.begin(); it!=listeners.end(); ++it)
		//Update
		(*it)->onResetStream();
	//Unlock
	pthread_mutex_unlock(&mutex);
}

void RTPMultiplexer::EndStream()
{
	//Lock mutexk
	pthread_mutex_lock(&mutex);
	//Iterate
	for (Listeners::iterator it = listeners.begin(); it!=listeners.end(); ++it)
		//Update
		(*it)->onEndStream();
	//Unlock
	pthread_mutex_unlock(&mutex);
}

void RTPMultiplexer::AddListener(Listener *listener)
{
	//reset it
	listener->onResetStream();
	//Lock mutexk
	pthread_mutex_lock(&mutex);
	//Apend
	listeners.insert(listener);
	//Unlock
	pthread_mutex_unlock(&mutex);
}

void RTPMultiplexer::RemoveListener(Listener *listener)
{
	//Lock mutexk
	pthread_mutex_lock(&mutex);
	//Find it
	Listeners::iterator it = listeners.find(listener);
	//If present
	if (it!=listeners.end())
		//erase it
		listeners.erase(it);
	//Unlock
	pthread_mutex_unlock(&mutex);
}

void RTPMultiplexer::Update()
{
	//Should be overriden
}

void RTPMultiplexer::SetREMB(DWORD estimation)
{
	//Should be overriden
}
