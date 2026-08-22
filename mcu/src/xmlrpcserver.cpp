#include "addressprofiles.h"
#include <errno.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "ipaddress.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <log.h>
#include <xmlrpc-c/abyss.h>
#include "xmlrpcserver.h"

#define ERRORMSG "MCU Error."
#define SHUTDOWNMSG "Shutting down."

/**************************************
* XmlRpcServer
*	Constructor
**************************************/
XmlRpcServer::XmlRpcServer(int port)
{
	//Iniciamos la fecha
	DateInit();

	//Los mime tipes
	MIMETypeInit();

	//Store port
	this->port = port;
}

XmlRpcServer::~XmlRpcServer()
{
}
/**************************************
* Start
*	Arranca el servidor
**************************************/
int XmlRpcServer::Start()
{
	Log("-Start [%p]\n",this);

	//Start it
	running = 1;

	//And run
	Run();
	
	return 1;
}

/**************************************
* XmlRpcServer
*	Constructor
**************************************/
int XmlRpcServer::Run()
{
	char name[65];

	//While we are not stopped
	while (running)
	{
		//LOg
		Log(">Run Server [%p]\n",this);
		
		//Le pasamos como nombre un puntero a nosotros mismos
		sprintf(name,"%p",this);

		//Socket d'écoute créée ICI, et non par Abyss : ServerCreate() ouvre une
		//socket AF_INET, donc un contrôleur en IPv6 ne pourrait pas atteindre
		//l'API de contrôle — celle par laquelle TOUT passe. AF_INET6 +
		//IPV6_V6ONLY=0 entend les deux familles sur une seule socket, et
		//ServerCreateSocket prend le descripteur tel quel.
		//
		//OÙ ÉCOUTER — c'est une question de SÛRETÉ, pas de confort. Dès qu'un
		//réseau interne est déclaré (--internal-ip), l'API de contrôle s'y
		//restreint : elle pilote entièrement le serveur média, elle n'a rien à
		//faire sur une interface publique. Sans réseau interne déclaré, on garde
		//l'écoute historique sur toutes les interfaces — c'est le déploiement
		//simple, et le restreindre casserait l'existant.
		//
		//Une seule socket, donc une seule famille quand l'adresse est précise :
		//si les deux profils internes sont configurés, l'IPv4 l'emporte (choix
		//déterministe, majoritaire sur les plans de contrôle). Le log le dit.
		IPAddress listenAddr;

		if (AddressProfiles::IsAvailable(AddressProfiles::InternalV4))
			listenAddr = AddressProfiles::BindAddress(AddressProfiles::InternalV4);
		else if (AddressProfiles::IsAvailable(AddressProfiles::InternalV6))
			listenAddr = AddressProfiles::BindAddress(AddressProfiles::InternalV6);

		const int listenFamily = listenAddr.IsSet() ? listenAddr.Family() : AF_INET6;

		int listenFd = socket(listenFamily, SOCK_STREAM, 0);

		if (listenFd < 0)
		{
			Error("-XmlRpcServer: cannot create listening socket (errno %d)\n",errno);
			return 0;
		}

		int optval = 1;
		setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

		//Échec non fatal : on perd les clients v4, on ne perd pas le serveur.
		//Sans objet quand on écoute sur une adresse v6 précise, où la question de
		//la double famille ne se pose pas.
		if (listenFamily == AF_INET6 && !listenAddr.IsSet())
		{
			int v6only = 0;
			if (setsockopt(listenFd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)) < 0)
				Error("-XmlRpcServer: cannot clear IPV6_V6ONLY (errno %d) — IPv4 controllers will not be served\n",errno);
		}

		const IPEndpoint listenOn = listenAddr.IsSet() ? listenAddr.To(port)
		                                              : IPAddress::Any(AF_INET6).To(port);

		if (listenAddr.IsSet())
			Log("-XmlRpcServer: ecoute restreinte au reseau interne [%s:%d] (--internal-ip)\n",
			    listenAddr.ToString().c_str(),port);
		else
			Log("-XmlRpcServer: ecoute sur toutes les interfaces [:::%d]\n",port);

		if (bind(listenFd, listenOn, listenOn.Len()) < 0)
		{
			Error("-XmlRpcServer: cannot bind %s:%d (errno %d)\n",
			      listenAddr.IsSet() ? listenAddr.ToString().c_str() : "::",port,errno);
			close(listenFd);
			return 0;
		}

		//Abyss attend une socket DÉJÀ en écoute.
		if (listen(listenFd, 32) < 0)
		{
			Error("-XmlRpcServer: cannot listen on port %d (errno %d)\n",port,errno);
			close(listenFd);
			return 0;
		}

		//Creamos el servidor
		ServerCreateSocket(&srv,name, listenFd, DEFAULT_DOCS, "http.log");

		//Iniciamos el servidor
		ServerInit(&srv);

		//Set the handler
		abyss_bool ret;

		//Create abyss handler
		ServerReqHandler3 abbysHndlr;

		//Set
		abbysHndlr.userdata	= (void*)this;
		abbysHndlr.handleReq	= RequestHandler;
		abbysHndlr.term		= NULL;
		//Pile déclarée pour les threads de requête Abyss. La valeur par défaut
		//(0) est trop juste pour les handlers qui rendent du TEXTE via
		//ImageMagick/fontconfig/freetype (SetParticipantDisplayName →
		//Overlay::RenderText) : débordement de pile = SIGSEGV sans la moindre
		//trace, systématique, vécu en recette (mcu.log s'arrête après
		//« Using helvetica »). Reproduit hors mcu : le même rendu segfaute
		//dans un thread à 64 Ko de pile et passe à 128 Ko — 1 Mo prend une
		//marge confortable pour tous les usages Magick des handlers
		//(RenderText, LoadImage des fonds/overlays).
		abbysHndlr.handleReqStackSize = 1024*1024;

		//Add handler
		ServerAddHandler3(&srv,&abbysHndlr,&ret);

		//Vamos a buscar en orden inverso
		LstHandlers::reverse_iterator it;

		//Recorremos la lista
		for (it=lstHandlers.rbegin();it!=lstHandlers.rend();it++)
			Log("-Handler on %s\n",(*it).first.c_str());

		//Ejecutamos
		ServerRun(&srv);

		//Log
		Log("<Run\n");
	}

	return 1;
}

