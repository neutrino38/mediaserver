/*
 * websockettransport.cpp
 *
 * Implémentation TLS (wss://) du transport WebSocket. Calquée sur dtls.cpp :
 * SSL + BIO mémoire, pilotés depuis la boucle poll() non bloquante unique du
 * serveur. OpenSSL reste confiné à ce fichier ; les en-têtes WebSocket ne
 * connaissent que la façade WebSocketTlsTransport (ClassInit/IsAvailable/Create).
 */
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <mutex>
#include <vector>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include "log.h"
#include "websockettransport.h"

//Contexte TLS serveur partagé (une instance pour toutes les connexions), comme
//DTLSConnection::ssl_ctx. Créé par WebSocketTlsTransport::ClassInit().
static SSL_CTX* g_ssl_ctx = NULL;

//Contexte TLS client, pour les connexions SORTANTES (SPEC WS-CLIENT §4.4). Il est
//distinct du précédent : un client vérifie son pair et n'a pas de certificat à
//présenter. Créé à la première connexion sortante, donc potentiellement depuis le
//thread XML-RPC : le mutex protège sa création et sa configuration.
static std::mutex   g_client_mutex;
static SSL_CTX*     g_client_ctx    = NULL;
static bool         g_client_verify = true;
static std::string  g_client_cafile;

/*******************************************************************************
 * TlsTransportImpl — transport TLS concret (BIO mémoire)
 ******************************************************************************/
namespace {

class TlsTransportImpl : public WebSocketTransport
{
public:
	TlsTransportImpl(SSL_CTX* ctx, bool client = false,
			 const std::string& host = std::string(), bool verifyPeer = false)
		: ctx(ctx), ssl(NULL), read_bio(NULL), write_bio(NULL),
		  fd(FD_INVALID), handshakeDone(false), peerClosed(false),
		  client(client), host(host), verifyPeer(verifyPeer)
	{}

	virtual ~TlsTransportImpl() { Shutdown(); }

	virtual int Init(int fd)
	{
		this->fd = fd;

		//Socket non bloquant + pas de délai Nagle (comme le transport clair)
		int fsflags = fcntl(fd,F_GETFL,0);
		fcntl(fd,F_SETFL, fsflags | O_NONBLOCK);
		int flag = 1;
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(int));

		//Créer l'objet SSL et les BIO mémoire
		ssl = SSL_new(ctx);
		if (!ssl)
			return Error("-WsTls: SSL_new failed\n");
		read_bio  = BIO_new(BIO_s_mem());
		write_bio = BIO_new(BIO_s_mem());
		if (!read_bio || !write_bio)
			return Error("-WsTls: BIO_new failed\n");
		BIO_set_mem_eof_return(read_bio, -1);
		BIO_set_mem_eof_return(write_bio, -1);
		//SSL prend possession des BIO (libérés par SSL_free)
		SSL_set_bio(ssl, read_bio, write_bio);

		if (client)
		{
			if (!InitClientSsl())
				return 0;
			SSL_set_connect_state(ssl);
		} else {
			//Côté serveur
			SSL_set_accept_state(ssl);
		}

		return 1;
	}

	virtual int Recv(BYTE* buffer, DWORD size)
	{
		if (broken)
			return -1;

		//Alimenter le read_bio avec les octets bruts du socket
		FillReadBio();

		//Handshake TLS non terminé : le faire progresser
		if (!handshakeDone)
		{
			int h = DoHandshake();
			if (h < 0)  return -1;	//échec → fermer
			if (h == 0) return 0;	//en cours
			//h == 1 : terminé, on tente de lire des données applicatives
		}

		//Lire des données applicatives déchiffrées
		int n = SSL_read(ssl, buffer, size);
		if (n > 0)
			return n;

		int err = SSL_get_error(ssl, n);
		if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
			//Rien de plus pour l'instant ; si le pair a fermé, terminer
			return peerClosed ? -1 : 0;
		if (err == SSL_ERROR_ZERO_RETURN)
			//close_notify TLS reçu
			return -1;

		//Autre erreur
		return -1;
	}

