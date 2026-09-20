#include <unistd.h>
#include <sys/socket.h>
 #include <math.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <string>
#include <strings.h>
#include <ctype.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
extern "C" {
#include <libavutil/base64.h>
}
#include "http.h"
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <list>
#include "log.h"
#include "tools.h"
#include "websocketconnection.h"

WebSocketConnection::WebSocketConnection(Listener* listener, uint64_t connId)
{
	//Store listener and identity
	this->listener = listener;
	this->connId   = connId;
	//Serveur tant qu'Init n'a pas dit le contraire
	this->role     = Server;
	this->clientState = NotAClient;
	this->errorNotified = false;

	//Not inited
	inited = false;
	closeRequested = false;
	closeAfterFlush = false;

	//No pong
	pong = NULL;
	//Not uypgraded yet
	upgraded = false;
	//No incoming frame yet
	incomingFrameLength = 0;
	//No request or response
	request = NULL;
	response = NULL;
	header = NULL;

	recvSize = 0;
	inBytes = 0;
	outBytes = 0;
	framePos = 0;

	//Set initial time
	gettimeofday(&startTime,0);
}

WebSocketConnection::~WebSocketConnection()
{
	//End the socket before destruction
	End();

	//Remove pending frames
	while (!frames.empty())
	{
		//Delete first frame from list
		delete(frames.front());
		//Remove from queue
		frames.pop_front();
	}
	if (request)  delete(request);
	if (response) delete(response);
	if (header)   delete(header);
	//Check unsent pong
	if (pong)     delete(pong);
}

int WebSocketConnection::Init(int fd, std::unique_ptr<WebSocketTransport> transport, Role role)
{
	Log(">WebSocket Connection init [fd:%d,id:%llu,client:%d]\n",fd,(unsigned long long)connId,role==Client);

	//Store role
	this->role = role;

	//Le serveur fournit le transport (clair ou TLS) ; on l'initialise sur le socket
	this->transport = std::move(transport);
	this->transport->Init(fd);

	//I am inited
	inited = true;

	//Start parser : un client lit une REPONSE, un serveur une requete
	parser.Init(this,role==Client ? HTTPParser::HTTP_RESPONSE : HTTPParser::HTTP_REQUEST);

	Log("<WebSocket Connection init\n");

	return 1;
}

int WebSocketConnection::InitClient(int fd, std::unique_ptr<WebSocketTransport> transport,
				    const std::string& host, const std::string& path,
				    std::weak_ptr<WebSocket::Listener> wsl)
{
	//Poser le listener AVANT d'initialiser : un echec de connexion doit se dire
	//meme s'il survient avant le 101 (§4.6).
	this->wsl	  = wsl;
	this->clientHost  = host;
	//Une requete HTTP porte toujours un chemin absolu
	this->clientPath  = path.empty() ? "/" : path;
	//Le socket est en cours de connexion : on attend POLLOUT
	this->clientState = Connecting;

	return Init(fd,std::move(transport),Client);
}

void WebSocketConnection::Close()
{
	//Demande de fermeture (peut venir d'un autre thread). Le thread serveur la
	//traitera à son prochain tour de boucle.
	closeRequested = true;
	//Réveiller le serveur pour qu'il traite la fermeture rapidement
	if (listener)
		listener->onWakeupNeeded();
}

std::weak_ptr<WebSocket> WebSocketConnection::GetWeakPtr()
{
	//La connexion est possédée par un shared_ptr côté serveur (make_shared) : on
	//peut donc en dériver un weak_ptr. Un consommateur externe (WSEndpoint, thread
	//RTP) le verrouille avant chaque appel → pas d'usage-après-libération.
	return shared_from_this();
}

int WebSocketConnection::End()
{
	if (!inited)
		//Exit
		return 0;

	Log(">End WebSocket connection [id:%llu]\n",(unsigned long long)connId);

	//Not inited any more
	inited = false;

	//Ferme le socket (shutdown + close)
	if (transport)
		transport->Shutdown();

	Log("<End WebSocket connection\n");

	return 1;
}

/***********************************************************************
 * Interface pilotée par le thread serveur (boucle poll() unique)
 ***********************************************************************/
