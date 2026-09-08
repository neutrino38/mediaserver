#include <srtp2/srtp.h>
#include "dtls.h"
#include "log.h"

#define SRTP_MASTER_KEY_LENGTH 16
#define SRTP_MASTER_SALT_LENGTH 14
#define SRTP_MASTER_LENGTH (SRTP_MASTER_KEY_LENGTH + SRTP_MASTER_SALT_LENGTH)

//Taille maximale d'un record TLS en clair (RFC 5246). Un record DTLS tient dans
//un datagramme UDP, donc en pratique on est bien en dessous ; ce qui compte est
//qu'aucun record ne puisse être livré en deux morceaux.
#define MAX_DTLS_APPLICATION_DATA 16384


/* Initialize static data. */
std::string DTLSConnection::certfile("mcu.crt");
std::string DTLSConnection::pvtfile("mcy.key");
// Liste de ciphers durcie : uniquement des suites fortes, on exclut
// explicitement les suites nulles/anonymes/export/faibles et les algos obsoletes.
std::string DTLSConnection::cipher("HIGH:!aNULL:!eNULL:!NULL:!EXPORT:!LOW:!MD5:!RC4:!3DES:!DES:!SEED:!IDEA:!PSK:!SRP");
SSL_CTX* DTLSConnection::ssl_ctx = NULL;
DTLSConnection::Suite DTLSConnection::suite = AES_CM_128_HMAC_SHA1_80;
DTLSConnection::LocalFingerPrints DTLSConnection::localFingerPrints;
DTLSConnection::AvailableHashes DTLSConnection::availableHashes;
bool DTLSConnection::hasDTLS = false;


/* Static callbacks for OpenSSL. */

static inline
int on_ssl_certificate_verify(int preverify_ok, X509_STORE_CTX* ctx)
{
	// Always valid.
	return 1;
}

static inline
void on_ssl_info(const SSL* ssl, int where, int ret)
{
	DTLSConnection *conn = (DTLSConnection*)SSL_get_ex_data(ssl, 0);
	conn->onSSLInfo(where, ret);  
}


/* Static methods. */

void DTLSConnection::SetCertificate(const char* cert,const char* key)
{
	//Set certificate files
	DTLSConnection::certfile.assign(cert);
	DTLSConnection::pvtfile.assign(key);
}

