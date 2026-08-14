#ifndef _XMLSTREAMINGHANDLER_H_
#define _XMLSTREAMINGHANDLER_H_
#include <xmlrpc.h>
#include <chrono>
#include <list>
#include <map>
#include <vector>
#include "config.h"
#include "xmlrpcserver.h"
#include "use.h"
#include "wait.h"


class XmlEvent
{
public:
	virtual ~XmlEvent() {}
	virtual xmlrpc_value* GetXmlValue(xmlrpc_env *env) = 0;
};

//File d'événements du long-poll XML-RPC, bâtie sur la primitive Wait
//(cf. wait.h). SÉMANTIQUE PIÈGE préservée (test_wait_sites.cpp) :
//WaitForEvent rend true MÊME sur timeout (déclencheur du keep-alive), et
//false SEULEMENT si annulée à l'ENTRÉE (Cancel en deux temps).
class XmlEventQueue : public Use, protected ::Wait
{
public:
	//Délai de grâce par défaut (secondes) sans aucun poller sur une file avant
	//que le service propriétaire ne la considère abandonnée — et détruise avec
	//elle les objets qui en dépendent. 60 s = deux keep-alive du long-poll
	//manqués (30 s), très au-dessus du délai de reconnexion d'un contrôleur
	//(~1 s) : aucun faux positif attendu. Porté ici, et non par un service, car
	//l'option --event-queue-expires vaut pour toutes les API (JSR309 puis MCU).
	static const int DefaultExpiresSecs = 60;

public:
	XmlEventQueue();
	virtual ~XmlEventQueue();
	void AddEvent(XmlEvent *event);
	using ::Wait::Cancel;
	bool WaitForEvent(DWORD timeout);
	xmlrpc_value* PeekXMLEvent(xmlrpc_env *env);
	void PopEvent();

	//--- Vitalité du client (expiration par event queue) --------------------
	//Le long-poll du client EST son battement de cœur : le handler HTTP
	//encadre sa boucle d'attente par Attach/DetachPoller, et une file que
	//plus personne ne polle signale un client mort (cf.
	//jsr309_session_expiry_plan.md §7). Volontairement distinct de
	//IncUse/DecUse, que d'autres chemins (AddEvent) prennent aussi.
	void AttachPoller();
	void DetachPoller();
	//Vraie si un poller est attaché OU s'il s'est détaché depuis moins de
	//`idle`. Une file jamais pollée compte depuis sa CRÉATION : les files
	//nues abandonnées (sonde de supervision morte, appel avorté avant le
	//média) expirent donc comme les autres.
	bool IsPolled(std::chrono::milliseconds idle);

private:
	typedef std::list<XmlEvent*> EventList;

private:
	//The event list
	EventList events;
	//État de vitalité, gardé sous le verrou de ::Wait (via Locked)
	int	pollers = 0;
	std::chrono::steady_clock::time_point lastPoller;
};

class XmlStreamingHandler :
	public Handler
{
public:
	XmlStreamingHandler();
	~XmlStreamingHandler();
	int CreateEventQueue();
	int AddEvent(DWORD id,XmlEvent *event);
	int DestroyEventQueue(DWORD id);
	virtual int ProcessRequest(TRequestInfo *req,TSession * const ses);
	int DestroyAllQueues();
	//Identifiants des files sans poller depuis plus de `idle` (files jamais
	//pollées comprises, cf. XmlEventQueue::IsPolled). Ne rend que des ids :
	//aucun pointeur de file ne survit au verrou, donc rien à invalider si une
	//file disparaît entre le recensement et son traitement.
	std::vector<DWORD> GetIdleQueues(std::chrono::milliseconds idle);
	//Cette file existe-t-elle encore ? (un objet dont le queueId ne désigne
	//plus rien a perdu son contrôleur, cf. eventqueuesweeper.h)
	bool HasQueue(DWORD id);

private:
	typedef std::map<DWORD,XmlEventQueue*> EventQueues;

private:
	EventQueues	queues;
	Use		listUse;
	DWORD		maxId;

};

#endif