/**************************************
* Stop
*	Para el servidor
**************************************/
int XmlRpcServer::Stop()
{
	Log("-Stop [%p]\n",this);

	running = 0;

	//Stop sercer
	ServerTerminate(&srv);
	
	return 1;
}
/**************************************
* AddHandler 
*	A�ade un handler para una uri
**************************************/
int XmlRpcServer::AddHandler(std::string base,Handler* hnd)
{
	//A�adimos al map
	lstHandlers[base] = hnd;

	return 1;
}

/**************************************
* RequestHandler
*	Callback
**************************************/
void XmlRpcServer::RequestHandler(void *par,TSession *ses, abyss_bool *ret)
{
	//Obtenemos el servidor
	XmlRpcServer *serv = (XmlRpcServer *)par;

	//Procesamos la llamada
	*ret = serv->DispatchRequest(ses);
}

/**************************************
* DispatchRequest
*       Busca el handler para procesar la peticion
**************************************/
int XmlRpcServer::DispatchRequest(TSession *ses)
{
	TRequestInfo *req;

	//Get request info
	SessionGetRequestInfo(ses,(const TRequestInfo**)&req);

	//Log it
	//Log("-Dispatching [%s]\n",req->uri);


	//Obtenemos la uri
	std::string uri = req->uri;

	//Vamos a buscar en orden inverso
	LstHandlers::reverse_iterator it;

	//Check stop
	if (uri.find("/stop")==0)
	{
		//Stop
		Stop();
		//Devolvemos el error
		SendResponse(ses,200,SHUTDOWNMSG,strlen(SHUTDOWNMSG));
		//Exit
		return 1;
	}
	
	//Recorremos la lista
	for (it=lstHandlers.rbegin();it!=lstHandlers.rend();it++)	
	{
		//Si la uri empieza por la base del handler
		if (uri.find((*it).first)==0)
			//Ejecutamos el handler
			return (*it).second->ProcessRequest(req,ses);
	}

	//Devolvemos el error
	SendError(ses,404);

	//Exit
	return 1;
}



/**************************************
* GetBody
	Devuelve el body de una peticion
**************************************/
int XmlRpcServer::GetBody(TSession *ses,char *body,DWORD bodyLen)
{
	int len=0;

	//MIentras no hayamos leido del todo
	while (len<bodyLen)
	{
		char * buffer;
		size_t readed;

		//If there is no data available
		if (!SessionReadDataAvail(ses))
			//Refill buffer
			SessionRefillBuffer(ses);

		//Read data
		SessionGetReadData(ses,bodyLen-len,(const char**)&buffer,&readed);

		//If not readed
		if (!readed)
			//error
			return Error("Not enought data readed");
		//Copy
		memcpy(body+len,buffer,readed);

		//Increased readed
		len+=readed;
	}

	//Return
	return len;
}

/**************************************
* SendResponse
*	Envia la respuesta de una peticion
**************************************/
int XmlRpcServer::SendResponse(TSession *r, short code, const char *msg, int length)
{
	//Pas de mode chunked : on connait toujours la longueur, et chunked +
	//Content-Length simultanes est interdit (RFC 7230 3.3.3, rejete par OTP 27)

	//POnemos el codigo
	ResponseStatus(r,code);

	//El content length
	ResponseContentLength(r, length);

	//Escribimos la respuesta
	ResponseWriteStart(r);
	
	//La mandamos
	ResponseWriteBody(r,(char*)msg,length);

	//End it
	ResponseWriteEnd(r);

	return 1;
}

/**************************************
* SendError
*	Devuelve el html con el error
**************************************/
int XmlRpcServer::SendError(TSession * r, short code) 
{
	Log("-XmlRpcServer::SendError [code:%d]\n",code);

	//POnemos el content type
	ResponseContentType(r, (char*)"text/html; charset=\"utf-8\"");

	//Escribimos el codigo de error
	return SendResponse(r,code,(char*)ERRORMSG,strlen(ERRORMSG));
}

/**************************************
* SendError
*	Devuelve el html con el error
**************************************/
int XmlRpcServer::SendError(TSession * r, short code,const char *msg)
{
	Log("-XmlRpcServer::SendError [code:%d,msg=%s]\n",code,msg);

	//POnemos el content type
	ResponseContentType(r, (char*)"text/html; charset=\"utf-8\"");

	//Escribimos el codigo de error
	return SendResponse(r,code,(char*)msg,strlen(msg));
}
