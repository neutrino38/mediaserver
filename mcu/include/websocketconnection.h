#ifndef _WebSocketConnection_H_
#define _WebSocketConnection_H_
#include <pthread.h>
#include <sys/poll.h>
#include <memory>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <list>
#include <map>
#include <openssl/rand.h>
#include "config.h"
#include "fifo.h"
#include "websockets.h"
#include "websockettransport.h"
#include "http.h"
#include "httpparser.h"


class WebSocketFrameHeader
{
public:
	enum OpCode
	{
		ContinuationFrame	= 0x0,
		TextFrame		= 0x1,
		BinaryFrame		= 0x2,
		Reserved3		= 0x3,
		Reserved4		= 0x4,
		Reserved5		= 0x5,
		Reserved6		= 0x6,
		Reserved7		= 0x7,
		Close			= 0x8,
		Ping			= 0x9,
		Pong			= 0xA,
		ReservedB		= 0xB,
		ReservedC		= 0xC,
		ReservedD		= 0xD,
		ReservedE		= 0xE,
		ReservedF		= 0xF,
	};

	class Parser
	{
		public:
			Parser()
			{
				header = NULL;
				len = 0;
			}

			~Parser()
			{
				if (header) delete(header);
			}

			int Parse(BYTE* data,DWORD size)
			{
				//IF still have a parsed header
				if (header==NULL && IsParsed())
					//Do nothing
					return 0;
				//Nothing yet
				DWORD pos = 0;
				//If no header
				if (header==NULL)
				{
					//Create new
					header = new WebSocketFrameHeader();
					//No length
					len = 0;
				}
				//We need first two bytes to know size
				if (len<2)
				{
					//Get missing
					BYTE c = 2-len;
					//IF we don't have enought
					if (size<c)
						//Only what's on it
						c = size;
					//Copy next
					memcpy(header->data+len,data,c);
					//Increase len
					len += c;
					//Remove size
					size-= c;
					//Increase position
					pos += c;
				}
				//Check if we still have data and header
				if (size)
				{
					//Get missing for complete header
					BYTE c = header->GetSize()-len;
					//IF we don't have enought
					if (size<c)
						//Only what's on it
						c = size;
					//Copy next
					memcpy(header->data+len,data+pos,c);
					//Increase len
					len += c;
					//Remove size
					size-= c;
					//Increase position
					pos += c;
				}
				//Return consumed
				return pos;
			}

			bool IsParsed()
			{
				return header ? header->GetSize()==len : false;
			}

			WebSocketFrameHeader* ConsumeHeader()
			{
				//Get header
				WebSocketFrameHeader* ret = header;
				//Remvoe current header
				header = NULL;
				//Return parsed header
				return ret;
			}

		private:
			WebSocketFrameHeader* header;
			DWORD len;
	};

