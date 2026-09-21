/**
 * test_ws_client_xmlrpc.cpp — la jambe texte sortante vue du CONTRÔLEUR
 * (lot 6 de docs/conception/WS-CLIENT/SPEC.md §4.8).
 *
 * Le lot 5 a livré `Endpoint::ConnectMediaConnection`. Ici on passe par la
 * porte qu'emprunte elixip : la table `jsr309CmdList`. Ce que ces tests
 * tiennent, et qu'aucun test du lot 5 ne voit :
 *
 *   - la méthode est RECENSÉE dans la table. Sans cette ligne, le C++ en
 *     dessous n'existe pour personne, et rien d'autre ne le dirait ;
 *   - l'ordre et le typage des paramètres `(sessionId, endpointId, media,
 *     role, url)`. Une inversion `media`/`role` passerait le parse sans bruit ;
 *   - une session inconnue, un endpoint inconnu, un média non texte et une URL
 *     inutilisable répondent une FAUTE, et n'arment rien ;
 *   - un appel mal typé rend une faute au lieu d'abattre le serveur (le défaut
 *     de test_xmlrpc_fault.cpp, par cette porte-ci).
 *
 *     ./tests/runtests --gtest_filter='WsClientXmlRpc*'
 */
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>

#include "xmlhandler.h"
#include "websocketserver.h"
#include "../src/jsr309/JSR309Manager.h"
#include "../src/jsr309/WSEndpoint.h"

//La table de commandes de l'API /jsr309, telle que main.cpp la voit
extern XmlHandlerCmd jsr309CmdList[];

namespace {

//---- Le pair : un serveur WebSocket qui compte ses ouvertures ----------------

class CountingPeer :
	public WebSocketServer::Handler,
	public WebSocket::Listener,
	public std::enable_shared_from_this<CountingPeer>
{
public:
	virtual void onWebSocketConnection(const HTTPRequest& request,WebSocket* ws)
	{
		ws->Accept(weak_from_this());
	}

	virtual void onOpen(WebSocket*)
	{
		std::lock_guard<std::mutex> lock(mutex);
		++opens;
	}
	virtual void onMessageStart(WebSocket*,WebSocket::MessageType,const DWORD)	{ }
	virtual void onMessageData(WebSocket*,const BYTE*,const DWORD)		{ }
	virtual void onMessageEnd(WebSocket*)					{ }
	virtual void onError(WebSocket*)					{ }
	virtual void onClose(WebSocket*)					{ }

	int Opens()	{ std::lock_guard<std::mutex> lock(mutex); return opens;	}

private:
	std::mutex	mutex;
	int		opens = 0;
};

//---- Outillage ---------------------------------------------------------------

//WebSocketServer::Init() rend la main AVANT que son thread n'appelle listen().
bool WaitForListener(int port)
{
	for (int attempt = 0; attempt < 100; ++attempt)
	{
		int fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd < 0)
			return false;

		sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		addr.sin_addr.s_addr = inet_addr("127.0.0.1");

		bool ok = connect(fd, (sockaddr*)&addr, sizeof(addr)) == 0;
		close(fd);
		if (ok)
			return true;
		usleep(20 * 1000);
	}
	return false;
}

bool WaitFor(const std::function<bool()>& until, int timeoutMs = 3000)
{
	for (int waited = 0; waited < timeoutMs; waited += 10)
	{
		if (until())
			return true;
		usleep(10 * 1000);
	}
	return until();
}

//Le pair distant, et le serveur par lequel le WSEndpoint ouvre ses connexions
//sortantes : un seul réacteur tient les deux bouts.
class PeerFixture
{
public:
	bool Start(int firstPort)
	{
		handler = std::make_shared<CountingPeer>();
		server.AddHandler("/t140", handler.get());

		for (int p = firstPort; p < firstPort+50 && !port; ++p)
			if (server.Init(p))
				port = p;

		if (!port || !WaitForListener(port))
			return false;

		WSEndpoint::SetServer(&server);
		return true;
	}

	~PeerFixture()
	{
		WSEndpoint::SetServer(NULL);
		if (port)
			server.End();
	}

	CountingPeer&	Peer()	{ return *handler;	}

	std::string Url(const std::string& path = "/t140")
	{
		return "ws://127.0.0.1:" + std::to_string(port) + path;
	}

private:
	WebSocketServer			server;
	std::shared_ptr<CountingPeer>	handler;
	int				port = 0;
};

