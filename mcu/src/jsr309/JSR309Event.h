/* 
 * File:   JSR309EVENT.h
 * Author: Sergio
 *
 * Created on 8 de septiembre de 2011, 13:06
 */

#ifndef JSR309EVENT_H
#define	JSR309EVENT_H

#include "media.h"
#include "xmlstreaminghandler.h"

class JSR309EventContext
{
public:
	int						joinableId;
	MediaFrame::Type 		media;
	MediaFrame::MediaRole 	role;
	
	JSR309EventContext(int joinableId, MediaFrame::Type media, MediaFrame::MediaRole role);
	
	void FillEventContext(const JSR309EventContext & ctx);
};

class JSR309Event : public XmlEvent, protected JSR309EventContext
{
public:
	// ⚠️ Contrat de fil partagé avec elixip et les clients Java : ces valeurs
	// numériques ne doivent JAMAIS être réutilisées ni réordonnées.
	enum Events
	{
		PlayerEndOfFileEvent		= 1,
		ExternalFIRRequestedEvent	= 2,
		PlayerStartedEvent			= 3,
		RecorderStartedEvent		= 4,
		RecorderStoppedEvent		= 5,
		EndpointDisconnectedEvent	= 6,	// watchdog d'inactivité RTP (gap 5)
		EndpointConnectedEvent		= 7	// P5 : média établi (DTLS OK + 1er RTP reçu)
	};
public:
	JSR309Event();
	virtual ~JSR309Event();

	void SetSessionTag(const std::wstring & tag) {sessionTag = tag;};
	
	void FillEvent(const JSR309EventContext & evt);
	
protected:
	int 					sessionId;
	std::wstring			sessionTag;
};





#endif	/* JSR309MANAGER_H */