int DTLSConnection::ClassInit()
{
/* This is defined in openssl/srtp.h */
#ifdef HEADER_D1_SRTP_H
	Log("-DTLSConnection::ClassInit()\n");

	/* Create a single SSL context. */
	DTLSConnection::ssl_ctx = SSL_CTX_new(DTLS_method());
	if (! ssl_ctx) {
		// Print SSL error.
		ERR_print_errors_fp(stderr);
		return Error("-DTLSConnection::ClassInit() | No SSL context\n");
	}

	// N'autoriser que DTLS 1.2 et superieur (DTLS 1.2 <=> TLS 1.2 ; il n'existe
	// pas de DTLS 1.1, DTLS 1.0 correspond a TLS 1.1). On interdit donc DTLS 1.0.
	if (! SSL_CTX_set_min_proto_version(ssl_ctx, DTLS1_2_VERSION))
		return Error("-DTLSConnection::ClassInit() | Could not set minimum DTLS version to 1.2\n");
	// Set certificate.
	if (! SSL_CTX_use_certificate_file(ssl_ctx, certfile.c_str(), SSL_FILETYPE_PEM))
		return Error("-DTLSConnection::ClassInit() | Specified certificate file '%s' could not be used\n", certfile.c_str());

	if (! SSL_CTX_use_PrivateKey_file(ssl_ctx, pvtfile.c_str(), SSL_FILETYPE_PEM) || !SSL_CTX_check_private_key(ssl_ctx))
		return Error("-DTLSConnection::ClassInit() | Specified private key file '%s' could not be used\n",pvtfile.c_str());

	if (! SSL_CTX_set_cipher_list(ssl_ctx, cipher.c_str()))
		return Error("-DTLSConnection::ClassInit() | Invalid cipher specified in cipher list '%s' for DTLS-SRTP\n",cipher.c_str());

	// La selection de la courbe ECDH est automatique depuis OpenSSL 1.1.0
	// (plus besoin de EC_KEY_new_by_curve_name / SSL_CTX_set_tmp_ecdh, deprecies
	// en 3.0). Rien a faire ici.

	// Don't use session cache.
	SSL_CTX_set_session_cache_mode(ssl_ctx, SSL_SESS_CACHE_OFF);

	// Set look ahead
	// See -> https://bugs.debian.org/cgi-bin/bugreport.cgi?bug=775502
	SSL_CTX_set_read_ahead(ssl_ctx,true);

	// Require cert from client (mandatory for WebRTC).
	SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, on_ssl_certificate_verify);
	// Require cert from client (mandatory for WebRTC).
	
	// Set SSL info callback.
	SSL_CTX_set_info_callback(ssl_ctx, on_ssl_info);

	// Set suite.
	/*switch(suite) {
		case AES_CM_128_HMAC_SHA1_80:
			SSL_CTX_set_tlsext_use_srtp(ssl_ctx, "SRTP_AES128_CM_SHA1_80");
			break;
		case AES_CM_128_HMAC_SHA1_32:
			SSL_CTX_set_tlsext_use_srtp(ssl_ctx, "SRTP_AES128_CM_SHA1_32");
			break;
		default:
			return Error("-DTLSConnection::ClassInit() | Unsupported suite [%d] specified for DTLS-SRTP\n",suite);
	}*/

	// Try to use GCM suite
	if (SSL_CTX_set_tlsext_use_srtp(ssl_ctx, "SRTP_AES128_CM_SHA1_80;SRTP_AEAD_AES_128_GCM;SRTP_AEAD_AES_256_GCM")!=0)
		//Set it without GCM
		SSL_CTX_set_tlsext_use_srtp(ssl_ctx, "SRTP_AES128_CM_SHA1_80");


	/* Map for local certificate fingerprints. */

	BIO* certbio;
	X509* cert;

	// Fill the DTLSConnection::availableHashes vector.
	DTLSConnection::availableHashes.push_back(SHA1);
	DTLSConnection::availableHashes.push_back(SHA224);
	DTLSConnection::availableHashes.push_back(SHA256);
	DTLSConnection::availableHashes.push_back(SHA384);
	DTLSConnection::availableHashes.push_back(SHA512);

	// Create BIO for certificate file.
	certbio = BIO_new(BIO_s_file());

	// Read certificate filename.
	if (! BIO_read_filename(certbio, certfile.c_str())) {
		return Error("-DTLSConnection::ClassInit() | Could not read certificate filename [%s]\n",certfile.c_str());
	}

	// Read certificate in X509 format.
	cert = PEM_read_bio_X509(certbio, NULL, 0, NULL);
	if (! cert) {
		return Error("-DTLSConnection::ClassInit() | Could not read X509 certificate from filename [%s]\n",certfile.c_str());
	}

	// Iterate the DTLSConnection::availableHashes.
	for(int i = 0; i < availableHashes.size(); i++) {
		Hash hash = availableHashes[i];
		unsigned int size;
		unsigned char fingerprint[EVP_MAX_MD_SIZE];
		char hex_fingerprint[EVP_MAX_MD_SIZE*3+1] = {0};

		switch (hash) {
			case SHA1:
				X509_digest(cert, EVP_sha1(), fingerprint, &size);
				break;
			case SHA224:
				X509_digest(cert, EVP_sha224(), fingerprint, &size);
				break;
			case SHA256:
				X509_digest(cert, EVP_sha256(), fingerprint, &size);
				break;
			case SHA384:
				X509_digest(cert, EVP_sha384(), fingerprint, &size);
				break;
			case SHA512:
				X509_digest(cert, EVP_sha512(), fingerprint, &size);
				break;
		}

		// Check size.
		if (! size) {
			return Error("-DTLSConnection::ClassInit() | Wrong X509 certificate size\n");
		}

		// Convert to hex format.
		for (int j = 0; j < size; j++)
			sprintf(hex_fingerprint+j*3, "%.2X:", fingerprint[j]);

		// End string.
		hex_fingerprint[size*3-1] = 0;

		// Store in the map.
		DTLSConnection::localFingerPrints[hash] = std::string(hex_fingerprint);
	}

	// Free BIO.
	BIO_free_all(certbio);

	// OK, we have DTLS.
	DTLSConnection::hasDTLS = true;
