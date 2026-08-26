/**
 * dtlsfixture.h — un certificat DTLS jetable, pour les tests qui en ont besoin.
 *
 * `DTLSConnection::ClassInit()` installe un contexte SSL *statique*, lu une fois
 * pour tout le binaire, depuis des fichiers PEM. Un certificat de test versionné
 * n'apporterait rien et ferait sonner les scanners de secrets : il est donc
 * généré à l'exécution, dans un répertoire temporaire.
 *
 * `Ensure()` est une méthode inline : son `static` local est UNE seule entité
 * pour tout le programme, quel que soit le nombre de fichiers de test qui
 * l'incluent. C'est ce qui garantit un seul ClassInit.
 */
#ifndef DTLSFIXTURE_H
#define DTLSFIXTURE_H

#include <openssl/pem.h>
#include <openssl/x509.h>
#include <stdio.h>
#include <string>
#include <unistd.h>

#include "dtls.h"

class DTLSTestCertificate
{
public:
	//true si le DTLS du binaire est prêt à servir. Idempotent.
	static bool Ensure();

	//L'empreinte SHA-256 du certificat, celle qu'un pair annoncerait.
	static std::string FingerPrint()
	{
		return DTLSConnection::GetCertificateFingerPrint(DTLSConnection::SHA256);
	}

private:
	static bool WriteSelfSigned(const std::string& crtPath,const std::string& keyPath);
};

inline bool DTLSTestCertificate::WriteSelfSigned(const std::string& crtPath,const std::string& keyPath)
{
	EVP_PKEY* pkey = EVP_RSA_gen(2048);
	if (!pkey)
		return false;

	X509* cert = X509_new();
	if (!cert)
	{
		EVP_PKEY_free(pkey);
		return false;
	}

	X509_set_version(cert, 2);
	ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
	X509_gmtime_adj(X509_getm_notBefore(cert), 0);
	X509_gmtime_adj(X509_getm_notAfter(cert), 3600);
	X509_set_pubkey(cert, pkey);

	X509_NAME* name = X509_get_subject_name(cert);
	X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
				   (const unsigned char*)"mcu-dtls-test", -1, -1, 0);
	X509_set_issuer_name(cert, name);

	bool ok = X509_sign(cert, pkey, EVP_sha256()) > 0;

	if (ok)
	{
		FILE* f = fopen(crtPath.c_str(), "wb");
		ok = f && PEM_write_X509(f, cert);
		if (f) fclose(f);
	}

	if (ok)
	{
		FILE* f = fopen(keyPath.c_str(), "wb");
		ok = f && PEM_write_PrivateKey(f, pkey, NULL, NULL, 0, NULL, NULL);
		if (f) fclose(f);
	}

	X509_free(cert);
	EVP_PKEY_free(pkey);
	return ok;
}

inline bool DTLSTestCertificate::Ensure()
{
	static bool tried  = false;
	static bool inited = false;

	if (tried)
		return inited;

	tried = true;

	char dir[] = "/tmp/mcu-dtls-test-XXXXXX";
	if (!mkdtemp(dir))
		return false;

	//Statiques : ClassInit ne copie pas les chemins, il les relit plus tard.
	static std::string crt = std::string(dir) + "/test.crt";
	static std::string key = std::string(dir) + "/test.key";

	if (!WriteSelfSigned(crt, key))
		return false;

	DTLSConnection::SetCertificate(crt.c_str(), key.c_str());
	inited = DTLSConnection::ClassInit() == 1;
	return inited;
}

#endif /* DTLSFIXTURE_H */
