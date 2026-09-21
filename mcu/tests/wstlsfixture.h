/**
 * wstlsfixture.h — une autorité et un certificat jetables pour les tests wss://.
 *
 * Même contrainte que dtlsfixture.h : `WebSocketTlsTransport::ClassInit()` installe
 * un contexte SSL *statique*, lu une fois pour tout le binaire depuis des fichiers
 * PEM. Un certificat versionné n'apporterait rien et ferait sonner les scanners de
 * secrets : il est généré à l'exécution, dans un répertoire temporaire.
 *
 * Ce que ce fixture a de plus que celui du DTLS : il fabrique une AUTORITÉ, puis un
 * certificat serveur qu'elle signe, portant `IP:127.0.0.1`, `IP:::1` et
 * `DNS:localhost` en SAN. C'est ce qui permet d'exercer la vérification cliente
 * dans les DEUX sens — elle accepte avec l'autorité, elle refuse sans.
 *
 * `Ensure()` est inline : son `static` local est UNE seule entité pour tout le
 * programme, quel que soit le nombre de fichiers de test qui l'incluent.
 */
#ifndef WSTLSFIXTURE_H
#define WSTLSFIXTURE_H

#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <stdio.h>
#include <string>
#include <unistd.h>

#include "websockettransport.h"

class WsTlsTestCertificate
{
public:
	//true si le contexte TLS serveur du binaire est prêt à servir. Idempotent.
	static bool Ensure();

	//L'autorité qui a signé le certificat serveur : ce que `--websocket-client-ca`
	//recevrait en production.
	static const std::string& CaFile()	{ return Storage().ca;	}
	//Ce qu'un WebSocketServer::SetSecure attend
	static const std::string& CertFile()	{ return Storage().crt;	}
	static const std::string& KeyFile()	{ return Storage().key;	}

private:
	struct Paths
	{
		std::string ca;		//certificat de l'autorité (PEM)
		std::string crt;	//certificat serveur
		std::string key;	//clé privée serveur
	};

	//Un seul `static` pour tout le binaire, comme Ensure()
	static Paths& Storage()			{ static Paths paths; return paths; }

	static bool Generate(const Paths& paths);
	static bool AddExtension(X509* cert,X509* issuer,int nid,const char* value);
};

inline bool WsTlsTestCertificate::AddExtension(X509* cert,X509* issuer,int nid,const char* value)
{
	X509V3_CTX ctx;
	X509V3_set_ctx_nodb(&ctx);
	X509V3_set_ctx(&ctx, issuer, cert, NULL, NULL, 0);

	X509_EXTENSION* ext = X509V3_EXT_conf_nid(NULL, &ctx, nid, value);
	if (!ext)
		return false;

	const bool ok = X509_add_ext(cert, ext, -1) == 1;
	X509_EXTENSION_free(ext);
	return ok;
}

inline bool WsTlsTestCertificate::Generate(const Paths& paths)
{
	EVP_PKEY* caKey		= NULL;
	EVP_PKEY* srvKey	= NULL;
	X509*	  caCert	= NULL;
	X509*	  srvCert	= NULL;
	bool	  ok		= false;

	caKey  = EVP_RSA_gen(2048);
	srvKey = EVP_RSA_gen(2048);
	caCert = X509_new();
	srvCert= X509_new();
	if (!caKey || !srvKey || !caCert || !srvCert)
		goto end;

	//---- L'autorité, auto-signée -------------------------------------------
	X509_set_version(caCert, 2);
	ASN1_INTEGER_set(X509_get_serialNumber(caCert), 1);
	X509_gmtime_adj(X509_getm_notBefore(caCert), 0);
	X509_gmtime_adj(X509_getm_notAfter(caCert), 3600);
	X509_set_pubkey(caCert, caKey);
	X509_NAME_add_entry_by_txt(X509_get_subject_name(caCert), "CN", MBSTRING_ASC,
				   (const unsigned char*)"mcu-ws-test-ca", -1, -1, 0);
	X509_set_issuer_name(caCert, X509_get_subject_name(caCert));
	//Sans basicConstraints CA:TRUE, OpenSSL refuse de bâtir la chaîne
	if (!AddExtension(caCert, caCert, NID_basic_constraints, "critical,CA:TRUE"))
		goto end;
	if (X509_sign(caCert, caKey, EVP_sha256()) <= 0)
		goto end;

	//---- Le serveur, signé par l'autorité ----------------------------------
	X509_set_version(srvCert, 2);
	ASN1_INTEGER_set(X509_get_serialNumber(srvCert), 2);
	X509_gmtime_adj(X509_getm_notBefore(srvCert), 0);
	X509_gmtime_adj(X509_getm_notAfter(srvCert), 3600);
	X509_set_pubkey(srvCert, srvKey);
	X509_NAME_add_entry_by_txt(X509_get_subject_name(srvCert), "CN", MBSTRING_ASC,
				   (const unsigned char*)"mcu-ws-test", -1, -1, 0);
	X509_set_issuer_name(srvCert, X509_get_subject_name(caCert));
	//Un client qui appelle une ADRESSE compare aux SAN de type iPAddress, jamais
	//au CN : sans ces SAN, la vérification échouerait quoi qu'il arrive.
	if (!AddExtension(srvCert, caCert, NID_subject_alt_name, "IP:127.0.0.1,IP:::1,DNS:localhost"))
		goto end;
	if (X509_sign(srvCert, caKey, EVP_sha256()) <= 0)
		goto end;

	//---- Écriture --------------------------------------------------------
	{
		FILE* f = fopen(paths.ca.c_str(), "wb");
		ok = f && PEM_write_X509(f, caCert);
		if (f) fclose(f);
		if (!ok) goto end;

		f = fopen(paths.crt.c_str(), "wb");
		ok = f && PEM_write_X509(f, srvCert);
		if (f) fclose(f);
		if (!ok) goto end;

		f = fopen(paths.key.c_str(), "wb");
		ok = f && PEM_write_PrivateKey(f, srvKey, NULL, NULL, 0, NULL, NULL);
		if (f) fclose(f);
	}

end:
	if (srvCert) X509_free(srvCert);
	if (caCert)  X509_free(caCert);
	if (srvKey)  EVP_PKEY_free(srvKey);
	if (caKey)   EVP_PKEY_free(caKey);
	return ok;
}

inline bool WsTlsTestCertificate::Ensure()
{
	static bool tried  = false;
	static bool inited = false;

	if (tried)
		return inited;

	tried = true;

	char dir[] = "/tmp/mcu-wstls-test-XXXXXX";
	if (!mkdtemp(dir))
		return false;

	Paths& paths = Storage();
	paths.ca  = std::string(dir) + "/ca.crt";
	paths.crt = std::string(dir) + "/server.crt";
	paths.key = std::string(dir) + "/server.key";

	if (!Generate(paths))
		return false;

	inited = WebSocketTlsTransport::ClassInit(paths.crt, paths.key);
	return inited;
}

#endif /* WSTLSFIXTURE_H */