#else
	return Error("DTLS is not supported. OpenSSL version required is >= 1.0.1 for DTLS.\n");
#endif
	return 1;
}

std::string DTLSConnection::GetCertificateFingerPrint(Hash hash)
{
	return DTLSConnection::localFingerPrints[hash];
}


/* Instance methods. */

DTLSConnection::DTLSConnection(Listener& listener) : listener(listener)
{
	//Set default values
	rekey		  	     = 0;
	dtls_setup		     = SETUP_PASSIVE;
	connection		     = CONNECTION_NEW;
	ssl			     = NULL;		/*!< SSL session */
	read_bio		     = NULL;		/*!< Memory buffer for reading */
	write_bio		     = NULL;		/*!< Memory buffer for writing */
	inited			     = false;
	remoteHash		     = UNKNOWN_HASH;
	appListener		     = NULL;
	//Reset remote fingerprint
	memset(remoteFingerprint,0,EVP_MAX_MD_SIZE);
}

DTLSConnection::~DTLSConnection()
{
	End();
}

int DTLSConnection::Init()
{
	Log(">DTLSConnection::Init()\n");

	if (! DTLSConnection::hasDTLS)
		return Error("-DTLSConnection::Init() | no DTLS\n");

	if (!(ssl = SSL_new(ssl_ctx)))
		return Error("-DTLSConnection::Init() | Failed to allocate memory for SSL context on \n");

	SSL_set_ex_data(ssl, 0, this);

	if (!(read_bio = BIO_new(BIO_s_mem())))
	{
		SSL_free(ssl);
		return Error("-DTLSConnection::Init() | Failed to allocate memory for inbound SSL traffic on \n");
	}
	BIO_set_mem_eof_return(read_bio, -1);

	if (!(write_bio = BIO_new(BIO_s_mem())))
	{
		BIO_free(read_bio);
		SSL_free(ssl);
		return Error("-DTLSConnection::Init() | Failed to allocate memory for outbound SSL traffic on \n");
	}
	BIO_set_mem_eof_return(write_bio, -1);

	SSL_set_bio(ssl, read_bio, write_bio);

	switch(dtls_setup)
	{
		case SETUP_ACTIVE:
			Debug("-DTLSConnection::Init() | we are SETUP_ACTIVE\n");
			SSL_set_connect_state(ssl);
			break;
		case SETUP_PASSIVE:
			Debug("-DTLSConnection::Init() | we are SETUP_PASSIVE\n");
			SSL_set_accept_state(ssl);
			break;
	}

	//New connection
	connection = CONNECTION_NEW;

	//Start handshake
	SSL_do_handshake(ssl);

	//Now we are ready to read and write DTLS packets.
	inited = true;

	Log("<DTLSConnection::Init()\n");

	return 1;
}

void DTLSConnection::End()
{
	Log("-DTLSConnection::End()\n");

	// NOTE: Don't use BIO_free() for write_bio and read_bio as they are
	// automatically freed by SSL_free().

	if (ssl) {
		SSL_free(ssl);
		ssl = NULL;
	}
}

void DTLSConnection::Reset()
{
	Log("-DTLSConnection::Reset()\n");

	/* If the SSL session is not yet finalized don't bother resetting */
	if (ssl == NULL ||!SSL_is_init_finished(ssl))
		return;

	SSL_shutdown(ssl);

	connection = CONNECTION_NEW;
}