bool WebSocketConnection::HasPendingOutput()
{
	//ATTENTION : l'appelant DOIT tenir framesMutex.
	return response != NULL || !frames.empty() || (transport && transport->WantsWrite());
}

short WebSocketConnection::GetPollEvents()
{
	short ev = POLLIN | POLLERR | POLLHUP;
	//Une connexion cliente attend POLLOUT tant qu'elle n'a pas emis sa requete :
	//c'est lui qui dit que le connect() non bloquant est termine.
	if (clientState==Connecting)
		ev |= POLLOUT;
	std::lock_guard<std::mutex> lock(framesMutex);
	if (HasPendingOutput())
		ev |= POLLOUT;
	return ev;
}

void WebSocketConnection::OnReadable()
{
	BYTE data[MTU];

	//Draine tout ce qui est lisible (le TLS peut avoir plusieurs enregistrements
	//déjà bufferisés au-delà d'une seule lecture socket).
	while (true)
	{
		//Contrat Recv : >0 octets ; 0 rien pour l'instant (would-block/handshake) ;
		//<0 connexion fermée / erreur.
		int len = transport->Recv(data,MTU);
		if (len < 0)
		{
			closeRequested = true;
			return;
		}
		if (len == 0)
			//Plus rien de disponible pour l'instant
			return;

		//Increase in bytes
		inBytes += len;

		try {
			//Parse data
			ProcessData(data,len);
		} catch (std::exception &e) {
			//Show error
			Error("Exception parsing data: %s\n",e.what());
			//Dump it
			Dump(data,len);
			//Close on any error
			closeRequested = true;
			return;
		}

		//La trame de contrôle a pu demander la fermeture (Close)
		if (closeRequested.load())
			return;
	}
}

void WebSocketConnection::OnWritable()
{
	//Pousser d'abord les octets (chiffrés) en attente côté transport (TLS)
	if (transport->Flush() < 0)
	{
		closeRequested = true;
		return;
	}

	//Ouverture cliente : POLLOUT dit seulement que le connect() non bloquant est
	//termine, SO_ERROR dit s'il a REUSSI. Tant que la requete d'upgrade n'est
	//pas partie, il n'y a rien d'autre a ecrire.
	if (clientState==Connecting)
	{
		int err = 0;
		socklen_t errlen = sizeof(err);
		if (getsockopt(GetFd(),SOL_SOCKET,SO_ERROR,&err,&errlen)<0 || err)
		{
			char msg[128];
			snprintf(msg,sizeof(msg),"connect failed (errno %d)",err);
			FailClient(msg);
			return;
		}
		//Le transport peut n'etre pas encore pret (handshake TLS en cours) :
		//`Send` y JETTE ce qu'on lui donne, donc la requete serait perdue en
		//silence (§4.4). On reessaiera au prochain tour.
		if (!transport->IsReady())
			return;
		//Emettre la poignee de main
		SendUpgradeRequest();
		return;
	}

	//S'il reste des octets à écouler (socket plein), attendre le prochain POLLOUT
	if (transport->WantsWrite())
		return;

	//Check if we have http response
	if (response)
	{
		//Serialize
		std::string out = response->Serialize();
		//Send it
		outBytes += transport->Send((BYTE*)out.c_str(),out.length());
		//Delete it
		delete(response);
		//Nullify
		response = NULL;
		return;
	}

	//Get next frame to send
	Frame* frame = GetNextFrame();
	//Check length
	if (frame)
	{
		//Send it
		outBytes += transport->Send(frame->GetData(),frame->GetSize());
		//Delete it
		delete(frame);
	}
}

bool WebSocketConnection::IsFinished()
{
	//Fermeture dure demandée (Close externe, trame Close du pair, erreur)
	if (closeRequested.load())
		return true;
	//Fermeture différée (Reject) : attendre que toute la sortie soit écoulée
	if (closeAfterFlush)
	{
		std::lock_guard<std::mutex> lock(framesMutex);
		if (!HasPendingOutput())
			return true;
	}
	return false;
}