	virtual int Send(const BYTE* buffer, DWORD size)
	{
		if (!handshakeDone)
			//Ne devrait pas arriver (données applicatives après onOpen) ; on
			//pousse au moins ce qui traîne côté handshake.
			return Flush()<0 ? -1 : 0;

		//SSL_write écrit dans write_bio (mémoire) : ne bloque jamais pour des
		//tailles raisonnables → aucune trame perdue.
		int n = SSL_write(ssl, buffer, size);
		if (n <= 0)
		{
			int err = SSL_get_error(ssl, n);
			if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
				return Flush()<0 ? -1 : 0;
			return -1;
		}
		//Pousser les octets chiffrés sur le socket
		if (Flush() < 0)
			return -1;
		return n;
	}

	virtual int Flush()
	{
		//Transport inutilisable : le dire par la première écriture, sinon la
		//connexion attendrait un pair qu'on n'a jamais salué.
		if (broken)
			return -1;

		//Le ClientHello doit partir TOUT SEUL (§4.4) : rien n'arrive encore sur ce
		//socket, donc rien n'appellerait Recv. C'est la première écriture possible
		//— le POLLOUT qui clôt le connect() — qui lance le handshake.
		if (client && !handshakeDone && DoHandshake() < 0)
			return -1;

		//1. Vider write_bio dans le tampon de sortie
		int pend;
		while ((pend = BIO_ctrl_pending(write_bio)) > 0)
		{
			size_t off = pendingOut.size();
			pendingOut.resize(off + pend);
			int n = BIO_read(write_bio, pendingOut.data()+off, pend);
			if (n <= 0) { pendingOut.resize(off); break; }
			if (n < pend) pendingOut.resize(off + n);
		}

		//2. Écrire le tampon sur le socket (gère les écritures partielles)
		size_t written = 0;
		while (written < pendingOut.size())
		{
			int w = write(fd, pendingOut.data()+written, pendingOut.size()-written);
			if (w > 0) { written += (size_t)w; continue; }
			if (w < 0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR))
				break;			//socket plein : on reprendra au prochain POLLOUT
			//Erreur socket
			if (written)
				pendingOut.erase(pendingOut.begin(), pendingOut.begin()+written);
			return -1;
		}
		if (written)
			pendingOut.erase(pendingOut.begin(), pendingOut.begin()+written);
		return 0;
	}

	virtual bool WantsWrite()
	{
		return !pendingOut.empty() || BIO_ctrl_pending(write_bio) > 0;
	}

	virtual bool IsReady()
	{
		return handshakeDone;
	}

	virtual int GetFd() { return fd; }

	virtual void Shutdown()
	{
		if (ssl)
		{
			//Best-effort close_notify
			SSL_shutdown(ssl);
			//Libère aussi read_bio/write_bio
			SSL_free(ssl);
			ssl = NULL;
			read_bio = write_bio = NULL;
		}
		if (fd != FD_INVALID)
		{
			shutdown(fd, SHUT_RDWR);
			close(fd);
			fd = FD_INVALID;
		}
	}

