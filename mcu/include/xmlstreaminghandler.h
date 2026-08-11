#ifndef _XMLSTREAMINGHANDLER_H_
#define _XMLSTREAMINGHANDLER_H_
#include <xmlrpc.h>
#include <list>
#include <map>
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
	XmlEventQueue() = default;
	virtual ~XmlEventQueue();
	void AddEvent(XmlEvent *event);
	using ::Wait::Cancel;
	bool WaitForEvent(DWORD timeout);
	xmlrpc_value* PeekXMLEvent(xmlrpc_env *env);
	void PopEvent();

private:
	typedef std::list<XmlEvent*> EventList;

private:
	//The event list
	EventList events;
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

private:
	typedef std::map<DWORD,XmlEventQueue*> EventQueues;

private:
	EventQueues	queues;
	Use		listUse;
	DWORD		maxId;

};

#endif