void WebSocketConnection::NotifyClose()
{
	//Une connexion cliente notifie TOUJOURS, upgradee ou non : un echec avant le
	//101 (refus TCP, DNS mort, 404, certificat) laisserait sinon le WSEndpoint
	//attendre indefiniment un pair qui ne viendra jamais (§4.6).
	if (!upgraded && !IsClient())
		return;

	std::shared_ptr<WebSocket::Listener> wsl2 = this->wsl.lock();
	if (!wsl2)
		return;

	//Un client jamais ouvert a echoue, quelle que soit la voie par laquelle le
	//reacteur l'a constate (POLLERR du connect, pair qui raccroche avant sa
	//reponse) : onError precede toujours onClose.
	if (IsClient() && !upgraded && !errorNotified)
	{
		errorNotified = true;
		wsl2->onError(this);
	}

	wsl2->onClose(this);
}

WebSocketConnection::Frame* WebSocketConnection::GetNextFrame()
{
	//Lock frames
	std::lock_guard<std::mutex> lock(framesMutex);

	//if there are frames waiting
	if (frames.empty())
		return NULL;

	//Write next chunk from this stream
	Frame* frame = frames.front();
	//Remove it
	frames.pop_front();

	//Return frame
	return frame;
}

/***********************
 * ProcessData
 * 	Process incomming data
 **********************/
void WebSocketConnection::ProcessData(BYTE *data,DWORD size)
{
	//And total size
	recvSize += size;

	if (!upgraded)
	{
		//Parse request (serveur) ou reponse (client)
		DWORD parsed = parser.Execute((char*)data,size);
		//Tant que l'ouverture n'est pas faite, rien d'autre a lire
		if (!upgraded)
			return;
		//Le parseur HTTP s'arrete a la fin des en-tetes et rend ce qu'il a
		//consomme : ce qui suit dans le MEME segment TCP est deja une trame
		//WebSocket. Un serveur peut coller sa premiere trame T.140 a sa reponse
		//101 ; ignorer ce reliquat, c'est perdre la premiere phrase du pair.
		if (parsed>=size)
			return;
		data += parsed;
		size -= parsed;
	}

	{
		std::shared_ptr<WebSocket::Listener> wsl2 = this->wsl.lock();
		//Process all input
		while(size)
		{
			//If we still don't have header
			if (!header)
			{
				//Parse
				DWORD len = headerParser.Parse(data,size);
				//Reduce size
				size-=len;
				data+=len;
				//Check if is header parsed
				if (headerParser.IsParsed())
				{
					//Clean data sent
					framePos = 0;
					//Get new header
					header = headerParser.ConsumeHeader();

					//R2 : borne de sécurité sur la longueur déclarée (protège le
					//thread serveur unique d'une allocation démesurée / OOM).
					if (header->GetPayloadLength() > MaxFramePayload)
					{
						Error("-WebSocketConnection: frame payload too large "
						      "(%llu > %llu), closing [id:%llu]\n",
						      (unsigned long long)header->GetPayloadLength(),
						      (unsigned long long)MaxFramePayload,
						      (unsigned long long)connId);
						closeRequested = true;
						return;
					}

					//RFC 6455 §5.1 : un serveur ne masque jamais, donc un
					//client qui recoit une trame masquee DOIT fermer.
					if (IsClient() && header->IsMasked())
					{
						Error("-WebSocketConnection: masked frame received from server, closing [id:%llu]\n",
						      (unsigned long long)connId);
						closeRequested = true;
						return;
					}

					//Check type
					switch(header->GetOpCode())
					{
						case WebSocketFrameHeader::ContinuationFrame:
							//Do nothing
							break;
						case WebSocketFrameHeader::TextFrame:
							//Start frame
							if (wsl2) wsl2->onMessageStart(this,WebSocket::Text,header->GetPayloadLength());
							break;
						case WebSocketFrameHeader::Close:
							//Log
							Log("-Received close request\n");
							//Demander la fermeture (traitée par le thread serveur)
							closeRequested = true;
							return;
						case WebSocketFrameHeader::BinaryFrame:
							//Start frame
							if (wsl2) wsl2->onMessageStart(this,WebSocket::Binary,header->GetPayloadLength());
							break;

						case WebSocketFrameHeader::Ping:
							//Debug
							Debug("-Received ping\n");
							//Create new pong frame
							pong = new Frame(true,WebSocketFrameHeader::Pong,NULL,header->GetPayloadLength(),IsClient());
							break;
						case WebSocketFrameHeader::Pong:
							break;
						default:
							break;
					}
				}
			} else {
				//Get missing
				QWORD len = header->GetPayloadLength()-framePos;
				//Check how much data do we have readed
				if (len>size)
					//Limit
					len = size;
				//Check if it is masked
				if (header->IsMasked())
				{
					BYTE mask[4];
					//Get mask
					set4(mask,0,header->GetMask());
					//For each byte
					for (QWORD i=0;i<len;++i)
						//XOR
						data[i] = data[i] ^ mask[(framePos+i) & 0x03];
				}
				//Check type
				switch(header->GetOpCode())
				{
					case WebSocketFrameHeader::ContinuationFrame:
					case WebSocketFrameHeader::TextFrame:
					case WebSocketFrameHeader::BinaryFrame:
						//Send data
						if (wsl2) wsl2->onMessageData(this,data,len);
						break;

					case WebSocketFrameHeader::Ping:
						//data here to the PONG
						pong->Append(data,len);
						break;
					default:
						break;
				}
				//Move pos
				framePos +=len;
				//Reduce size
				size-=len;
				data+=len;
				//Check if we have ended with the frame
				if (framePos==header->GetPayloadLength())
				{
					//Check type
					switch(header->GetOpCode())
					{
						case WebSocketFrameHeader::ContinuationFrame:
						case WebSocketFrameHeader::TextFrame:
						case WebSocketFrameHeader::BinaryFrame:
							//Check if it is end frame for message
							if (header->IsFin())
								//Send data
								if (wsl2) wsl2->onMessageEnd(this);
							break;
						case WebSocketFrameHeader::Ping:
							//Debug
							Debug("-Sending pong\n");
							{
								//Lock frames
								std::lock_guard<std::mutex> lock(framesMutex);
								//Push pong frame
								frames.push_back(pong);
								//NO pong to send
								pong = NULL;
							}
							//We need to write data! → réveiller le serveur
							if (listener) listener->onWakeupNeeded();
							break;
						default:
							break;
					}
					//Delete header
					delete(header);
					//Parse new header
					header = NULL;
				}
			}
		}
	}
}