private:
	void FillReadBio()
	{
		BYTE tmp[4096];
		while (true)
		{
			int n = read(fd, tmp, sizeof(tmp));
			if (n > 0)   { BIO_write(read_bio, tmp, n); continue; }
			if (n == 0)  { peerClosed = true; break; }	//EOF
			break;						//EAGAIN / erreur → stop
		}
	}

	//Ce qu'un client doit poser AVANT SSL_connect : qui il appelle, et à quelle
	//condition il accepte de lui parler.
	bool InitClientSsl()
	{
		//Un littéral IP ne se met pas dans un SNI (RFC 6066 §3, qui exige un nom)
		//et ne se vérifie pas comme un nom : il se compare aux SAN de type
		//iPAddress. Sans cette distinction, wss://127.0.0.1/ échouerait toujours.
		BYTE probe[sizeof(struct in6_addr)];
		const bool literal = inet_pton(AF_INET ,host.c_str(),probe)==1 ||
				     inet_pton(AF_INET6,host.c_str(),probe)==1;

		if (!literal && !SSL_set_tlsext_host_name(ssl,host.c_str()))
		{
			broken = true;
			return Error("-WsTls: cannot set SNI for \"%s\"\n",host.c_str());
		}

		if (verifyPeer)
		{
			//Vérifier la chaîne SANS vérifier le nom laisserait n'importe quelle
			//autorité publique se faire passer pour le pair demandé.
			X509_VERIFY_PARAM* param = SSL_get0_param(ssl);
			X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
			const int ok = literal ? X509_VERIFY_PARAM_set1_ip_asc(param,host.c_str())
					       : X509_VERIFY_PARAM_set1_host(param,host.c_str(),0);
			if (!ok)
			{
				broken = true;
				return Error("-WsTls: cannot check peer identity \"%s\"\n",host.c_str());
			}
		}

		//SSL_VERIFY_PEER fait ÉCHOUER le handshake sur un certificat refusé : sans
		//lui la vérification serait purement décorative.
		SSL_set_verify(ssl, verifyPeer ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, NULL);

		return true;
	}

	int DoHandshake()
	{
		int r = client ? SSL_connect(ssl) : SSL_accept(ssl);
		if (r == 1)
		{
			handshakeDone = true;
			Debug("-WsTls: handshake done\n");
			return 1;
		}
		int err = SSL_get_error(ssl, r);
		if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
			return 0;	//en cours (la sortie éventuelle est dans write_bio)

		//Échec du handshake. Le résultat de vérification est la seule chose qui
		//distingue un certificat refusé d'une panne de protocole.
		broken = true;
		Error("-WsTls: handshake failed [client:%d,ssl_err:%d,verify:%ld,host:\"%s\"]\n",
		      client, err, SSL_get_verify_result(ssl), host.c_str());
		ERR_print_errors_fp(stderr);
		return -1;
	}

private:
	SSL_CTX* ctx;
	SSL*     ssl;
	BIO*     read_bio;	//réseau → SSL
	BIO*     write_bio;	//SSL → réseau
	int      fd;
	bool     handshakeDone;
	bool     peerClosed;
	bool        client;	//wss:// sortant : c'est nous qui saluons
	std::string host;	//hôte de l'URL (SNI + identité vérifiée)
	bool        verifyPeer;
	bool        broken = false;	//transport condamné : toute E/S le dit
	std::vector<BYTE> pendingOut;	//octets chiffrés en attente d'écriture socket
};

} // namespace

/*******************************************************************************
 * WebSocketTlsTransport — façade statique
 ******************************************************************************/
bool WebSocketTlsTransport::ClassInit(const std::string& certfile, const std::string& keyfile)
{
	//Déjà initialisé ?
	if (g_ssl_ctx)
		return true;

	//Init OpenSSL (idempotent ; utile pour les binaires qui n'appellent pas
	//OPENSSL_init_ssl par ailleurs, comme le harnais wstest).
	OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);

	SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
	if (!ctx)
	{
		ERR_print_errors_fp(stderr);
		return Error("-WebSocketTlsTransport::ClassInit() | No SSL context\n");
	}

	//TLS 1.2 minimum
	if (!SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION))
	{
		SSL_CTX_free(ctx);
		return Error("-WebSocketTlsTransport::ClassInit() | Could not set minimum TLS version\n");
	}

	//Certificat (chaîne) + clé privée
	if (!SSL_CTX_use_certificate_chain_file(ctx, certfile.c_str()))
	{
		ERR_print_errors_fp(stderr);
		SSL_CTX_free(ctx);
		return Error("-WebSocketTlsTransport::ClassInit() | certificate file '%s' unusable\n", certfile.c_str());
	}
	if (!SSL_CTX_use_PrivateKey_file(ctx, keyfile.c_str(), SSL_FILETYPE_PEM) || !SSL_CTX_check_private_key(ctx))
	{
		ERR_print_errors_fp(stderr);
		SSL_CTX_free(ctx);
		return Error("-WebSocketTlsTransport::ClassInit() | private key file '%s' unusable\n", keyfile.c_str());
	}

	//Pas de cache de session
	SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);
	//Pas de vérification du certificat client (navigateurs sans certificat)
	SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);

	g_ssl_ctx = ctx;
	Log("-WebSocketTlsTransport: TLS server context ready [cert:\"%s\",key:\"%s\"]\n",
	    certfile.c_str(), keyfile.c_str());
	return true;
}

