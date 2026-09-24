/*
 * File:   shareddocmixer.cpp
 *
 * Le partage de document d'une conférence. BFCP vient de la pile interne :
 * docs/conception/BFCP-INTERNE/SPEC.md, docs/reference/bfcp.md.
 */

#include "multiconf.h"
#include "shareddocmixer.h"
#include "log.h"
#include "rtpsession.h"


SharedDocMixer::SharedDocMixer()
{
	output  		= NULL;
	logo			= nullptr;
	conf			= NULL;

	sharedMosaic	= -1;
	confId			= 0;
}

SharedDocMixer::~SharedDocMixer()
{
	StopBfcpServer();

	output  		= NULL;
	logo			= nullptr;
	conf			= NULL;

	sharedMosaic	= -1;
	confId			= 0;
}

int SharedDocMixer::Init(VideoOutput* output, const PictPtr& logo, MultiConf* conf)
{
	//Set output
	this->output 	= output;
	this->logo 		= logo;
	this->conf		= conf;

	part.reset();
	sharedMosaic	= -1;
	confId			= 0;
	return 1;
}

ParticipantPtr SharedDocMixer::GetParticipant(int partId)
{
	std::lock_guard<std::mutex> lock(mutex);

	std::map<int, std::weak_ptr<Participant> >::iterator it = participants.find(partId);
	if (it == participants.end())
		return ParticipantPtr();
	return it->second.lock();
}

int SharedDocMixer::GetFloorRequestId(int partId)
{
	std::lock_guard<std::mutex> lock(mutex);

	std::map<int, int>::iterator it = floorRequests.find(partId);
	return it != floorRequests.end() ? it->second : 0;
}

int SharedDocMixer::ShareSecondaryStream(ParticipantPtr part)
{
	Log(">ShareSecondaryStream\n");

	ParticipantPtr currentPart = this->part.lock();
	if ( currentPart && currentPart != part )
		//Le floor precedent est deja revoque par le serveur : il ne reste que
		//le demontage local.
		TearDownSharing();

	if ( part && part != currentPart)
	{
	    part->SetVideoOutput(this,MediaFrame::VIDEO_SLIDES);
		conf->onRequestDocSharing(part->GetPartId(),L"ACTIVE");
	}

	{
		std::lock_guard<std::mutex> lock(mutex);
		this->part = part;
	}
	return 1;
}


int SharedDocMixer::initDocSharing( ParticipantPtr part,char *sendIp,int sendPort)
{
	if (! part)
		return 0;

	if ( part->GetDocSharingMode() != Participant::BFCP_UDP )
		//En TCP c'est le pair qui se connecte : il n'y a rien a lui envoyer
		//avant de l'avoir entendu.
		return 1;

	BFCPUdpEndpoint* endpoint = NULL;
	{
		std::lock_guard<std::mutex> lock(mutex);
		std::map<int, std::unique_ptr<BFCPUdpEndpoint> >::iterator it = udpEndpoints.find(part->GetPartId());
		if (it != udpEndpoints.end())
			endpoint = it->second.get();
	}

	if (! endpoint)
		return Error("-initDocSharing: participant %d has no UDP endpoint\n", part->GetPartId());

	int err = 0;
	const IPAddress remote = IPAddress::Parse(sendIp, err);
	if (err)
		return Error("-initDocSharing: cannot parse remote address '%s'\n", sendIp);

	Log(">initDocSharing sendIp=%s , sendPort=%i\n",sendIp,sendPort);

	//La socket est AF_INET6 : un pair v4 se joint par une adresse mappee.
	endpoint->SetRemote(remote.ToDualStack((WORD)sendPort));
	//Saluer le premier, comme le faisait SendHello : le pair sait alors que la
	//conference l'attend, sans avoir a parler d'abord.
	endpoint->SendServerHello();

	return 1;
}


int SharedDocMixer::AcceptDocSharingRequest(int confId, ParticipantPtr part)
{
	if (! part || ! bfcpServer)
		return 0;

	const int floorRequestId = GetFloorRequestId(part->GetPartId());
	if (! floorRequestId)
		return Error("-AcceptDocSharingRequest: participant %d has no pending floor request\n", part->GetPartId());

	//L'octroi emet Accepted puis Granted, et c'est onFloorGranted qui branche
	//le flux et previent le controleur.
	return bfcpServer->GrantFloorRequest(floorRequestId) ? 1 : 0;
}

int SharedDocMixer::RefuseDocSharingRequest(int confId, ParticipantPtr part)
{
	if (! part || ! bfcpServer)
		return 0;

	const int floorRequestId = GetFloorRequestId(part->GetPartId());
	if (! floorRequestId)
		return Error("-RefuseDocSharingRequest: participant %d has no pending floor request\n", part->GetPartId());

	if (! bfcpServer->DenyFloorRequest(floorRequestId, "refused by the conference"))
		return 0;

	{
		std::lock_guard<std::mutex> lock(mutex);
		floorRequests.erase(part->GetPartId());
	}

	//Un refus ne relache rien : personne ne tenait la parole, donc le serveur
	//ne previent pas. C'est ici que le controleur l'apprend.
	conf->onRequestDocSharing(part->GetPartId(),L"NONE");
	return 1;
}