void  WebSocketConnection::SendMessage(const std::string& message)
{
	//Create new frame
	Frame *frame = new Frame(true,WebSocketFrameHeader::TextFrame,(BYTE*)message.c_str(),message.length(),IsClient());

	{
		//Lock frames
		std::lock_guard<std::mutex> lock(framesMutex);
		//Push frame
		frames.push_back(frame);
	}

	//We need to write data! → réveiller le thread serveur (peut être un autre thread)
	if (listener) listener->onWakeupNeeded();
}

void WebSocketConnection::SendMessage(const BYTE* data, const DWORD size)
{
	//Do not send empty frames
	if (!size)
		return;

	//Not last frame
	bool last = false;

	//Binary type
	WebSocketFrameHeader::OpCode code  = WebSocketFrameHeader::BinaryFrame;

	//Sent length
	DWORD pos = 0;

	{
		//Lock frames
		std::lock_guard<std::mutex> lock(framesMutex);

		//Send 1300 byte frames
		while (!last)
		{
			//Get remaining frame size
			DWORD len = size-pos;

			//Check if bigger than desired frame length
			if (len>1300)
				//Set new length
				len = 1300;

			//Check if it is last
			last = (len+pos==size);

			//Create new frame
			Frame *frame = new Frame(last,code,data+pos,len,IsClient());

			//Push frame
			frames.push_back(frame);

			//Next is always a continuation frame
			code = WebSocketFrameHeader::ContinuationFrame;

			//Move pos
			pos += len;
		}
	}

	//We need to write data! → réveiller le thread serveur
	if (listener) listener->onWakeupNeeded();
}