bool WebSocketTlsTransport::IsAvailable()
{
	return g_ssl_ctx != NULL;
}

std::unique_ptr<WebSocketTransport> WebSocketTlsTransport::Create()
{
	if (!g_ssl_ctx)
		return nullptr;
	return std::make_unique<TlsTransportImpl>(g_ssl_ctx);
}

//Bâtit le contexte TLS client depuis g_client_cafile. À appeler g_client_mutex
//tenu. Rend NULL et journalise la cause si le contexte est inutilisable.
static SSL_CTX* CreateClientContextLocked()
{
	//Idempotent, et nécessaire : un mediaserver qui n'écoute qu'en clair n'a
	//jamais appelé ClassInit, et peut très bien appeler en wss://.
	OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);

	SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
	if (!ctx)
	{
		ERR_print_errors_fp(stderr);
		Error("-WebSocketTlsTransport: no SSL client context\n");
		return NULL;
	}

	//TLS 1.2 minimum, comme le contexte serveur
	if (!SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION))
	{
		SSL_CTX_free(ctx);
		Error("-WebSocketTlsTransport: could not set minimum TLS version (client)\n");
		return NULL;
	}

	//Magasin du système
	if (!SSL_CTX_set_default_verify_paths(ctx))
	{
		ERR_print_errors_fp(stderr);
		Error("-WebSocketTlsTransport: no system CA store\n");
	}

	//Une autorité demandée et illisible n'est pas un détail : la jambe serait
	//refusée plus tard, et personne ne saurait que c'est ce fichier.
	if (!g_client_cafile.empty() &&
	    !SSL_CTX_load_verify_locations(ctx, g_client_cafile.c_str(), NULL))
	{
		ERR_print_errors_fp(stderr);
		SSL_CTX_free(ctx);
		Error("-WebSocketTlsTransport: CA file '%s' unusable\n", g_client_cafile.c_str());
		return NULL;
	}

	//Pas de cache de session
	SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);

	Log("-WebSocketTlsTransport: TLS client context ready [ca:\"%s\"]\n", g_client_cafile.c_str());
	return ctx;
}

bool WebSocketTlsTransport::SetClientConfig(bool verifyPeer, const std::string& cafile)
{
	std::lock_guard<std::mutex> lock(g_client_mutex);

	g_client_verify = verifyPeer;
	g_client_cafile = cafile;

	//Le magasin d'autorités est porté par le contexte : le refaire MAINTENANT.
	//Le bâtir à la première connexion sortante laisserait une autorité illisible
	//dormir jusqu'au premier appel réel — l'exploitant ne verrait sa faute de
	//frappe qu'en production. SSL_CTX_free ne libère rien tant qu'un SSL le
	//référence.
	if (g_client_ctx)
	{
		SSL_CTX_free(g_client_ctx);
		g_client_ctx = NULL;
	}

	g_client_ctx = CreateClientContextLocked();

	return g_client_ctx != NULL;
}

bool WebSocketTlsTransport::GetClientVerifyPeer()
{
	std::lock_guard<std::mutex> lock(g_client_mutex);
	return g_client_verify;
}

std::unique_ptr<WebSocketTransport> WebSocketTlsTransport::CreateClient(const std::string& host, bool verifyPeer)
{
	std::lock_guard<std::mutex> lock(g_client_mutex);

	//Le contexte vient normalement de SetClientConfig, posé au démarrage. Le
	//bâtir ici couvre le mediaserver qui n'a jamais configuré son côté client.
	if (!g_client_ctx)
		g_client_ctx = CreateClientContextLocked();

	if (!g_client_ctx)
		return nullptr;

	return std::make_unique<TlsTransportImpl>(g_client_ctx, true, host, verifyPeer);
}