//Defait le partage en cours. Ne touche PAS a BFCP : on arrive ici APRES que la
//parole a ete rendue, jamais avant.
void SharedDocMixer::TearDownSharing()
{
	ParticipantPtr p;
	{
		std::lock_guard<std::mutex> lock(mutex);
		p = this->part.lock();
		this->part.reset();
	}

	if (p)
	{
		conf->onRequestDocSharing(p->GetPartId(),L"NONE");
		p->SetVideoOutput(NULL,MediaFrame::VIDEO_SLIDES);
		conf->SetDocSharingMosaic(-1);
	}

	if (logo && output != NULL)
	{
		output->SetVideoSize(logo->GetWidth(),logo->GetHeight());
		output->NextFrame(logo);
	}
}


int SharedDocMixer::StopSharing()
{
	ParticipantPtr currentPart = this->part.lock();

	int floorRequestId = 0;
	if (currentPart)
		floorRequestId = GetFloorRequestId(currentPart->GetPartId());

	//Rendre la parole d'abord : le pair doit l'apprendre. onFloorReleased
	//enchaine sur le demontage.
	if (bfcpServer && floorRequestId)
		return bfcpServer->RevokeFloorRequest(floorRequestId, "stopped by the conference") ? 1 : 0;

	TearDownSharing();
	return 1;
}

int SharedDocMixer::StopSharing(ParticipantPtr part)
{
	ParticipantPtr currentPart = this->part.lock();
	if ( currentPart == part )
		return StopSharing();

	return 1;
}

int SharedDocMixer::NextFrame(PictPtr pic)
{
	ParticipantPtr currentPart = this->part.lock();
	if ( currentPart && output != NULL)
		return output->NextFrame(pic);

	return 0;
}

int SharedDocMixer::SetVideoSize(int width,int height)
{
	if ( output != NULL)
		return output->SetVideoSize(width,height);

	return 0;
}

int SharedDocMixer::addParticipant(int confId, ParticipantPtr part,Participant::DocSharingMode docSharingMode,MediaFrame::MediaProtocol proto)
{
	int res = 1;

	if (part)
	{
		part->SetDocSharingMode(docSharingMode);

		switch ( part->GetDocSharingMode() )
		{
			case  Participant::BFCP_TCP:
			case  Participant::BFCP_UDP:
				res = StartBfcpServer(confId, part);
				break;
			default:
				break;
		}
	}

	Log(">addParticipant res=%i\n",res);

	return res;
}

int SharedDocMixer::removeParticipant(ParticipantPtr part)
{
	if (! part)
		return 0;

	switch ( part->GetDocSharingMode() )
	{
		case  Participant::BFCP_TCP:
		case  Participant::BFCP_UDP:
			break;
		default:
			return 0;
	}

	const int partId = part->GetPartId();

	//Le retrait revoque la parole que ce participant tenait, et le pair recoit
	//un Goodbye : c'est le serveur qui s'en charge.
	if (bfcpServer)
		bfcpServer->RemoveUser(partId);

	std::unique_ptr<BFCPUdpEndpoint> endpoint;
	{
		std::lock_guard<std::mutex> lock(mutex);
		floorRequests.erase(partId);
		participants.erase(partId);

		std::map<int, std::unique_ptr<BFCPUdpEndpoint> >::iterator it = udpEndpoints.find(partId);
		if (it != udpEndpoints.end())
		{
			endpoint = std::move(it->second);
			udpEndpoints.erase(it);
		}
	}

	//Hors du verrou : le destructeur retire le handler du reacteur, ce qui
	//attend la fin du tour en cours.
	endpoint.reset();

	return 1;
}

// BFCP Implementation
bool SharedDocMixer::StartBfcpServer(int confId, ParticipantPtr part)
{
	if (! part)
		return false;

	const int partId = part->GetPartId();

	if (! bfcpServer)
	{
		bfcpServer.reset(new BFCPFloorControlServer(confId, this));
		bfcpServer->AddFloor(FloorId);
		this->confId = confId;
	}

	bfcpServer->AddUser(partId);

	{
		std::lock_guard<std::mutex> lock(mutex);
		participants[partId] = part;
	}

	switch ( part->GetDocSharingMode() )
	{
		case Participant::BFCP_TCP:
		{
			//Une seule ecoute pour la conference : c'est son port que le SDP
			//annonce, et les participants s'y connectent.
			if (tcpListener)
				return true;

			std::unique_ptr<BFCPTcpListener> listener(new BFCPTcpListener(bfcpServer.get()));
			if (! listener->Open())
				return Error("StartBfcpServer: cannot open the TCP listen\n");

			Log("StartBfcpServer on TCP port %d\n", listener->GetPort());
			tcpListener = std::move(listener);
			return true;
		}

		case Participant::BFCP_UDP:
		{
			//Une socket par participant : c'est elle qui porte la fiabilite,
			//et c'est son port que le SDP de CE participant annonce.
			{
				std::lock_guard<std::mutex> lock(mutex);
				if (udpEndpoints.find(partId) != udpEndpoints.end())
					return true;
			}

			std::unique_ptr<BFCPUdpEndpoint> endpoint(new BFCPUdpEndpoint(bfcpServer.get(), partId));
			if (! endpoint->Open())
				return Error("StartBfcpServer: cannot open the UDP socket of participant %d\n", partId);

			Log("Started UDP/BFCP for participant %d on port %d\n", partId, endpoint->GetPort());

			std::lock_guard<std::mutex> lock(mutex);
			udpEndpoints[partId] = std::move(endpoint);
			return true;
		}

		default:
			return false;
	}
}