	friend class Parser;
public:
	WebSocketFrameHeader(bool fin,OpCode opCode,QWORD len,DWORD mask)
	{
		//Empty
		memset(data,0,14);
		//Set data
		SetFin(fin);
		SetOpCode(opCode);
		SetPayloadLength(len);
		if (mask) SetMask(mask);
	}
	/*
	      0                   1                   2                   3
	      0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
	     +-+-+-+-+-------+-+-------------+-------------------------------+
	     |F|R|R|R| opcode|M| Payload len |    Extended payload length    |
	     |I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
	     |N|V|V|V|       |S|             |   (if payload len==126/127)   |
	     | |1|2|3|       |K|             |                               |
	     +-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
	     |     Extended payload length continued, if payload len == 127  |
	     + - - - - - - - - - - - - - - - +-------------------------------+
	     |                               |Masking-key, if MASK set to 1  |
	     +-------------------------------+-------------------------------+
	     | Masking-key (continued)       |          Payload Data         |
	     +-------------------------------- - - - - - - - - - - - - - - - +
	     :                     Payload Data continued ...                :
	     + - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - +
	     |                     Payload Data continued ...                |
	     +---------------------------------------------------------------+
	 */
	bool	IsFin()		{ return data[0] & 0x80;		}
	OpCode	GetOpCode()	{ return (OpCode) (data[0] & 0x0F);	}
	bool	IsMasked()	{ return data[1] & 0x80;		}
	DWORD	GetMask()	{ return IsMasked()? get4(data,1+GetPayloadLenghtSize()) : 0;		}
	BYTE*	GetData()	{ return data;								}
	DWORD	GetSize()	{ return 1 + GetPayloadLenghtSize() + IsMasked()*4;			}
	QWORD	GetPayloadLength()
	{
		QWORD len = data[1] & 0x7F;
		if (len==126)
			return get2(data,2);
		else if (len==127)
			return get8(data,2);
		return len;
	}
private:
	WebSocketFrameHeader()
	{
		//Empty
		memset(data,0,14);
	}
	void	SetFin(bool fin)
	{
		data[0] = (data[0] & 0x7F) | (fin<<7);
	}
	void	SetMask(DWORD mask)
	{
		//Set mask bit
		data[1] = data[1] | 0x80;
		//Set mask key
		set4(data,1+GetPayloadLenghtSize(),mask);
	}
	void	SetOpCode(OpCode opCode)
	{
		data[0] = (data[0] & 0x80) | opCode;
	}
	void	SetPayloadLength(QWORD len)
	{
		if (len<126)
		{
			//1 byte & len
			data[1] |= len;
		}
		else if (len<0xFFFF)
		{
			//2 bytes
			data[1] |= 126;
			//Set lenght
			set2(data,2,len);
		} else {
			//8 bytes
			data[1] |= 127;
			//Set lenght
			set8(data,2,len);
		}

	}
	BYTE GetPayloadLenghtSize()
	{
		QWORD len = data[1] & 0x7F;
		if (len==126)
			return 3;
		else if (len==127)
			return 9;
		return 1;
	}
private:
	BYTE	data[14];
};