//---- L'API de contrôle, telle que le contrôleur l'appelle --------------------

typedef xmlrpc_value* (*CommandFunc)(xmlrpc_env*,xmlrpc_value*,void*);

//Le handler recensé sous ce nom, ou NULL s'il n'y en a aucun.
CommandFunc FindCommand(const char* name)
{
	for (int i = 0; jsr309CmdList[i].name; ++i)
		if (strcmp(jsr309CmdList[i].name,name)==0)
			return jsr309CmdList[i].func;
	return NULL;
}

//Le `returnCode` d'une réponse : 1 = succès, 0 = faute, -1 = pas de réponse.
int ReturnCode(xmlrpc_env* env, xmlrpc_value* response)
{
	if (!response)
		return -1;

	xmlrpc_value* code = NULL;
	xmlrpc_struct_find_value(env, response, "returnCode", &code);
	if (!code)
		return -1;

	int value = -1;
	xmlrpc_parse_value(env, code, "i", &value);
	xmlrpc_DECREF(code);
	return value;
}

//Appelle ConnectMediaConnection comme le ferait un contrôleur bien typé.
int CallConnect(JSR309Manager& manager,int sessionId,int endpointId,
		int media,int role,const std::string& url)
{
	auto command = FindCommand("ConnectMediaConnection");
	if (!command)
		return -2;

	xmlrpc_env env;
	xmlrpc_env_init(&env);

	xmlrpc_value* params = xmlrpc_build_value(&env,"(iiiis)",
						  sessionId,endpointId,media,role,url.c_str());
	xmlrpc_value* response = command(&env,params,(void*)&manager);

	const int code = ReturnCode(&env,response);

	if (response) xmlrpc_DECREF(response);
	xmlrpc_DECREF(params);
	xmlrpc_env_clean(&env);

	return code;
}

//Une session et un endpoint texte, montés comme le ferait le contrôleur.
class SessionFixture
{
public:
	SessionFixture()
	{
		manager.Init(&handler,0);
		sessionId = manager.CreateMediaSession(L"ws-client-xmlrpc",0);
		if (sessionId>0 && manager.GetMediaSessionRef(sessionId,session))
			endpointId = session->EndpointCreate(L"leg",
							     /*audio=*/false,
							     /*video=*/false,
							     /*text=*/true);
	}

	~SessionFixture()
	{
		session.reset();
		manager.End();
	}

	//Ce que la jambe publie : la cible configurée, ou rien si elle n'est pas armée.
	std::string Published()
	{
		std::shared_ptr<Endpoint> endpoint = session->GetEndpoint(endpointId);
		if (!endpoint)
			return "";

		char* candidate = endpoint->GetMediaCandidates(MediaFrame::WS, MediaFrame::Text);
		if (!candidate)
			return "";

		const std::string url = candidate;
		free(candidate);
		return url;
	}

	XmlStreamingHandler		handler;
	JSR309Manager			manager;
	std::shared_ptr<MediaSession>	session;
	int				sessionId  = 0;
	int				endpointId = 0;
};

//Les valeurs de l'API, telles que docs/JSR-309-API.md §4 les publie
const int MEDIA_AUDIO	= 0;
const int MEDIA_TEXT	= 2;
const int ROLE_MAIN	= 0;

} // namespace

//=============================================================================

//Sans cette ligne dans la table, la méthode n'existe pour aucun contrôleur.
TEST(WsClientXmlRpc, LaMethodeEstRecenseeDansLaTable)
{
	EXPECT_TRUE(FindCommand("ConnectMediaConnection") != NULL);
}

//Le chemin nominal, de bout en bout : l'appel arme la jambe, la jambe atteint
//le pair, et c'est la CIBLE que le serveur publie.
TEST(WsClientXmlRpc, UnAppelArmeLaJambeQuiAtteintLePair)
{
	PeerFixture peer;
	if (!peer.Start(39700))
		GTEST_SKIP() << "Aucun port loopback disponible (sandbox réseau ?)";

	SessionFixture fixture;
	ASSERT_GT(fixture.sessionId,0);
	ASSERT_GT(fixture.endpointId,0);

	const std::string url = peer.Url();

	EXPECT_EQ(1, CallConnect(fixture.manager,fixture.sessionId,fixture.endpointId,
				 MEDIA_TEXT,ROLE_MAIN,url));

	EXPECT_TRUE(WaitFor([&]{ return peer.Peer().Opens() > 0; }))
		<< "la jambe armée par XML-RPC n'a jamais atteint le pair";

	EXPECT_EQ(url, fixture.Published())
		<< "GetMediaCandidates doit rendre la cible, et donc l'URL reçue ici";
}