void DTLSConnection::SetRemoteSetup(Setup remote)
{
	Log("-DTLSConnection::SetRemoteSetup() | [remote:%d]\n", remote);

	if (! DTLSConnection::hasDTLS) {
		Error("no DTLS\n");
		return;
	}

	Setup old = dtls_setup;

	switch (remote)
	{
		case SETUP_ACTIVE:
			dtls_setup = SETUP_PASSIVE;
			break;
		case SETUP_PASSIVE:
			dtls_setup = SETUP_ACTIVE;
			break;
		case SETUP_ACTPASS:
			/* We can't respond to an actpass setup with actpass ourselves... so respond with active, as we can initiate connections */
			if (dtls_setup == SETUP_ACTPASS)
				dtls_setup = SETUP_ACTIVE;
			break;
		case SETUP_HOLDCONN:
			dtls_setup = SETUP_HOLDCONN;
			break;
		default:
			/* This should never occur... if it does exit early as we don't know what state things are in */
			return;
	}

	/* If the setup state did not change we go on as if nothing happened */
	if (old == dtls_setup || !ssl)
		return;

	switch (dtls_setup)
	{
		case SETUP_ACTIVE:
			Debug("-DTLSConnection::SetRemoteSetup() | we are SETUP_ACTIVE\n");
			SSL_set_connect_state(ssl);
			break;
		case SETUP_PASSIVE:
			Debug("-DTLSConnection::SetRemoteSetup() | we are SETUP_PASSIVE\n");
			SSL_set_accept_state(ssl);
			break;
		case SETUP_HOLDCONN:
		default:
			return;
	}

	return;
}

void DTLSConnection::SetRemoteFingerprint(Hash hash, const char *fingerprint)
{
	if (! DTLSConnection::hasDTLS) {
		Error("-DTLSConnection::SetRemoteFingerprint() | no DTLS\n");
		return;
	}

	remoteHash = hash;
	char* tmp = strdup(fingerprint);
	char* value;
	int pos = 0;

	while ((value = strsep(&tmp, ":")) && (pos != (EVP_MAX_MD_SIZE - 1)))
		sscanf(value, "%02x", (unsigned int*)&remoteFingerprint[pos++]);

	free(tmp);
}

int DTLSConnection::Read(BYTE* data,int size)
{
	if (! DTLSConnection::hasDTLS) {
		Error("-DTLSConnection::Read() | no DTLS\n");
		return 0;
	}

	if (! inited)
		return Error("-DTLSConnection::Read() | SSL not yet ready\n");

	if (BIO_ctrl_pending(write_bio))
		return BIO_read(write_bio, data, size);

	return 0;
}

inline
void DTLSConnection::onSSLInfo(int where, int ret)
{
	Debug("[where:%d, ret:%d] | SSL status: %s | handshake done: %s\n", where, ret, SSL_state_string_long(this->ssl), SSL_is_init_finished(this->ssl) ? "yes" : "no");

	if (where & SSL_CB_HANDSHAKE_START)
	{
		Debug("-DTLSConnection::onSSLInfo() | DTLS handshake starts\n");
	}
	else if (where & SSL_CB_HANDSHAKE_DONE)
	{
		Log("-DTLSConnection::onSSLInfo() | DTLS handshake done\n");

		/* Any further connections will be existing since this is now established */
		connection = CONNECTION_EXISTING;

		//L'authentification du pair d'abord, et sans condition : l'empreinte du
		//certificat qu'on vient de voir contre celle qu'a annoncée la
		//signalisation. Elle échoue -> aucune clé n'est posée, donc rien ne se
		//déchiffre : la jambe est fermée.
		if (!VerifyRemoteFingerprint())
			return;

		//Les clés SRTP ensuite, et seulement si le pair a bien négocié
		//l'extension use_srtp (RFC 5764) : une jambe qui ne porte que des données
		//applicatives — un data channel — ne sélectionne aucun profil, et il n'y
		//a alors aucune clé à exporter. Le nom du profil est journalisé : si une
		//jambe RTP arrivait ici sans profil, la ligne le dirait tout de suite.
		SRTP_PROTECTION_PROFILE* profile = SSL_get_selected_srtp_profile(ssl);

		if (!profile)
		{
			Log("-DTLSConnection::onSSLInfo() | no SRTP profile negotiated, application data only\n");
			return;
		}

		Debug("-DTLSConnection::onSSLInfo() | SRTP profile [%s]\n",profile->name);

		/* Use the keying material to set up key/salt information */
		SetupSRTP();
	}

	// NOTE: checking SSL_get_shutdown(this->ssl) & SSL_RECEIVED_SHUTDOWN here upon
	// receipt of a close alert does not work.
}