bool SharedDocMixer::StopBfcpServer()
{
	std::unique_ptr<BFCPTcpListener> listener;
	std::map<int, std::unique_ptr<BFCPUdpEndpoint> > endpoints;

	{
		std::lock_guard<std::mutex> lock(mutex);
		listener.swap(tcpListener);
		endpoints.swap(udpEndpoints);
		floorRequests.clear();
		participants.clear();
	}

	//Les transports d'abord : une fois retires, plus rien n'arrive du reseau.
	listener.reset();
	endpoints.clear();

	if (bfcpServer)
	{
		Log("StopBfcpServer closing....\n");
		bfcpServer->End();
		bfcpServer.reset();
	}

	return true;
}

int SharedDocMixer::getServerPort(ParticipantPtr part)
{
	if (! part)
		return 0;

	switch ( part->GetDocSharingMode() )
	{
		case  Participant::BFCP_TCP:
			//Le port reellement lie par l'ecoute, pas celui qu'une socket
			//sonde avait vu libre un instant plus tot.
			return tcpListener ? tcpListener->GetPort() : 0;

		case  Participant::BFCP_UDP:
		{
			std::lock_guard<std::mutex> lock(mutex);
			std::map<int, std::unique_ptr<BFCPUdpEndpoint> >::iterator it = udpEndpoints.find(part->GetPartId());
			return it != udpEndpoints.end() ? it->second->GetPort() : 0;
		}

		default:
			return 0;
	}
}


/* BFCPFloorControlServer::Listener */

void SharedDocMixer::onFloorRequest(int floorRequestId, int userId, int beneficiaryId, std::set<int> floorIds)
{
	ParticipantPtr currentPart = this->part.lock();

	if (currentPart && currentPart->GetPartId() == userId)
	{
		Log("FloorRequest already processed for this user %i.\n", userId);
		return;
	}

	//Une demande concurrente : celle du partage en cours tombe d'abord, sinon
	//le controleur verrait deux demandes vivantes pour un seul floor.
	if (currentPart)
	{
		const int previous = GetFloorRequestId(currentPart->GetPartId());
		if (previous && bfcpServer)
			bfcpServer->RevokeFloorRequest(previous, "granted to other user");
	}

	{
		std::lock_guard<std::mutex> lock(mutex);
		floorRequests[userId] = floorRequestId;
	}

	if (conf)
		conf->onRequestDocSharing(userId, L"WAITING_ACCEPT");
}


void SharedDocMixer::onFloorGranted(int floorRequestId, int beneficiaryId, std::set<int> floorIds)
{
	ParticipantPtr granted = GetParticipant(beneficiaryId);
	if (! granted)
		return (void)Error("onFloorGranted: participant %d is gone\n", beneficiaryId);

	ShareSecondaryStream(granted);
}


void SharedDocMixer::onFloorReleased(int floorRequestId, int beneficiaryId, std::set<int> floorIds)
{
	{
		std::lock_guard<std::mutex> lock(mutex);
		floorRequests.erase(beneficiaryId);
	}

	ParticipantPtr currentPart = this->part.lock();
	if (! currentPart || currentPart->GetPartId() != beneficiaryId)
		//Une demande qui n'avait pas encore la parole : rien a demonter.
		return;

	TearDownSharing();
}


void SharedDocMixer::onUserConnected(int userId)
{
	if (sharedMosaic < 0)
		return;

	if (conf)
		conf->SetDocSharingMosaic(sharedMosaic, userId);

	if (bfcpServer)
		bfcpServer->NotifyFloorStatus(userId, FloorId);
}


void SharedDocMixer::SetSharedMosaic(int mosaicId)
{
	Log(">SetSharedMosaic\n");

	//Retenu MEME quand personne ne partage : c'est ce que StartSending relit
	//pour brancher un flux SLIDES qui arrive apres.
	sharedMosaic = mosaicId;

	ParticipantPtr currentPart = this->part.lock();
	if (currentPart && bfcpServer)
		bfcpServer->NotifyFloorStatus(currentPart->GetPartId(), FloorId);
}


int SharedDocMixer::End()
{
	StopSharing();
	StopBfcpServer();

	output 		= NULL;
	logo		= nullptr;
	conf    	= NULL;

	confId			= 0;
	sharedMosaic	= -1;

	return 1;
}
