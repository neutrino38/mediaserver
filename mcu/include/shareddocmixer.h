/*
 * File:   shareddocmixer.h
 *
 * Le partage de document d'une conférence : la parole BFCP d'un côté, le flux
 * vidéo SLIDES de l'autre.
 *
 * BFCP vient de la pile interne (`mcu/src/bfcp`). Conception et arbitrages :
 * docs/conception/BFCP-INTERNE/SPEC.md, état : docs/reference/bfcp.md.
 */


#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include "config.h"
#include "media.h"
#include "video.h"
#include "participant.h"
#include "bfcp/BFCPFloorControlServer.h"
#include "bfcp/BFCPTcpTransport.h"
#include "bfcp/BFCPUdpTransport.h"

#ifndef SHAREDDOCMIXER_H
#define	SHAREDDOCMIXER_H

class MultiConf;

class SharedDocMixer  :
 public VideoOutput,
 public BFCPFloorControlServer::Listener
{
public:
	// Une conférence, un floor, celui du document partagé.
	static const int FloorId = 1;

public:
	SharedDocMixer();
	~SharedDocMixer();
	int Init(VideoOutput *output, const PictPtr& logo,MultiConf *conf);
	int ShareSecondaryStream(ParticipantPtr part);

	int initDocSharing( ParticipantPtr part,char *sendIp,int sendPort);
	int AcceptDocSharingRequest(int confId, ParticipantPtr part);
	int RefuseDocSharingRequest(int confId, ParticipantPtr part);

	int StopSharing();
	int StopSharing(ParticipantPtr part);
	virtual int NextFrame(PictPtr pic);
	virtual int SetVideoSize(int width,int height) ;

	int addParticipant(int confId, ParticipantPtr part,Participant::DocSharingMode docSharingMode, MediaFrame::MediaProtocol proto = MediaFrame::TCP);
	int removeParticipant(ParticipantPtr part);
	int getServerPort(ParticipantPtr part);
	bool StopBfcpServer();
	int		GetSharedMosaic(){return sharedMosaic;}
	void	SetSharedMosaic(int mosaicId);

	int End();

private:
	// BFCPFloorControlServer::Listener — appelés depuis le thread du réacteur
	// BFCP. Ce qu'ils font doit rester court : ils battent toutes les jambes
	// BFCP du processus (docs/reference/threads-rtp.md).
	virtual void onFloorRequest(int floorRequestId, int userId, int beneficiaryId, std::set<int> floorIds);
	virtual void onFloorGranted(int floorRequestId, int beneficiaryId, std::set<int> floorIds);
	virtual void onFloorReleased(int floorRequestId, int beneficiaryId, std::set<int> floorIds);
	virtual void onUserConnected(int userId);

	bool StartBfcpServer(int confId, ParticipantPtr part);
	// Le floorRequestId en cours de ce participant, 0 s'il n'en a pas.
	int GetFloorRequestId(int partId);
	ParticipantPtr GetParticipant(int partId);
	// Défait le partage en cours, SANS toucher à BFCP : c'est la révocation
	// qui mène ici, pas l'inverse.
	void TearDownSharing();

private:
	PictPtr			logo;
	std::weak_ptr<Participant> part;
	VideoOutput*	output;
	MultiConf* 		conf;
	int				confId;
	int				sharedMosaic;

	std::mutex	mutex;

	// BFCP : un serveur de contrôle de parole par conférence, une écoute TCP
	// pour elle, et une socket UDP par participant qui en veut une.
	std::unique_ptr<BFCPFloorControlServer>	bfcpServer;
	std::unique_ptr<BFCPTcpListener>	tcpListener;
	std::map<int, std::unique_ptr<BFCPUdpEndpoint> > udpEndpoints;
	// Un seul entier par participant : la demande de parole en cours.
	std::map<int, int>			floorRequests;
	std::map<int, std::weak_ptr<Participant> > participants;
};

#endif	/* SHAREDDOCMIXER_H */