int DTLSConnection::Renegotiate()
{
	SSL_renegotiate(ssl);
	SSL_do_handshake(ssl);

	rekeyid = -1;
	return 1;
}

bool DTLSConnection::GetTimeout(struct timeval* tv)
{
	//Pas de session ou pas de timer armé -> rien à attendre
	if (!ssl)
		return false;
	//DTLSv1_get_timeout renvoie 1 si un délai de retransmission est en cours
	return DTLSv1_get_timeout(ssl, tv) == 1;
}

int DTLSConnection::HandleTimeout()
{
	if (!ssl)
		return 0;
	//DTLSv1_handle_timeout : -1 sur échec (trop de retransmissions), 0 si le délai
	//n'est pas encore écoulé, 1 si un flight a été re-mis en file (à flusher).
	return DTLSv1_handle_timeout(ssl);
}

int DTLSConnection::VerifyRemoteFingerprint()
{
	if (! DTLSConnection::hasDTLS)
		return Error("-DTLSConnection::VerifyRemoteFingerprint() | no DTLS\n");

	X509* certificate;
	unsigned char fingerprint[EVP_MAX_MD_SIZE];
	unsigned int size=0;
	const EVP_MD* hash_function;
	std::string hash_str;

	if (!(certificate = SSL_get_peer_certificate(ssl)))
		return Error("-DTLSConnection::VerifyRemoteFingerprint() | no certificate was provided by the peer\n");

	switch (remoteHash)
	{
		case SHA1:
			hash_function = EVP_sha1();
			hash_str = "SHA-1";
			break;
		case SHA224:
			hash_function = EVP_sha224();
			hash_str = "SHA-224";
			break;
		case SHA256:
			hash_function = EVP_sha256();
			hash_str = "SHA-256";
			break;
		case SHA384:
			hash_function = EVP_sha384();
			hash_str = "SHA-384";
			break;
		case SHA512:
			hash_function = EVP_sha512();
			hash_str = "SHA-512";
			break;
		default:
			X509_free(certificate);
			return Error("-DTLSConnection::VerifyRemoteFingerprint() | unknown remote hash, cannot verify remote fingerprint\n");
	}

	if (!X509_digest(certificate, hash_function, fingerprint, &size) || !size || memcmp(fingerprint, remoteFingerprint, size))
	{
		X509_free(certificate);
		return Error("-DTLSConnection::VerifyRemoteFingerprint() | fingerprint in remote SDP does not match that of peer certificate (hash %s)\n", hash_str.c_str());
	}

	Debug("-DTLSConnection::VerifyRemoteFingerprint() | fingerprint in remote SDP matches that of peer certificate (hash %s)\n", hash_str.c_str());
	X509_free(certificate);

	return 1;
}