int WebSocketConnection::on_url (HTTPParser* parser, const char *at, DWORD length)
{
	//ACCUMULER, et non remplacer : le parseur rend l'URL par morceaux (un par
	//segment TCP). L'ancien code creait une requete neuve a chaque morceau, donc
	//abandonnait la precedente et ne gardait que le dernier fragment d'URL.
	requestUrl.append(at,length);
	//OK
	return 0;
}

//Ferme le couple (champ, valeur) en cours et le pose sur la requete.
void WebSocketConnection::FlushPendingHeader()
{
	//Rien en cours
	if (!parsingHeaderValue)
		return;
	if (IsClient())
	{
		//Une reponse n'a pas de HTTPRequest ou se poser. Clef normalisee en
		//minuscules : la casse d'un en-tete HTTP est libre.
		if (!headerField.empty())
		{
			std::string key = headerField;
			for (size_t i=0;i<key.size();++i)
				key[i] = tolower(key[i]);
			responseHeaders[key] = headerValue;
		}
	}
	//La requete existe forcement ici (creee au premier champ d'en-tete)
	else if (request && !headerField.empty())
		request->AddHeader(headerField,headerValue);
	//Pret pour le suivant
	headerField.clear();
	headerValue.clear();
	parsingHeaderValue = false;
}

//Cree la requete des que l'URL est complete : le parseur termine l'URL avant
//d'annoncer le premier champ d'en-tete.
void WebSocketConnection::EnsureRequest(HTTPParser* parser)
{
	//Deja creee
	if (request)
		return;
	//Get method
	std::string method(parser->GetMethodStr());
	//Create request
	request = new HTTPRequest(method,requestUrl,parser->GetHttpMajor(),parser->GetHttpMinor());
}

int WebSocketConnection::on_header_field (HTTPParser* parser, const char *at, DWORD length)
{
	//L'URL est complete des le premier champ d'en-tete. Une reponse n'a pas
	//d'URL ni de methode : EnsureRequest y construirait une requete absurde.
	if (!IsClient())
		EnsureRequest(parser);
	//Un nouveau champ ferme le couple precedent
	FlushPendingHeader();
	//Get field (par morceaux, comme l'URL)
	headerField.append(at,length);
	//OK
	return 0;
}

int WebSocketConnection::on_header_value (HTTPParser*, const char *at, DWORD length)
{
	//double check (une reponse n'a pas de requete : cf. FlushPendingHeader)
	if (!request && !IsClient())
		//Error
		return 1;
	//Get value (par morceaux : c'est ainsi qu'une cle Sec-WebSocket-Key coupee
	//en deux se retrouvait tronquee, et la reponse d'acceptation fausse)
	headerValue.append(at,length);
	//Une valeur est en cours
	parsingHeaderValue = true;
	//OK
	return 0;
}
int WebSocketConnection::on_body (HTTPParser*, const char *at, DWORD length)
{
	//Ignore body
	return 0;
}
int WebSocketConnection::on_message_begin (HTTPParser*)
{
	//OK
	return 0;
}
int WebSocketConnection::on_status_complete (HTTPParser*)
{
	return 0;
}
int WebSocketConnection::on_headers_complete (HTTPParser* parser)
{
	if (IsClient())
	{
		//Poser le dernier couple (champ, valeur)
		FlushPendingHeader();
		//Juger la reponse DES la fin des en-tetes : un 101 n'a pas de corps, et
		//un echec ne doit pas faire attendre le sien.
		CheckUpgradeResponse(parser);
		//1 : il n'y a pas de corps a attendre
		return 1;
	}
	//Une requete sans le moindre en-tete n'est jamais passee par on_header_field
	EnsureRequest(parser);
	//Poser le dernier couple (champ, valeur)
	FlushPendingHeader();
	return 0;
}
int WebSocketConnection::on_message_complete (HTTPParser*)
{
	//Mode client : la reponse a deja ete jugee a la fin des en-tetes
	if (IsClient())
		return 0;

	//Une requete que le parseur n'a pas menee jusqu'a son URL ne donne rien a
	//traiter : ne pas la dereferencer pour le seul plaisir de la tracer.
	if (!request)
		//Error
		return 1;

	//Debug
	Log("-Incoming websocket connection for url:%s\n",request->GetRequestURI().c_str());

	//Check listener
	if (listener)
		//Send event
		listener->onUpgradeRequest(this);
	return 0;
}