//L'ordre des paramètres : `media` en 3e, `role` en 4e. Une inversion enverrait
//MEDIA_TEXT dans `role`, et le média deviendrait ROLE_MAIN — c'est-à-dire
//Audio, que la méthode refuse. Ce test échoue donc sur une inversion.
TEST(WsClientXmlRpc, UnMediaNonTexteEstRefuse)
{
	PeerFixture peer;
	if (!peer.Start(39760))
		GTEST_SKIP() << "Aucun port loopback disponible (sandbox réseau ?)";

	SessionFixture fixture;
	ASSERT_GT(fixture.endpointId,0);

	EXPECT_EQ(0, CallConnect(fixture.manager,fixture.sessionId,fixture.endpointId,
				 MEDIA_AUDIO,ROLE_MAIN,peer.Url()));

	EXPECT_EQ(0, peer.Peer().Opens()) << "rien ne doit avoir été tenté";
}

//Une URL qui ne se réparera pas en attendant est une faute IMMÉDIATE : elle
//n'arme rien, sinon le serveur boucherait pour toujours sur une faute de frappe.
TEST(WsClientXmlRpc, UneUrlInutilisableNArmeRien)
{
	PeerFixture peer;
	if (!peer.Start(39820))
		GTEST_SKIP() << "Aucun port loopback disponible (sandbox réseau ?)";

	SessionFixture fixture;
	ASSERT_GT(fixture.endpointId,0);

	EXPECT_EQ(0, CallConnect(fixture.manager,fixture.sessionId,fixture.endpointId,
				 MEDIA_TEXT,ROLE_MAIN,"http://127.0.0.1/t140"));
	EXPECT_EQ(0, CallConnect(fixture.manager,fixture.sessionId,fixture.endpointId,
				 MEDIA_TEXT,ROLE_MAIN,"pas une url"));

	EXPECT_EQ(0, peer.Peer().Opens());
}

//Deux identifiants que le contrôleur peut se tromper à donner.
TEST(WsClientXmlRpc, SessionOuEndpointInconnuEstUneFaute)
{
	PeerFixture peer;
	if (!peer.Start(39880))
		GTEST_SKIP() << "Aucun port loopback disponible (sandbox réseau ?)";

	SessionFixture fixture;
	ASSERT_GT(fixture.sessionId,0);

	EXPECT_EQ(0, CallConnect(fixture.manager,fixture.sessionId+1000,fixture.endpointId,
				 MEDIA_TEXT,ROLE_MAIN,peer.Url()));
	EXPECT_EQ(0, CallConnect(fixture.manager,fixture.sessionId,fixture.endpointId+1000,
				 MEDIA_TEXT,ROLE_MAIN,peer.Url()));

	EXPECT_EQ(0, peer.Peer().Opens());
}

//Un contrôleur qui se trompe de type ne doit pas emporter le serveur : sur le
//motif fautif, cette ligne abort() le binaire au lieu d'échouer.
TEST(WsClientXmlRpc, UnAppelMalTypeNeTuePasLeServeur)
{
	SessionFixture fixture;

	auto command = FindCommand("ConnectMediaConnection");
	ASSERT_TRUE(command != NULL);

	xmlrpc_env env;
	xmlrpc_env_init(&env);

	//Une struct là où le format attend un entier
	xmlrpc_value* bad    = xmlrpc_build_value(&env,"{s:s}","pas","un entier");
	xmlrpc_value* params = xmlrpc_build_value(&env,"(Viiis)",bad,0,0,0,"ws://127.0.0.1/t140");

	xmlrpc_value* response = command(&env,params,(void*)&fixture.manager);

	EXPECT_TRUE(env.fault_occurred) << "la faute doit remonter à l'appelant";
	EXPECT_TRUE(response == NULL)   << "aucune réponse ne se construit sur un env en faute";

	if (response) xmlrpc_DECREF(response);
	xmlrpc_DECREF(params);
	xmlrpc_DECREF(bad);
	xmlrpc_env_clean(&env);
}
