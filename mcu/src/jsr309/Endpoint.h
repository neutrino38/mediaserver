/* 
 * File:   Endpoint.h
 * Author: Sergio
 *
 * Created on 7 de septiembre de 2011, 0:59
 */

#ifndef ENDPOINT_H
#define	ENDPOINT_H

#include <memory>
#include <map>
#include <string>
#include "Joinable.h"
#include "websockets.h"
#include "RTPMultiplexer.h"
#include "remoterateestimator.h"
#include "medkit/negotiator.h"

class RTPEndpoint;

class Endpoint
{
public:
	//Profil d'adressage demandé par le contrôleur pour cette jambe (§14 de
	//ipv6.md) : décide de l'adresse liée — donc de l'interface — et de l'adresse
	//annoncée dans les candidats. À poser AVANT StartReceiving/StartSending.
	bool SetAddressProfile(MediaFrame::Type media, const char* profile, std::string& error,
	                       MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN);

	class Port : public RTPMultiplexer
	{
	public:
	    virtual ~Port();
	    MediaFrame::Type GetMedia() { return type; }
	    int Detach();
	    int Attach(const std::shared_ptr<Joinable> & join);
		int SwitchJoin(std::shared_ptr<Port> oldPort);
		
		int GetLocalMediaPort();
		char* GetLocalMediaHost();
		
	    MediaFrame::MediaProtocol GetTransport() const { return proto; }
	    bool IsReceiving() const override { return receiving; }

	    // --- Négociation de codecs (phase 4 nego_fmtp) ---
	    // Route les propriétés codec.* (préfixe « codec. » retiré) vers le stockage
	    // local consommé par le négociateur. Les clés transport sont ignorées ici
	    // (elles continuent vers RTPSession, qui ignore de son côté codec.*).
	    void StoreCodecProperties(const Properties& properties);
	    // Filtre la map proposée selon les codecs réellement supportés (décision D)
	    // et dérive le fmtp local (params seuls) SANS ouvrir de codec. Remplit
	    // acceptedOut (sous-ensemble accepté) et mémorise le résultat (fmtp par PT
	    // + effectiveProps) pour le retour XML-RPC et l'encodeur (ph.5).
	    // `offerFmtp` (P8a, optionnel) : le fmtp de l'offre PAR PAYLOAD TYPE, posé
	    // en clés "pt.<pt>.fmtp" au-dessus du canal par codec — deux PT d'un même
	    // codec repartent alors chacun avec son propre profil (RFC 6184 §8.2.2).
	    void NegotiateReceiving(const RTPMap& proposed, RTPMap& acceptedOut,
	                            const std::map<int,std::string>* offerFmtp = NULL);
	    // fmtp de la dernière négociation : PT -> paramètres (params seuls). TOUT PT
	    // accepté est présent ; un codec sans fmtp a une valeur "" (chaîne vide). Un
	    // PT absent a été filtré (non supporté). Vide tant qu'aucune négociation n'a eu lieu.
	    const std::map<int,std::string>& GetNegotiatedFmtp() const { return negotiatedFmtp; }

	    // --- Câblage émission (phase 5 nego_fmtp §6.3) ---
	    // Le PT d'émission est connu à StartSending : affine les bornes par codec
	    // (l'entrée de negotiatedProps du PT réellement émis l'emporte sur le
	    // premier-PT-du-codec retenu à la négociation) et les pousse au producteur.
	    void RefineNegotiatedForSending(const RTPMap& sendingMap);
	    // Pousse les bornes négociées (par code codec) au Joinable attaché — le
	    // producteur du flux que ce port émet. Appelé à la fin de la négociation,
	    // à l'attach et à StartSending, pour être robuste aux trois ordres.
	    void PushNegotiatedProps();

	    virtual int Init() = 0;
	    virtual int End() = 0;
	    
	    // Overriden if the port needs a thread to run
	    virtual int StartReceiving() 
	    { 
			receiving = true;
	        return 1; 
	    }
	    
	    virtual int StopReceiving()
	    { 
	        receiving = false;
	        return 1;
	    }
	    
	    virtual int StartSending()
	    { 
			sending = true;
	        return 1;
	    }
	    
	    virtual int StopSending()
	    {
	        sending = false;
		return 1;
	    }


	protected:
	    // Lien retour NON possédant vers la source : weak_ptr → lock() au site
	    // d'usage. Si la source est détruite, lock() échoue et le Detach ultérieur
	    // ne déréférence pas d'objet libéré (C-13, lien A — remplace onJoinableEnded).
	    std::weak_ptr<Joinable> joined;
	    MediaFrame::Type type;
		MediaFrame::MediaProtocol proto;
	    bool sending;
	    bool receiving;
	    bool portinited;
	    MediaStatistics stats;
	    // Propriétés locales de codec (clés « <codec>.<param> », préfixe codec.
	    // retiré) alimentées par EndpointSetRTPProperties, lues par le négociateur.
	    Properties codecProperties;
	    // Résultat de la dernière négociation : PT -> params fmtp (décision E).
	    std::map<int,std::string> negotiatedFmtp;
	    // effectiveProps par PT retenu, pour brancher l'encodeur d'émission (phase 5).
	    std::map<int,Properties> negotiatedProps;
	    // PT accepté -> code codec (la clé de résolution émission : le PT émis est
	    // choisi par identité de codec, pas par égalité de PT).
	    std::map<int,int> negotiatedCodecs;
	    // Bornes d'émission par code codec : le premier PT accepté du codec (l'ordre
	    // de l'offre, donc le primaire du contrôleur), affiné par StartSending.
	    std::map<int,Properties> negotiatedByCodec;
	    // Protected constructir
	    Port( MediaFrame::Type type, MediaFrame::MediaProtocol transp) : RTPMultiplexer()
	    {
			this->type = type;
			this->proto = transp;
			sending = false;
			receiving = false;
			portinited = false;
	    }
		
		
	};