std::string WebSocketConnection::ComputeAcceptKey(const std::string& secWebSocketKey)
{
	std::string concat = secWebSocketKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

	BYTE digest[SHA_DIGEST_LENGTH];
	char base64[SHA_DIGEST_LENGTH*2];

	//API EVP one-shot (remplace SHA1() deprecie en OpenSSL 3.0)
	size_t digestLen = 0;
	EVP_Q_digest(NULL, "SHA1", NULL,
		     (unsigned char*)concat.c_str(), concat.length(),
		     digest, &digestLen);

	//Calculate base 64
	av_base64_encode(base64,SHA_DIGEST_LENGTH*2,digest,SHA_DIGEST_LENGTH);

	return std::string(base64);
}

std::string WebSocketConnection::GetResponseHeader(const char* name) const
{
	//Les clefs ont ete normalisees en minuscules a la reception
	std::map<std::string,std::string>::const_iterator it = responseHeaders.find(name);
	return it==responseHeaders.end() ? std::string() : it->second;
}

void WebSocketConnection::FailClient(const char* reason)
{
	//Journaliser la cause : sans cela, un echec avant le 101 est muet (§4.6)
	Error("-WebSocketConnection: client connection failed: %s [id:%llu,host:%s,path:%s]\n",
	      reason,(unsigned long long)connId,clientHost.c_str(),clientPath.c_str());

	//Etat terminal
	clientState = Failed;

	//onError tout de suite ; onClose suivra par NotifyClose, quand le reacteur
	//retirera la connexion.
	if (!errorNotified)
	{
		errorNotified = true;
		std::shared_ptr<WebSocket::Listener> wsl2 = this->wsl.lock();
		if (wsl2) wsl2->onError(this);
	}

	//Fermer
	closeRequested = true;
}

bool WebSocketConnection::SendUpgradeRequest()
{
	//Cle de 16 octets aleatoires en base64 (RFC 6455 §4.1)
	BYTE nonce[16];
	char base64[AV_BASE64_SIZE(sizeof(nonce))];

	if (RAND_bytes(nonce,sizeof(nonce))!=1)
	{
		FailClient("could not get a random Sec-WebSocket-Key");
		return false;
	}
	av_base64_encode(base64,sizeof(base64),nonce,sizeof(nonce));
	//Retenue : le Sec-WebSocket-Accept de la reponse doit la confirmer
	secWebSocketKey = base64;

	HTTPRequest request("GET",clientPath,1,1);
	request.AddHeader("Host"			, clientHost);
	request.AddHeader("Upgrade"			, "websocket");
	request.AddHeader("Connection"			, "Upgrade");
	request.AddHeader("Sec-WebSocket-Key"		, secWebSocketKey);
	request.AddHeader("Sec-WebSocket-Version"	, "13");

	const std::string out = request.Serialize();

	int n = transport->Send((BYTE*)out.c_str(),out.length());
	if (n<0)
	{
		FailClient("could not send the upgrade request");
		return false;
	}
	outBytes += n;

	//On attend la reponse
	clientState = Upgrading;

	Log("-WebSocket client upgrade sent [id:%llu,host:%s,path:%s]\n",
	    (unsigned long long)connId,clientHost.c_str(),clientPath.c_str());

	return true;
}

