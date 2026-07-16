/*
 * wstest.cpp
 *
 * Harnais de test autonome du serveur WebSocket (cf. websocket-refactor.md §8).
 * Démarre un WebSocketServer avec un handler d'écho (TextEchoWebsocketHandler,
 * fourni par websocketserver.h) sur le chemin /echo, puis reste en vie jusqu'à
 * réception d'un signal. Le client de test (test/websocket/ws_client.py) s'y
 * connecte et vérifie handshake, écho texte/binaire, ping/pong et close.
 *
 * Ce binaire ne dépend PAS de JSR309/RTP : il n'exerce que la brique
 * WebSocketServer/WebSocketConnection/WebSocketTransport — ce qui en fait le
 * harnais de non-régression des Phases 0 → 2 du refactor.
 *
 * Build : make -f mcu/Makefile.rpm wstest   (sortie : bin/debug/wstest)
 * Usage : wstest [port] [-d]
 */
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <memory>
#include "log.h"
#include "websocketserver.h"

static WebSocketServer* gServer = NULL;

static void sigHandler(int sig)
{
	if (gServer)
		gServer->End();
	exit(0);
}

int main(int argc, char** argv)
{
	int port = 9001;
	bool secure = false;
	const char* certfile = NULL;
	const char* keyfile  = NULL;

	//Parse args : wstest [port] [-d] [--secure] [--cert file] [--key file]
	for (int i=1;i<argc;i++)
	{
		if (strcmp(argv[i],"-d")==0)
			Logger::EnableDebug(true);
		else if (strcmp(argv[i],"--secure")==0)
			secure = true;
		else if (strcmp(argv[i],"--cert")==0 && i+1<argc)
			certfile = argv[++i];
		else if (strcmp(argv[i],"--key")==0 && i+1<argc)
			keyfile = argv[++i];
		else
			port = atoi(argv[i]);
	}

	//Do not die on a broken pipe (peer closed)
	signal(SIGPIPE, SIG_IGN);
	signal(SIGINT,  sigHandler);
	signal(SIGTERM, sigHandler);

	WebSocketServer server;
	gServer = &server;

	//Mode sécurisé (wss://) si demandé
	if (secure)
		server.SetSecure(true, certfile, keyfile);

	//The echo handler uses weak_from_this() → must be owned by a shared_ptr
	auto echo = std::make_shared<TextEchoWebsocketHandler>();
	server.AddHandler("/echo", echo.get());

	//Init() starts the accept thread by itself (no Start() needed)
	if (! server.Init(port))
	{
		Error("wstest: cannot init WebSocket server on port %d\n", port);
		return 1;
	}

	Log("wstest: echo WebSocket server ready on port %d (path /echo)\n", port);

	//Stay alive until a signal ends us
	while (true)
		pause();

	return 0;
}
