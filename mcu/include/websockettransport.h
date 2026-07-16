#ifndef _WebSocketTransport_H_
#define _WebSocketTransport_H_

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <memory>
#include <string>
#include <vector>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "config.h"

/**
 * WebSocketTransport
 *
 * Abstraction du transport d'une connexion WebSocket : isole la lecture/écriture
 * sur le socket du reste de la connexion (parseur HTTP d'upgrade, parseur de
 * trames, file de frames). Objectif : pouvoir greffer TLS (WSS) sans toucher à
 * la logique WebSocket.
 *
 * Phase 0 (refactor pur, cf. websocket-refactor.md) : seule l'implémentation en
 * clair `WebSocketPlainTransport` existe ; c'est un simple relais read()/write().
 * Les méthodes Handshake()/WantsWrite() sont déjà présentes au contrat pour le
 * futur transport TLS (Phase 2) mais restent triviales ici — la connexion ne les
 * sollicite pas encore.
 */
class WebSocketTransport
{
public:
	virtual ~WebSocketTransport() {}

	/** Initialise le transport sur le socket accepté (options socket, etc.).
	 *  Renvoie >0 si OK. Le handshake TLS éventuel est piloté de façon
	 *  transparente depuis Recv/Send/Flush (pas de méthode dédiée). */
	virtual int  Init(int fd) = 0;

	/** Lit des octets applicatifs (déchiffrés).
	 *  >0 = octets lus ; 0 = rien de disponible maintenant (would-block ou
	 *  handshake TLS en cours) ; <0 = connexion fermée / erreur. */
	virtual int  Recv(BYTE* buffer, DWORD size) = 0;

	/** Émet des octets applicatifs. Les octets non encore écrits sur le socket
	 *  (write() partiel/EAGAIN, ou chiffrement TLS) sont conservés en interne et
	 *  repoussés par Flush() : l'appelant peut donc libérer son tampon aussitôt.
	 *  Renvoie le nombre d'octets applicatifs acceptés, <0 = erreur. */
	virtual int  Send(const BYTE* buffer, DWORD size) = 0;

	/** Pousse vers le socket les octets en attente. 0 = OK, <0 = erreur. */
	virtual int  Flush() = 0;

	/** Reste-t-il des octets à pousser sur le socket ? */
	virtual bool WantsWrite() = 0;

	/** Ferme le socket (shutdown + close) et l'invalide. Idempotent. */
	virtual void Shutdown() = 0;

	/** fd sous-jacent (pour poll()). */
	virtual int  GetFd() = 0;
};

/**
 * WebSocketPlainTransport
 * 	Transport WebSocket en clair (ws://). Relais direct read()/write() sur le
 * 	socket TCP, reproduisant à l'identique le comportement historique.
 */
class WebSocketPlainTransport : public WebSocketTransport
{
public:
	WebSocketPlainTransport() : fd(FD_INVALID) {}
	virtual ~WebSocketPlainTransport() { Shutdown(); }

	virtual int Init(int fd)
	{
		this->fd = fd;

		//Non bloquant : permet de détecter la fermeture par End()
		int fsflags = fcntl(fd,F_GETFL,0);
		fsflags |= O_NONBLOCK;
		fcntl(fd,F_SETFL,fsflags);

		//Pas de délai Nagle
		int flag = 1;
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(int));

		return 1;
	}

	virtual int  Recv(BYTE* buffer, DWORD size)
	{
		int n = read(fd, buffer, size);
		if (n > 0)  return n;
		if (n == 0) return -1;			//pair fermé (EOF)
		if (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR)
			return 0;			//rien de disponible maintenant
		return -1;				//erreur
	}

	virtual int  Send(const BYTE* buffer, DWORD size)
	{
		//Copie dans le tampon de sortie (l'appelant peut libérer aussitôt), puis
		//tente d'écouler. Les octets non écrits (write partiel/EAGAIN) restent
		//dans pendingOut et sont repoussés par Flush() au prochain POLLOUT.
		pendingOut.insert(pendingOut.end(), buffer, buffer+size);
		if (Flush() < 0)
			return -1;
		return (int)size;
	}

	virtual int  Flush()
	{
		size_t written = 0;
		while (written < pendingOut.size())
		{
			int w = write(fd, pendingOut.data()+written, pendingOut.size()-written);
			if (w > 0) { written += (size_t)w; continue; }
			if (w < 0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR))
				break;			//socket plein : on reprendra au prochain POLLOUT
			//Erreur d'écriture
			if (written)
				pendingOut.erase(pendingOut.begin(), pendingOut.begin()+written);
			return -1;
		}
		if (written)
			pendingOut.erase(pendingOut.begin(), pendingOut.begin()+written);
		return 0;
	}

	virtual bool WantsWrite()				{ return !pendingOut.empty(); }
	virtual int  GetFd()					{ return fd; }

	virtual void Shutdown()
	{
		if (fd != FD_INVALID)
		{
			shutdown(fd, SHUT_RDWR);
			close(fd);
			fd = FD_INVALID;
		}
	}

private:
	int fd;
	std::vector<BYTE> pendingOut;	//octets en attente d'écriture socket
};

/**
 * WebSocketTlsTransport
 * 	Façade statique du transport TLS (wss://). L'implémentation concrète (SSL /
 * 	BIO mémoire, calquée sur dtls.cpp) vit dans websockettransport.cpp afin de
 * 	garder OpenSSL hors des en-têtes WebSocket.
 */
class WebSocketTlsTransport
{
public:
	/** Initialise (une seule fois) le contexte TLS serveur à partir des fichiers
	 *  PEM. Renvoie false en cas d'échec (certificat/clé invalides…). */
	static bool ClassInit(const std::string& certfile, const std::string& keyfile);

	/** Le TLS est-il disponible (ClassInit a réussi) ? */
	static bool IsAvailable();

	/** Crée une nouvelle instance de transport TLS, ou nullptr si indisponible. */
	static std::unique_ptr<WebSocketTransport> Create();
};

#endif /* _WebSocketTransport_H_ */