bool WebSocketConnection::CheckUpgradeResponse(HTTPParser* parser)
{
	char msg[160];

	//RFC 6455 §4.1 : tout autre code est un echec propre, et le code doit se
	//lire dans le journal — c'est la seule trace qu'aura l'exploitant.
	const unsigned short code = parser->GetStatusCode();
	if (code!=101)
	{
		snprintf(msg,sizeof(msg),"server answered %u instead of 101",code);
		FailClient(msg);
		return false;
	}

	if (strcasecmp(GetResponseHeader("upgrade").c_str(),"websocket")!=0)
	{
		snprintf(msg,sizeof(msg),"bad Upgrade header \"%s\"",GetResponseHeader("upgrade").c_str());
		FailClient(msg);
		return false;
	}

	//Connection peut porter plusieurs jetons (ex. "keep-alive, Upgrade")
	std::string connection = GetResponseHeader("connection");
	for (size_t i=0;i<connection.size();++i)
		connection[i] = tolower(connection[i]);
	if (connection.find("upgrade")==std::string::npos)
	{
		snprintf(msg,sizeof(msg),"bad Connection header \"%s\"",connection.c_str());
		FailClient(msg);
		return false;
	}

	//La cle d'acceptation prouve que le pair parle bien WebSocket et qu'il
	//repond a NOTRE requete
	if (GetResponseHeader("sec-websocket-accept")!=ComputeAcceptKey(secWebSocketKey))
	{
		FailClient("bad Sec-WebSocket-Accept");
		return false;
	}

	//We are upgraded
	upgraded = true;
	clientState = Opened;

	Log("-WebSocket client connected [id:%llu,host:%s,path:%s]\n",
	    (unsigned long long)connId,clientHost.c_str(),clientPath.c_str());

	//We are opened
	std::shared_ptr<WebSocket::Listener> wsl2 = this->wsl.lock();
	if (wsl2) wsl2->onOpen(this);

	return true;
}

void WebSocketConnection::Accept(std::weak_ptr<WebSocket::Listener> wsl)
{
	//Une connexion cliente n'a rien a accepter : son listener est pose par
	//InitClient, et c'est elle qui demande l'upgrade.
	if (IsClient())
	{
		Error("-WebSocketConnection::Accept called on a client connection [id:%llu]\n",
		      (unsigned long long)connId);
		return;
	}

	//Store websocket listener
	this->wsl = wsl;

	/*
	If the response lacks a |Sec-WebSocket-Accept| header field or
	the |Sec-WebSocket-Accept| contains a value other than the
	base64-encoded SHA-1 of the concatenation of the |Sec-WebSocket-
	Key| (as a string, not base64-decoded) with the string "258EAFA5-
	E914-47DA-95CA-C5AB0DC85B11" but ignoring any leading and
	trailing whitespace, the client MUST _Fail the WebSocket
	Connection_.
	 */
	//Get Sec-WebSocket-Key header value
	std::string secWebSocketKey = request->GetHeader("Sec-WebSocket-Key");

	//If not found
	if (secWebSocketKey.size()==0)
	{
		//Update
		response = new HTTPResponse(400,"Bad request, no Sec-WebSocket-Key",1,1);
		//Fermer une fois la réponse écoulée
		closeAfterFlush = true;
		//Réveiller le serveur pour émettre la réponse
		if (listener) listener->onWakeupNeeded();
		return;
	}
	//Get accept key
	std::string secWebSocketAccept64 = ComputeAcceptKey(secWebSocketKey);

	//Update
	response = new HTTPResponse(101,"Switching Protocols",1,1);
	//Add headers
	response->AddHeader("Upgrade"			, "Websocket");
	response->AddHeader("Connection"		, "Upgrade");
	//Check if we have input protocols
	if (request->HasHeader("Sec-WebSocket-Protocol"))
		//Add websockets protocols back
		response->AddHeader("Sec-WebSocket-Protocol"	, request->GetHeader("Sec-WebSocket-Protocol"));
	//Add accept key
	response->AddHeader("Sec-WebSocket-Accept"	, secWebSocketAccept64);

	//We are upgraded
	upgraded = true;

	//Réveiller le serveur pour émettre la réponse 101
	if (listener) listener->onWakeupNeeded();

	//We are opened
	std::shared_ptr<WebSocket::Listener> wsl2 = wsl.lock();
	if (wsl2) wsl2->onOpen(this);
}

void WebSocketConnection::Reject(const WORD code, const char* reason)
{
	//Update
	response = new HTTPResponse(code,reason,1,1);
	//Fermer une fois la réponse écoulée
	closeAfterFlush = true;
	//Réveiller le serveur pour émettre la réponse
	if (listener) listener->onWakeupNeeded();
}