int DTLSConnection::SetupSRTP()
{
/* This is defined in openssl/srtp.h */
#ifdef HEADER_D1_SRTP_H
	if (! DTLSConnection::hasDTLS)
		return Error("-DTLSConnection::SetupSRTP() | no DTLS\n");

	BYTE material[SRTP_MASTER_LENGTH * 2];
	BYTE localMasterKey[SRTP_MASTER_LENGTH];
	BYTE remoteMasterKey[SRTP_MASTER_LENGTH];
	BYTE *local_key, *local_salt, *remote_key, *remote_salt;

	if (! SSL_export_keying_material(ssl, material, SRTP_MASTER_LENGTH * 2, "EXTRACTOR-dtls_srtp", 19, NULL, 0, 0))
		return Error("-DTLSConnection::SetupSRTP() | Unable to extract SRTP keying material from DTLS-SRTP negotiation on RTP instance \n");

	/* Whether we are acting as a server or client determines where the keys/salts are */

	if (dtls_setup == SETUP_ACTIVE)
	{
		local_key = material;
		remote_key = local_key + SRTP_MASTER_KEY_LENGTH;
		local_salt = remote_key + SRTP_MASTER_KEY_LENGTH;
		remote_salt = local_salt + SRTP_MASTER_SALT_LENGTH


;
	} else	{
		remote_key = material;
		local_key = remote_key + SRTP_MASTER_KEY_LENGTH;
		remote_salt = local_key + SRTP_MASTER_KEY_LENGTH;
		local_salt = remote_salt + SRTP_MASTER_SALT_LENGTH


;
	}

	//Create local master key
	memcpy(localMasterKey,local_key,SRTP_MASTER_KEY_LENGTH);
	memcpy(localMasterKey+SRTP_MASTER_KEY_LENGTH,local_salt,SRTP_MASTER_SALT_LENGTH)
;
	//Create remote master key
	memcpy(remoteMasterKey,remote_key,SRTP_MASTER_KEY_LENGTH);
	memcpy(remoteMasterKey+SRTP_MASTER_KEY_LENGTH,remote_salt,SRTP_MASTER_SALT_LENGTH)
;

	//Fire event
	listener.onDTLSSetup(suite,localMasterKey,SRTP_MASTER_LENGTH,remoteMasterKey,SRTP_MASTER_LENGTH);

	return 1;
#else
	return Error("DTLS is not supported. OpenSSL version required is >= 1.0.1 for DTLS.\n");
#endif
}

//Chiffre un bloc applicatif. Il attend ensuite dans write_bio : c'est
//l'appelant qui l'émet, par Read() puis son propre sendto — le DTLS ne connaît
//pas de socket ici.
int DTLSConnection::WriteApplicationData(const BYTE* data,DWORD size)
{
	if (! DTLSConnection::hasDTLS)
		return Error("-DTLSConnection::WriteApplicationData() | no DTLS\n");

	if (! inited)
		return Error("-DTLSConnection::WriteApplicationData() | SSL not yet ready\n");

	//Avant la fin du handshake il n'y a pas de canal applicatif : le pair n'est
	//pas encore authentifié, et SSL_write pousserait dans le flight en cours.
	if (! SSL_is_init_finished(ssl))
		return Error("-DTLSConnection::WriteApplicationData() | handshake not finished\n");

	int len = SSL_write(ssl, data, (int) size);

	if (len <= 0)
		return Error("-DTLSConnection::WriteApplicationData() | SSL_write failed [err:%d]\n",
				SSL_get_error(ssl,len));

	return len;
}

int DTLSConnection::Write(BYTE *buffer,int size)
{
	if (! DTLSConnection::hasDTLS)
		return Error("-DTLSConnection::Write() | no DTLS\n");

	if (! inited) {
		return Error("-DTLSConnection::Write() | SSL not yet ready\n");
	}

	BIO_write(read_bio, buffer, size);

	//Un record entrant peut libérer plusieurs blocs applicatifs, et SSL_read n'en
	//rend qu'un par appel : on boucle. Le tampon est LOCAL — celui de l'appelant
	//porte encore le datagramme entrant, dont il se sert au retour, et SSL_read
	//écrivait dedans. Sa taille est celle d'un record TLS en clair : un record
	//plus grand serait coupé en deux appels, et le pair SCTP recevrait deux
	//moitiés de datagramme.
	BYTE app[MAX_DTLS_APPLICATION_DATA];
	int len;

	while ((len = SSL_read(ssl, app, sizeof(app))) > 0)
	{
		if (appListener)
			appListener->onDTLSApplicationData(app,(DWORD)len);
	}

	// Check if the peer sent close alert or a fatal error happened.
	if (SSL_get_shutdown(ssl) & SSL_RECEIVED_SHUTDOWN) {
		Debug("-DTLSConnection::Write() | SSL_RECEIVED_SHUTDOWN on instance '%p', resetting SSL\n", this);
		int err = SSL_clear(ssl);
		if (err == 0)
			Error("-DTLSConnection::Write() | SSL_clear() failed: %s", ERR_error_string(ERR_get_error(), NULL));

		return 0;
	}

	return 1;
}

int DTLSConnection::CheckPending()
{
	if (!write_bio)
		return 0;

	return BIO_ctrl_pending(write_bio);
}