class WebSocketConnection :
	public WebSocket,
	public HTTPParser::Listener,
	public std::enable_shared_from_this<WebSocketConnection>
{
public:
	//Une trame prete a ecrire : en-tete puis payload.
	//RFC 6455 §5.3 : un client masque TOUTES ses trames sortantes avec une cle
	//tiree par trame, un serveur n'en masque aucune. L'offset du XOR compte
	//depuis le debut du payload de CETTE trame, donc un message decoupe en
	//fragments porte un masque par fragment, chacun repartant de zero.
	class Frame
	{
	public:
		Frame(bool fin,WebSocketFrameHeader::OpCode opCode,const BYTE* data,DWORD size,bool masked)
		{
			this->masked = masked;
			memset(mask,0,sizeof(mask));
			//Get masking key
			if (masked)
			{
				if (RAND_bytes(mask,sizeof(mask))!=1)
					Error("-WebSocketConnection::Frame could not get a random masking key\n");
				//Une cle nulle laisserait l'en-tete se declarer NON masque
				//(WebSocketFrameHeader ne pose le bit MASK que si mask!=0), et
				//le pair fermerait pour trame non masquee.
				if (!get4(mask,0))
					mask[0] = 1;
			}
			//Create header
			WebSocketFrameHeader header(fin,opCode,size,masked ? get4(mask,0) : 0);
			//Calculate total size
			this->size = size+header.GetSize();
			//Set values
			this->data = (BYTE*)calloc(this->size,1);
			//Copy header data
			memcpy(this->data,header.GetData(),header.GetSize());
			//Set initial length
			length = header.GetSize();
			//Remember where the payload starts (mask offset origin)
			headerSize = header.GetSize();
			//If we have payload
			if (data)
				//Append it
				Append(data,size);
		}

		bool Append(const BYTE* data,DWORD size)
		{
			//Check
			if (size+length>this->size)
				//Error
				return Error("-WebSocketConnection::Frame not enoguth length for appending data size:%d,length:%d,data:%d",this->size,length,size);
			//Check if it is masked
			if (masked)
			{
				//Position of the appended data inside the payload
				DWORD pos = length-headerSize;
				//For each byte
				for (DWORD i=0;i<size;++i)
					//XOR
					this->data[length+i] = data[i] ^ mask[(pos+i) & 0x03];
			} else {
				//Copy payload data
				memcpy(this->data+length,data,size);
			}
			//Set length
			length += size;
	return true;
		}

		~Frame()
		{
			free(data);
		}

		const BYTE* GetData()	{ return data;	}
		const DWORD GetSize()	{ return size;	}
	private:
		BYTE* data;
		DWORD size;
		DWORD length;
		DWORD headerSize;
		bool  masked;
		BYTE  mask[4];
	};
public:
	//Borne de sécurité sur la longueur déclarée d'une trame WS (protège le thread
	//serveur unique d'une allocation démesurée — R2 de websocket-refactor.md).
	static constexpr QWORD MaxFramePayload = 16*1024*1024; // 16 Mo

	//Le serveur (thread unique) est notifié via ce Listener. Il ne possède plus
	//de thread par-connexion : la connexion est une machine à état passive pilotée
	//par la boucle poll() unique du serveur.
	class Listener
	{
	public:
		//Virtual desctructor
		virtual ~Listener(){};
	public:
		//Interface
		virtual void onUpgradeRequest(WebSocketConnection* conn) = 0;
		//Réveille le thread serveur (ex. depuis un autre thread après SendMessage/
		//Close) pour qu'il reconstruise son jeu de poll() et traite la sortie.
		virtual void onWakeupNeeded() = 0;
	};
public:
	//Role de la connexion sur le fil. Il decide du masquage des trames
	//sortantes (RFC 6455 §5.3) et du refus des trames masquees entrantes
	//(§5.1) : un client masque tout et ne doit rien recevoir de masque.
	enum Role
	{
		Server = 0,
		Client = 1,
	};

	//Etapes de l'ouverture d'une connexion cliente (SPEC WS-CLIENT §4.1). Une
	//connexion serveur reste a NotAClient.
	enum ClientState
	{
		NotAClient	= 0,
		Connecting	= 1,	//connect() non bloquant en cours
		Upgrading	= 2,	//requete GET emise, on attend la reponse
		Opened		= 3,	//101 verifie
		Failed		= 4,	//echec avant l'ouverture
	};
public:
	//Clé d'acceptation RFC 6455 §1.3 : base64(SHA1(clé + GUID)). Le serveur la
	//pose dans sa réponse 101, le client compare la sienne à celle reçue.
	static std::string ComputeAcceptKey(const std::string& secWebSocketKey);

	WebSocketConnection(Listener* listener, uint64_t connId);
	~WebSocketConnection();

	//Le serveur fournit le transport (clair ou TLS) déjà choisi.
	int Init(int fd, std::unique_ptr<WebSocketTransport> transport, Role role = Server);

	//Mode client : le socket est DEJA en cours de connexion (connect() non
	//bloquant lance par l'appelant, cf. §4.5) ; la requete d'upgrade part au
	//premier POLLOUT, une fois le transport pret. `host` est la valeur de
	//l'en-tete Host (hote[:port]), `path` le chemin avec sa query.
	//Le listener est fourni ICI : en mode client personne n'appelle Accept(),
	//et un echec avant le 101 doit deja pouvoir se dire.
	int InitClient(int fd, std::unique_ptr<WebSocketTransport> transport,
		       const std::string& host, const std::string& path,
		       std::weak_ptr<WebSocket::Listener> wsl);

	Role GetRole() const		{ return role;		}
	bool IsClient() const		{ return role==Client;	}
	ClientState GetClientState() const { return clientState;	}
	int End();

	//Weksocket (appelables depuis n'importe quel thread — thread-safe)
	virtual void Accept(std::weak_ptr<WebSocket::Listener> wsl);
	virtual void Reject(const WORD code, const char* reason);
	virtual void SendMessage(const std::string& message);
	virtual void SendMessage(const BYTE* data, const DWORD size);
	virtual void Close();
	virtual std::weak_ptr<WebSocket> GetWeakPtr();

	//HTTPParser listener
	virtual int on_url (HTTPParser*, const char *at, DWORD length);
	virtual int on_header_field (HTTPParser* parser, const char *at, DWORD length);
	virtual int on_header_value (HTTPParser*, const char *at, DWORD length);
	virtual int on_body (HTTPParser*, const char *at, DWORD length);
	virtual int on_message_begin (HTTPParser*);
	virtual int on_status_complete (HTTPParser*);
	virtual int on_headers_complete (HTTPParser* parser);
	virtual int on_message_complete (HTTPParser*);

	HTTPRequest* GetRequest() { return request; };

private:
	//Reassemblage des chaines que le parseur HTTP rend par morceaux
	void FlushPendingHeader();
	void EnsureRequest(HTTPParser* parser);
	//Ouverture cliente
	bool SendUpgradeRequest();
	bool CheckUpgradeResponse(HTTPParser* parser);
	void FailClient(const char* reason);
	std::string GetResponseHeader(const char* name) const;
public:

	//---- Interface pilotée par le thread serveur (boucle poll() unique) --------
	uint64_t GetConnId() const	{ return connId;		}
	int      GetFd()		{ return transport ? transport->GetFd() : FD_INVALID; }
	short    GetPollEvents();	//Événements poll() souhaités (POLLIN + POLLOUT si sortie en attente)
	void     OnReadable();		//Données entrantes disponibles
	void     OnWritable();		//Socket prêt en écriture
	bool     IsFinished();		//La connexion doit-elle être fermée/détruite ?
	void     NotifyClose();		//Émet onClose vers le WebSocket::Listener (si upgraded)

private:
	Frame* GetNextFrame();
	void   ProcessData(BYTE *data,DWORD size);
	bool   HasPendingOutput();	//appelant DOIT tenir framesMutex

private:
	//Le serveur possède la connexion et lui survit → pointeur brut.
	Listener* listener;
	uint64_t  connId;
	Role      role;

	//Etat de l'ouverture cliente, et ce qu'elle a besoin de retenir : la cible
	//(pour l'en-tete Host et la ligne de requete) et la cle tiree au sort, que
	//le Sec-WebSocket-Accept recu doit confirmer.
	ClientState clientState;
	std::string clientHost;
	std::string clientPath;
	std::string secWebSocketKey;
	//En-tetes de la reponse, clefs en MINUSCULES : la casse d'un en-tete HTTP
	//est libre, et le pair n'est plus notre serveur. Ils ne peuvent pas se poser
	//sur `request` (il n'y en a pas pour une reponse) ni sur `response` (qui est
	//la sortie du mode serveur).
	std::map<std::string,std::string> responseHeaders;
	//onError n'est emis qu'une fois, quel que soit le chemin d'echec
	bool errorNotified;

	std::unique_ptr<WebSocketTransport> transport;

	//framesMutex protège UNIQUEMENT la file `frames` (seul état touché hors thread
	//serveur, par SendMessage). Tout le reste (parser, request/response, header…)
	//est exclusivement manipulé par le thread serveur.
	std::mutex		framesMutex;
	std::list<Frame*>	frames;

	//Fermeture demandée depuis un autre thread (Close) ou le pair (trame Close).
	std::atomic<bool>	closeRequested;
	//Fermeture différée : fermer une fois toute la sortie écoulée (Reject).
	bool			closeAfterFlush;

	bool inited;

	timeval startTime;

	bool upgraded;
	DWORD recvSize;
	DWORD inBytes;
	DWORD outBytes;

	HTTPParser parser;
	HTTPRequest* request;
	HTTPResponse* response;
	//URL et en-tetes sont ACCUMULES : le parseur HTTP rend ses chaines par
	//morceaux, autant de fois que TCP a decoupe la requete (et un client peut
	//decouper a l'octet). Cf. tests/test_websocket_http_hardening.cpp.
	std::string requestUrl;
	std::string headerField;
	std::string headerValue;
	//Vrai entre le debut d'une valeur d'en-tete et le champ suivant : c'est ce
	//qui dit quand le couple (champ, valeur) est complet.
	bool parsingHeaderValue = false;

	std::weak_ptr<WebSocket::Listener> wsl;
	WebSocketFrameHeader::Parser headerParser;
	WebSocketFrameHeader* header;
	QWORD framePos;

	fifo<BYTE,65538> incoming;
	WORD		 incomingFrameLength;

	Frame*		   pong;
};

#endif
