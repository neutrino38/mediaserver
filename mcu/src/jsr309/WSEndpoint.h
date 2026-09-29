#ifndef WSENDPOINT_H
#define	WSENDPOINT_H

#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include "RTPMultiplexer.h"
#include "Joinable.h"
#include "websocketserver.h"
#include "websockets.h"
#include "Endpoint.h"
#include "medkit/codecs.h"
#include "text.h"
#include "redcodec.h"

class WSEndpoint :
	public Endpoint::Port,
	public Joinable::Listener,
	public WebSocket::Listener,
	public TextOutput,
	//Le serveur WebSocket ne garde qu'un weak_ptr sur son listener : c'est ce qui
	//borne la reprise d'une jambe cliente (une jambe detruite ne se reconnecte
	//pas). Il faut donc pouvoir se donner soi-meme en weak_ptr.
	public std::enable_shared_from_this<WSEndpoint>
{
public :
	WSEndpoint(MediaFrame::Type type);
	
	virtual ~WSEndpoint();

	// Port interface
	virtual int Init() { return 0; };
	virtual int End();

	//Joinable interface
	virtual void Update() {};
	virtual void SetREMB(DWORD estimation) {};

	//Joinable::Listener
	virtual void onRTPPacket(RTPPacket &packet);
	virtual void onResetStream() { SendReplacementChar(true); };
	virtual void onEndStream() { SendReplacementChar(true); };
	//Le lien retour Endpoint::Port::joined est désormais un weak_ptr : le
	//Port::Detach le lock() et ne déréférence jamais une source détruite. Plus
	//besoin de notification onJoinableEnded (C-13, lien A).

	//Websocket::Listener
	virtual void onOpen(WebSocket *ws);
	virtual void onMessageStart(WebSocket *ws,const WebSocket::MessageType type,const DWORD length) ;
	virtual void onMessageData(WebSocket *ws,const BYTE* data, const DWORD size);
	virtual void onMessageEnd(WebSocket *ws);
	virtual void onError(WebSocket *ws);
	virtual void onClose(WebSocket *ws);
		
	static void SetLocalPort(int port);
	static void SetLocalHost(char* host);
	//wss:// ou ws:// ? Réglé une fois au démarrage depuis --websocket-secure
	//(main.cpp). Le serveur est le SEUL à le savoir : le contrôleur SIP construit
	//l'URL qu'il publie dans son SDP depuis GetMediaCandidates, et ne peut pas
	//deviner si nous écoutons en TLS — nous écoutons sur le même port dans les
	//deux cas.
	static void SetLocalSecure(bool secure);
	static bool IsLocalSecure();

	//Statiques : ils ne lisent que la configuration globale du serveur WS (un
	//seul port/host pour tout le binaire, posés par main.cpp). L'API
	//conférence (S5, MultiConf::ConfigureParticipantMediaConnection) les
	//appelle sans instance ; l'appel via une instance (Endpoint::Port::
	//GetLocalMediaHost) reste valide.
	static int  	GetLocalPort();
	static char*  	GetLocalHost();

	//---- Mode client : le mediaserver joue le navigateur --------------------
	//SPEC docs/conception/WS-CLIENT, lot 5. La jambe ne PUBLIE plus une URL, elle
	//en CONSOMME une : elle se connecte a celle que le controleur lui donne.

	//Le serveur WebSocket du binaire, pose une fois au demarrage par main() —
	//meme regle que le port et l'hote d'ecoute ci-dessus. C'est lui qui ouvre les
	//connexions sortantes : ni le DNS ni le connect() n'ont lieu dans son
	//reacteur, et la jambe n'a pas de thread a elle.
	static void SetServer(WebSocketServer* server);

	//Arme la jambe en mode client et lance la premiere tentative. Rend 0 pour une
	//URL inutilisable — une faute du controleur ne se resout pas par la
	//repetition, donc elle se dit tout de suite et n'arme RIEN. Tout le reste est
	//asynchrone : l'ouverture se dit par EndpointConnectedEvent, la perte par
	//EndpointDisconnectedEvent, et la reprise repart toutes les 5 s sans limite
	//tant que la jambe vit (arbitrage du 2026-09-21).
	int Connect(const std::string& url);

	bool IsClientMode() const;
	//Copie, et non reference : l'URL est lue depuis le thread de controle et
	//depuis le reacteur, sous le meme verrou que le reste de l'etat client.
	std::string GetRemoteUrl() const;


	void SetUseRed(bool red){useRed = red;};
	void SetPrimaryPayloadType(BYTE pt){payloadType = pt;};
	
	virtual int SendFrame(TextFrame &frame);
	
private:
	WebSocket::MessageType msgType;
	MediaFrame * media;
	
	// to generate timestamp
	timeval clock;
	Joinable *joined;
	
	//weak_ptr : le WebSocket est possédé par le WebSocketServer. On le verrouille
	//avant chaque appel (SendMessage/Close) → sûr même si la connexion est détruite
	//de façon concurrente par le thread serveur.
	std::weak_ptr<WebSocket> _ws;
	bool		useRed;
	static int  	wsPort;
	static char* 	wsHost;
	static bool 	wsSecure;
	BYTE		payloadType;

	//Trames reçues du côté RTP avant que le navigateur n'ait ouvert son
	//WebSocket : entre le 200 OK et le handshake il s'écoule un aller-retour SDP,
	//et la première phrase est justement celle où l'appelant se présente. Bornée
	//dans les deux dimensions (§4.5 de jsr309_text_over_wss.md) : une file non
	//bornée sur un flux que personne ne viendra peut-être jamais lire est une
	//fuite.
	//L'horodatage est une date ABSOLUE (getTimeMS), pas un age relatif a `clock` :
	//celui-ci est remis a zero par la premiere association, ce qui rendait
	//perimee toute la file au moment meme de la rejouer.
	static const size_t maxPendingFrames = 32;
	static const QWORD  maxPendingAgeMs  = 5000;
	std::list<std::pair<QWORD,std::string>> pending;

	//---- Mode client ------------------------------------------------------
	//Trois threads touchent cet etat : celui du controle (Connect/End), celui du
	//reacteur WebSocket (onOpen/onClose) et celui du RTP (SendFrame). Le verrou
	//ne couvre QUE les trois champs qui suivent — `_ws` et `pending` gardent le
	//regime de la jambe serveur. Il n'est JAMAIS tenu pendant un appel au
	//serveur WebSocket : Connect() y resout un nom, et un DNS lent bloquerait
	//alors le reacteur.
	mutable std::mutex clientMutex;
	//URL distante, vide tant que la jambe est en mode serveur : elle dit A LA
	//FOIS le mode et la cible.
	std::string	clientUrl;
	//La jambe DOIT-elle etre connectee ? Faux apres End() : c'est ce qui arrete
	//la reprise, et rien d'autre ne l'arrete.
	bool		wantConnected;
	//Demande de reprise en cours cote serveur (0 = aucune)
	uint64_t	retryId;
	//Une connexion s'est deja ouverte : la file d'attente ne sert QU'AVANT la
	//premiere ouverture. Passe ce point, le texte d'une coupure est perdu, et la
	//perte s'annonce par un U+FFFD a la reconnexion (arbitrage du 2026-09-21).
	bool		everOpened;
	//Trames jetees depuis la derniere coupure, pour la trace
	unsigned	droppedWhileDown;
	//Rythme de la reprise : toutes les 5 s, sans limite.
	static const DWORD retryMs = 5000;
	static WebSocketServer* wsServer;

	void TryConnect();
	void ScheduleReconnect();

	RedundentCodec* RedCodec;
	WORD pseudoSeqNum; 
	WORD pseudoSeqCycle; 
	
	void SendReplacementChar(bool toWsSide);
	void PacketToWs(TextFrame & frame);	
};
#endif	/* WSENDPOINT_H */