        typedef std::map<std::string,MediaStatistics> Statistics;

	Endpoint(std::wstring name,bool audioSupported,bool videoSupported,bool textSupport);
	~Endpoint();
	
	//Methods
	int Init();
	int End();
	
	//Endpoint  functionality
	int StartSending(MediaFrame::Type media,char *sendIp,int sendPort,RTPMap& rtpMap, MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN);
	//Trickle ICE Niveau 1 (gap 1) : transmet un candidat SDP au flux RTP concerné.
	int AddICECandidate(MediaFrame::Type media,const char* candidate, MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN);
	//Watchdog d'inactivité RTP (gap 5) : arme (timeoutMs>0) ou désarme (0) le flux.
	int ArmRTPTimeout(MediaFrame::Type media,DWORD timeoutMs, MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN);
	int StopSending(MediaFrame::Type media, MediaFrame::MediaRole role =  MediaFrame::VIDEO_MAIN);
	//P8a : `offerFmtp` (optionnel) porte le fmtp de l'offre par payload type, relayé
	//au négociateur — NULL = pas d'entrée distante, négociation contre notre config.
	int StartReceiving(MediaFrame::Type media,RTPMap& rtpMap, MediaFrame::MediaRole role =  MediaFrame::VIDEO_MAIN,
	                   const std::map<int,std::string>* offerFmtp = NULL);
	int StopReceiving(MediaFrame::Type media, MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN);
	int RequestUpdate(MediaFrame::Type media, MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN);

	int SetLocalCryptoSDES(MediaFrame::Type media,const char* suite, const char* key64, MediaFrame::MediaRole role =  MediaFrame::VIDEO_MAIN);
	int SetRemoteCryptoSDES(MediaFrame::Type media,const char* suite, const char* key64, MediaFrame::MediaRole role =  MediaFrame::VIDEO_MAIN);
	int SetRemoteCryptoDTLS(MediaFrame::Type media,const char *setup,const char *hash,const char *fingerprint, MediaFrame::MediaRole role =  MediaFrame::VIDEO_MAIN);
	int SetLocalSTUNCredentials(MediaFrame::Type media,const char* username, const char* pwd, MediaFrame::MediaRole role =  MediaFrame::VIDEO_MAIN);
	int SetRemoteSTUNCredentials(MediaFrame::Type media,const char* username, const char* pwd, MediaFrame::MediaRole role =  MediaFrame::VIDEO_MAIN);
	int SetRTPProperties(MediaFrame::Type media,const Properties& properties, MediaFrame::MediaRole role =  MediaFrame::VIDEO_MAIN);
	int SetRTPTsTransparency(MediaFrame::Type media, bool transparency, MediaFrame::MediaRole role =  MediaFrame::VIDEO_MAIN);

	//Attach
	int Attach(MediaFrame::Type media, MediaFrame::MediaRole role, const std::shared_ptr<Joinable> & join);
	int Detach(MediaFrame::Type media, MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN);

	//Récupère le fmtp négocié (PT -> paramètres) du port media/role (phase 4).
	int GetNegotiatedFmtp(MediaFrame::Type media, MediaFrame::MediaRole role, std::map<int,std::string>& out);
	std::shared_ptr<Joinable> GetJoinable(MediaFrame::Type media, MediaFrame::MediaRole role  = MediaFrame::VIDEO_MAIN);

	std::wstring& GetName() { return name; }
	
	
	char* GetMediaCandidates( MediaFrame::MediaProtocol protocol ,MediaFrame::Type media = MediaFrame::Audio ) ;
	
	// Use other media protocol than RTP
	
	int onNewMediaConnection(MediaFrame::Type media, MediaFrame::MediaRole role,
	                         MediaFrame::MediaProtocol transp, WebSocket * ws );
	
	int ConfigureMediaConnection( MediaFrame::Type media, MediaFrame::MediaRole role, 
				      MediaFrame::MediaProtocol proto, const char * expectedPayload );

	int SetEventContextId( MediaFrame::Type media, MediaFrame::MediaRole role, int ctxId );
    int SetEventHandler( MediaFrame::Type media, MediaFrame::MediaRole role, int sessionId,	JSR309Manager* jsrManager);
	
	const Statistics * GetStatistics();

private:
	inline std::shared_ptr<Port> GetPort(MediaFrame::Type media)
	{
		if ( media >= MediaFrame::Audio && media <= MediaFrame::Text )
		{
			return ports[media];
		}
		else
		{
			return NULL;
		}
	}
	
	inline std::shared_ptr<Port> GetPort(MediaFrame::Type media, MediaFrame::MediaRole role)
	{
		if ( role == MediaFrame::VIDEO_MAIN )
		{
			return GetPort(media);
		}
		else if ( media == MediaFrame::Video && role != MediaFrame::VIDEO_MAIN)
		{
			return ports2[MediaFrame::Video];
		}
		else
		{
			return NULL;
		}
	}

	RTPEndpoint* GetRTPEndpoint(MediaFrame::Type media, MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN);
	
private:
	std::wstring name;
	//RTP sessions
	std::shared_ptr<Port> ports[4];
	std::shared_ptr<Port> ports2[4];
	
	RemoteRateEstimator estimator;
	RemoteRateEstimator estimator2;
	EvenSource eventSource;
        Statistics stats;
};

#endif	/* ENDPOINT_H */

