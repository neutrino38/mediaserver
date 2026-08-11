#include <tools.h>
#include "log.h"
#include <videomixer.h>
#include <pipevideoinput.h>
#include <pipevideooutput.h>
#include <set>
#include <functional>

typedef std::pair<int, DWORD> Pair;
typedef std::set<Pair, std::less<Pair>    > OrderedSetOfPairs;
typedef std::set<Pair, std::greater<Pair> > RevOrderedSetOfPairs;


DWORD VideoMixer::vadDefaultChangePeriod = 5000;

inline void CleanSlot(int pos, Mosaic *mosaic, const PictPtr &p_logo)
{
	if ( p_logo )
	{
		mosaic->KeepAspectRatio(false);
		//Update with logo
		mosaic->Update(pos,p_logo);
	}
	else
	{
		//Clean it
		mosaic->Clean(pos);

	}
}

inline void CleanSlot(int pos, Mosaic *mosaic)
{
		//Clean it
		mosaic->Clean(pos);
}

int VideoMixer::LoadLogo(const char * filename)
{
	logo = Pict::Load(filename);
	if (logo)
	{
		//Protegemos la lista
		lstVideosUse.WaitUnusedAndLock();


		//For each mosaic
		for ( Mosaics::iterator itMosaic=mosaics.begin();itMosaic!=mosaics.end();++itMosaic)
		{
			//Set logo and strech it
			itMosaic->second->KeepAspectRatio(false);
			UpdateMosaic(itMosaic->second.get());
		}

		lstVideosUse.Unlock();
		return 1;
	}
	return 0;
}
void VideoMixer::SetVADDefaultChangePeriod(DWORD ms)
{
	//Set it
	vadDefaultChangePeriod = ms;
	//Log it
	Log("-VideoMixer VAD default change period set to %dms\n",vadDefaultChangePeriod);
}
/***********************
* VideoMixer
*	Constructord
************************/
VideoMixer::VideoMixer(const std::wstring &tag) : eventSource(tag)
{
        //Save tag
 	this->tag = tag;
	
	//Incializamos a cero
	defaultMosaic	= NULL;

	//No mosaics
	maxMosaics = 0;

	//No proxy
	proxy = NULL;
	//No vad
	vadMode = NoVAD;

}

/***********************
* ~VideoMixer
*	Destructor
************************/
VideoMixer::~VideoMixer()
{
	//Contrat Worker : arrêter le thread avant de détruire l'état dérivé
	StopThread();
}

/************************
* Run
*	Thread de mezclado de video (corps du Worker)
*************************/
int VideoMixer::Run()
{
	int forceUpdate = 0;
	DWORD version = 0;

	//Video Iterator
	Videos::iterator it;
	Mosaics::iterator itMosaic;

	Log(">MixVideo\n");

	//Mientras estemos mezclando
	while(mixingVideo)
	{
		//Protegemos la lista
		lstVideosUse.WaitUnusedAndLock();

		//For each video
		for (it=lstVideos.begin();it!=lstVideos.end();++it)
		{
			//Get input
			PipeVideoInput *input = it->second->input.get();

			//Get mosaic
			Mosaic *mosaic = it->second->mosaic;

			//Si no ha cambiado el frame volvemos al principio
			if (input && mosaic && (mosaic->HasChanged() || forceUpdate))
				//Colocamos el frame (composite mosaïque -> Pict, pont Phase 5)
				input->SetFrame(mosaic->GetPict());
		}

		
		//For each mosaic
		for (itMosaic=mosaics.begin();itMosaic!=mosaics.end();++itMosaic)
			//Reset it
			itMosaic->second->Reset();

		//Desprotege la lista (crash correction - moved after mosaic reset)
		lstVideosUse.Unlock();
		//LOck the mixing (tenu pendant toute la passe, relâché en fin
		//d'itération — l'ancien unlock explicite était au même endroit)
		std::unique_lock<std::mutex> mixLock(mixVideoMutex);

		//Everything is updated
		forceUpdate = 0;

		//Wait for new images or timeout and adquire mutex on exit
		if (mixVideoCond.wait_for(mixLock, std::chrono::milliseconds(500)) == std::cv_status::timeout)
		{

			//Force an update each second
			forceUpdate = 1;
			//go to the begining (le unique_lock se libère par le scope)
			continue;
		}

		//Protegemos la lista
		lstVideosUse.WaitUnusedAndLock();

		//New version
		version++;

		//For each mosaic
		for (itMosaic=mosaics.begin();itMosaic!=mosaics.end();++itMosaic)
		{
			//Calculate max vad
			DWORD maxVAD = 0;
			//No maximum vad participant
			int vadId = 0;
			//No previous pos of var participant
			int vadPos = Mosaic::NotShown;

			//Get Mosaic
			Mosaic *mosaic = itMosaic->second.get();

			//If vad mode is full
			if (vadMode!=NoVAD && proxy)
			{
				//We have not changed, yet
				bool changed = false;
				//Get number of available slots
				int numSlots = mosaic->GetNumSlots();
				
				//Nos recorremos los videos
				for (it=lstVideos.begin();it!=lstVideos.end();++it)
				{
					//Get Id
					int id = it->first;
					//Get vad
					DWORD vad = proxy->GetVAD(id);
					//Get position
					int pos = mosaic->GetPosition(id);

					//Check if it is in the mosaic
					if (pos==Mosaic::NotFound)
						//Next
						continue;

					//Check if position is fixed
					if (mosaic->IsFixed(pos))
						//this slot cannot be changed and the participant cannot be moved
						continue;

					//Found the highest VAD participant but elect at least one.
					if (vad>maxVAD || vadId==0)
					{
						//Store max vad value
						maxVAD = vad;
						//Store speaker participant
						vadId = id;
						//Store pos of speaker
						vadPos = pos;
					}

					//Send VAD info to mosaic
					if (mosaic->UpdateParticipantInfo(id,vad) > 0)
						//We need to racalculate positions
						changed = true;
				}

				//Get old vad participant
				int oldVad = mosaic->GetVADParticipant();
				//Get old vad position
				int oldVadPos = mosaic->GetVADPosition();

				//If the VAD is shown
				if (oldVadPos>=0)
				{
					//If there is an active speaker and the vad is shown
					if (maxVAD>0)
					{
						//Depending on the VAD mode
						switch(vadMode)
						{
							case FullVAD:
								//If VAD slot is not blocked
								if (mosaic->GetBlockingTime(oldVadPos)<=getTime())
								{
									//We have a new vad participant
									if (vadId!=oldVad)
									{
										//Log it
										Log("old active speaker = %d -> new one = %d, vad level = %d\n", oldVad, vadId, maxVAD);
										
										CleanSlot(oldVadPos, mosaic);
									}
									// set the VAD participant
									mosaic->SetVADParticipant(vadId,getTime() + vadDefaultChangePeriod*1000);
					
									//If it was shown elsewere than in VAD
									//and cleanup the slot
									if (vadPos>=0 && vadPos!=oldVadPos && vadId!=oldVad)
									{
										
										//If slot is not locked or fixed, reset pos.
										mosaic->SetSlot(vadPos,Mosaic::SlotReset);
										//We are changed
										changed = true;
										//Clean it
										
										CleanSlot(vadPos, mosaic, logo);
									}
								}
								break;

							case BasicVAD:
								//Set VAD participant
								CleanSlot(oldVadPos, mosaic, logo);
								mosaic->SetVADParticipant(vadId,0);
								break;

							default:
							break;
						}
					//NO active speaker
					} else  {
						//Depending on the VAD mode
						switch ( vadMode )
						{
							case BasicVAD:
								//Clean it
								CleanSlot(oldVadPos, mosaic, logo);
								break;
								
							case FullVAD:
								//If we had an active speaker before
								
								if ( oldVad > 0 )
								{
									// Reelect the same participant if no voice activity
									mosaic->SetVADParticipant(oldVad,getTime() + vadDefaultChangePeriod*1000);
								} else {
									// Special case where no partipant was elected before
									// or VAD participant left the conference
									// If vadId = 0 -> no video in this mosaic, clean old slot but do not do anything else
									if ( vadId > 0 ) Log("No previous active speaker and no voice activity -> new one = %d (was in pos %d)\n",
														  vadId, vadPos);
									mosaic->SetVADParticipant(vadId,getTime() + vadDefaultChangePeriod*1000);
									//If the new vad was shown in a different slot than the vad one
									if (vadPos >= 0 && vadPos != oldVadPos)
									{
										//Free the slot
										mosaic->SetSlot(vadPos,Mosaic::SlotReset);
										//We are changed
										changed = true;
										//Clean it
										CleanSlot(oldVadPos, mosaic);
										CleanSlot(vadPos, mosaic, logo);
										// Feed old VAD participa,t with non null VAD info
										// to try to keep it in the visible mosaic (and set it as eligible)
										if ( vadId > 0 ) mosaic->UpdateParticipantInfo(vadId, 200);
									}
								}
								break;

							default:
								break;
						}
					}
				}

				//Check if full vad so reshufle participants
				if (vadMode==FullVAD && changed)
				{
					//Get old possitions
					int* old = (int*)malloc(numSlots*sizeof(int));
					//Copy them
					memcpy(old,mosaic->GetPositions(),numSlots*sizeof(int));
					//Calculate positions after change
					mosaic->CalculatePositions();
					//Get new mosaic positions
					int* positions = mosaic->GetPositions();
					//For each slot
					for (int i=0;i<numSlots;++i)
					{
					//If it is free
						if ( old[i] != positions[i] )
						{
						    if (  positions[i] != Mosaic::SlotFree )
						    {
						        // Update participant if it has moved even if the picture has not changed
							// Correct issue with videoMute and avatar
							it = lstVideos.find(positions[i]);
							//If it is found
							if (it!=lstVideos.end())
							{
								//Get output
								PipeVideoOutput *output = it->second->output.get();
								//Check if it has chenged
								if (output)
								{
									//Change mosaic
									CleanSlot(i, mosaic);
									mosaic->KeepAspectRatio( output->IsAspectRatioKept() );
									mosaic->Update(i,output->GetFrame());
								}
							}
						    }
						    else
						    {
								//Clean it
								CleanSlot(i, mosaic, logo);
						    }
						}			
					}
					//Free olf
					free(old);
				}

				//Get posistion and id for VAD now that we have updated it
				vadPos = mosaic->GetVADPosition();
				vadId = mosaic->GetVADParticipant();

				//Check if it is a active speaker and it is shown
				if (vadPos>=0)
				{
					//Get participant
					it = lstVideos.find(vadId);
					//If it is found
					if (it!=lstVideos.end())
					{
						//Get output
						PipeVideoOutput *output = it->second->output.get();
						//Check if it has chenged
						if (output && (output->IsChanged(version) || vadPos!=oldVadPos || vadId != oldVad))
							//Change mosaic
							mosaic->KeepAspectRatio( output->IsAspectRatioKept() );		
							mosaic->Update(vadPos,output->GetFrame());
					}
				}
			}

			//Nos recorremos los videos
			for (it=lstVideos.begin();it!=lstVideos.end();++it)
			{
				//Get Id
				int id = it->first;

				//Get output
				PipeVideoOutput *output = it->second->output.get();

				//Get position
				int pos = mosaic->GetPosition(id);

				//If we've got a new frame on source and it is visible
				if (output && pos>=0)
				{
					//Change mosaic
					mosaic->KeepAspectRatio( output->IsAspectRatioKept() );
					if ( output->IsChanged(version) )
					{
						if ( output->SizeHasChanged(version) )mosaic->Clean(pos);
					
						mosaic->Update(pos,output->GetFrame());
					}
				}
			}
		}

		//Desprotege la lista
		lstVideosUse.Unlock();

		//Desbloqueamos (fin de la passe : le unique_lock sort du scope)
	}

	Log("<MixVideo\n");
	return 0;
}
/*******************************
 * CreateMosaic
 *	Create new mosaic in the conference
 **************************************/
int VideoMixer::CreateMosaic(Mosaic::Type comp, int size)
{
	Log(">Create mosaic\n");

	//Get the new id
	int mosaicId = maxMosaics++;

	//Create mosaic. Un type invalide ne crée RIEN : propager l'échec par -1 (et non
	//par 0, qui est l'id légitime de la mosaïque par défaut) plutôt que de rendre un
	//id auquel aucune mosaïque ne correspond — les appelants s'en serviraient pour
	//indexer 'mosaics' et y trouveraient un shared_ptr vide.
	if (!SetCompositionType(mosaicId, comp, size))
	{
		//Error() rend 0, or 0 est l'id de la mosaïque par défaut : renvoyer -1.
		Error("<Create mosaic : echec pour le type de composition [%d]\n",comp);
		return -1;
	}

	Log("<Create mosaic  [id:%d]\n",mosaicId);

	//Return the new id
	return mosaicId;
}

/*******************************
 * GetMosaicSize
 *	Taille du composite d'une mosaique
 **************************************/
int VideoMixer::GetMosaicSize(int mosaicId,int &width,int &height)
{
	//Protege l'acces a la map (meme discipline que les autres accesseurs)
	lstVideosUse.IncUse();

	int res = 0;
	Mosaics::iterator it = mosaics.find(mosaicId);
	if (it!=mosaics.end() && it->second)
	{
		width  = it->second->GetWidth();
		height = it->second->GetHeight();
		res = 1;
	}

	lstVideosUse.DecUse();

	if (!res)
		return Error("-GetMosaicSize: mosaic not found [id:%d]\n",mosaicId);

	return res;
}

/*******************************
 * SetMosaicOverlayImage
 *	Set an overlay image in mosaic
 **************************************/
int VideoMixer::SetOverlayImage(int mosaicId, int id, const char* filename)
{
	Log("-SetOverlayImage [id:%d,\"%s\"]\n",mosaicId,filename);

	//Get mosaic from id
	Mosaics::iterator it = mosaics.find(mosaicId);

	//Check if we have found it
	if (it==mosaics.end())
		//error
		return Error("Mosaic not found [id:%d]\n",mosaicId);

	//Get the old mosaic
	Mosaic *mosaic = it->second.get();

	//Exit
	return mosaic->SetOverlayImage(id, filename);
}

/*******************************
 * ResetMosaicOverlay
 *	Set an overlay image in mosaic
 **************************************/
int VideoMixer::ResetOverlayImage(int mosaicId, int id)
{
	Log("-ResetMosaicOverlay [id:%d]\n",mosaicId);

	//Get mosaic from id
	Mosaics::iterator it = mosaics.find(mosaicId);

	//Check if we have found it
	if (it==mosaics.end())
		//error
		return Error("Mosaic not found [id:%d]\n",mosaicId);

	//Get the old mosaic
	Mosaic *mosaic = it->second.get();

	//Exit
	return mosaic->ResetOverlay(id);
}

int VideoMixer::GetMosaicPositions(int mosaicId,std::list<int> &positions)
{
	Log("-GetMosaicPositions [id:%d]\n",mosaicId);

	//Protegemos la lista
	lstVideosUse.IncUse();
	
	//Get mosaic from id
	Mosaics::iterator it = mosaics.find(mosaicId);

	//Check if we have found it
	if (it==mosaics.end())
	{
		//Unlock
		lstVideosUse.DecUse();
		//error
		return Error("Mosaic not found [id:%d]\n",mosaicId);
	}

	//Get the old mosaic
	Mosaic *mosaic = it->second.get();

	//Get num slots
	DWORD numSlots = mosaic->GetNumSlots();

	//Get data
	int* mosaicPos = mosaic->GetPositions();

	//For each pos
	for (int i=0;i<numSlots;++i)
		//Add it
		positions.push_back(mosaicPos[i]);

	//Unlock
	lstVideosUse.DecUse();
	
	//Exit
	return numSlots;
}
/***********************
* Init
*	Inicializa el mezclado de video
************************/
int VideoMixer::Init(Mosaic::Type comp,int size, const char * logoFile)
{
	if ( logoFile == NULL ) logoFile = "/var/lib/mediaserver/logo.png";
	//Allocamos para el logo
	logo = Pict::Load(logoFile);

	//Create default misxer
	int id = CreateMosaic(comp,size);

	//Type de composition invalide passé à la création de la conférence : refuser
	//l'initialisation. Sans ce test, mosaics[id] insérait un shared_ptr VIDE et
	//defaultMosaic devenait nul — le mixer partait avec une mosaïque fantôme.
	if (id < 0)
		return Error("-VideoMixer::Init: type de composition invalide [%d]\n",comp);

	//Set default
	defaultMosaic = mosaics[id].get();

	// Estamos mzclando
	mixingVideo = true;

	//Y arrancamoe el thread
	StartThread();

	return 1;
}

/***********************
* End
*	Termina el mezclado de video
************************/
int VideoMixer::End()
{
	Log(">End videomixer\n");

	//Borramos los videos
	Videos::iterator it;

	//Terminamos con la mezcla
	if (mixingVideo)
	{
		//Terminamos la mezcla
		mixingVideo = 0;

		//Seï¿½alamos la condicion (sous verrou : pas de réveil perdu)
		{
			std::lock_guard<std::mutex> g(mixVideoMutex);
			mixVideoCond.notify_one();
		}

		//Y esperamos
		StopThread();
	}

	//Protegemos la lista
	lstVideosUse.WaitUnusedAndLock();

	//Recorremos la lista
	for (it =lstVideos.begin();it!=lstVideos.end();++it)
	{
		//Obtenemos el video source
		VideoSource *video = (*it).second;
		//Les pipes sont des shared_ptr : rendus quand le dernier stream les
		//relâche (Point 1 / C-4).
		delete video;
	}

	//Clean the list
	lstVideos.clear();

	//Clean list (les shared_ptr detruisent les Mosaic)
	mosaics.clear();

	//Desprotegemos la lista
	lstVideosUse.Unlock();

	Log("<End videomixer\n");

	return 1;
}

/***********************
* CreateMixer
*	Crea una nuevo source de video para mezclar
************************/
int VideoMixer::CreateMixer(int id)
{
	Log(">CreateMixer video [%d]\n",id);

	//Protegemos la lista
	lstVideosUse.WaitUnusedAndLock();

	//Miramos que si esta
	if (lstVideos.find(id)!=lstVideos.end())
	{
		//Desprotegemos la lista
		lstVideosUse.Unlock();
		return Error("Video sourecer already existed\n");
	}

	//Creamos el source
	VideoSource *video = new VideoSource();

	//POnemos el input y el output
	//NB : PipeVideoOutput reçoit le mutex/cond du VideoMixer lui-même (couplage
	//de durée de vie résiduel) — la co-propriété shared_ptr du pipe ne le rend
	//PAS indépendant de la durée de vie du VideoMixer, qui vit toute la durée de
	//la conférence (membre de MultiConf détruit après participants). Point 1.
	video->input  = std::make_shared<PipeVideoInput>();
	video->output = std::make_shared<PipeVideoOutput>(&mixVideoMutex,&mixVideoCond);
	//No mosaic yet
	video->mosaic = NULL;

	//Y lo aï¿½adimos a la lista
	lstVideos[id] = video;

	//Desprotegemos la lista
	lstVideosUse.Unlock();

	//Y salimos
	Log("<CreateMixer video\n");

	return true;
}

/***********************
* InitMixer
*	Inicializa un video
*************************/
int VideoMixer::InitMixer(int id,int mosaicId)
{
	Log(">Init mixer [id:%d,mosaic:%d]\n",id,mosaicId);

	//Protegemos la lista
	lstVideosUse.IncUse();

	//Buscamos el video source
	Videos::iterator it = lstVideos.find(id);

	//Si no esta
	if (it == lstVideos.end())
	{
		//Desprotegemos
		lstVideosUse.DecUse();
		//Salimos
		return Error("Mixer not found\n");
	}

	//Obtenemos el video source
	VideoSource *video = (*it).second;

	//INiciamos los pipes
	video->input->Init();
	video->output->Init();

	//Get the mosaic for the user
	Mosaics::iterator itMosaic = mosaics.find(mosaicId);

	//If found
	if (itMosaic!=mosaics.end())
		//Set mosaic
		video->mosaic = itMosaic->second.get();
	else
		//Send only participant
		Log("-No mosaic for participant found, will be send only.\n");

	//Desprotegemos
	lstVideosUse.DecUse();

	Log("<Init mixer [%d]\n",id);

	//Si esta devolvemos el input
	return true;
}

/***********************************
 * SetMixerMosaic
 *	Add a participant to be shown in a mosaic
 ************************************/
int VideoMixer::SetMixerMosaic(int id,int mosaicId)
{
	Log(">SetMixerMosaic [id:%d,mosaic:%d]\n",id,mosaicId);

	//Get the mosaic for the user
	Mosaics::iterator itMosaic = mosaics.find(mosaicId);

	//Get mosaic
	Mosaic *mosaic = NULL;

	//If found
	if (itMosaic!=mosaics.end())
		//Set it
		mosaic = itMosaic->second.get();
	else
		//Send only participant
		Log("-No mosaic for participant found, will be send only.\n");

	//Protegemos la lista
	lstVideosUse.IncUse();

	//Buscamos el video source
	Videos::iterator it = lstVideos.find(id);

	//Si no esta
	if (it == lstVideos.end())
	{
		//Desprotegemos
		lstVideosUse.DecUse();
		//Salimos
		return Error("Mixer not found\n");
	}

	//Obtenemos el video source
	VideoSource *video = (*it).second;

	//Set mosaic
	video->mosaic = mosaic;

	//Desprotegemos
	lstVideosUse.DecUse();

	Log("<SetMixerMosaic [%d]\n",id);

	//Si esta devolvemos el input
	return true;
}
/***********************************
 * AddMosaicParticipant
 *	Add a participant to be shown in a mosaic
 ************************************/
int VideoMixer::AddMosaicParticipant(int mosaicId, int partId)
{
	Log("-AddMosaicParticipant [mosaic:%d,partId:%d]\n",mosaicId,partId);

	//Protegemos la lista
	lstVideosUse.IncUse();
	//Get the mosaic for the user
	Mosaics::iterator itMosaic = mosaics.find(mosaicId);

	//If not found
	if (itMosaic==mosaics.end())
	{
		//Desprotegemos
		lstVideosUse.DecUse();
		//Salimos
		return Error("Mosaic %d not found\n", mosaicId);
	}
	
	//Add participant to the mosaic
	itMosaic->second->AddParticipant(partId);
	//Desprotegemos
	lstVideosUse.DecUse();
	//Dump positions
	itMosaic->second->Dump();

	//Everything ok
	return 1;
}

/***********************************
 * RemoveMosaicParticipant
 *	Remove a participant to be shown in a mosaic
 ************************************/
int VideoMixer::RemoveMosaicParticipant(int mosaicId, int partId)
{
	int pos = 0;
	Log(">-RemoveMosaicParticipant [mosaic:%d,partId:%d]\n",mosaicId,partId);

	//Block
	lstVideosUse.WaitUnusedAndLock();

	//Get the mosaic for the user
	Mosaics::iterator itMosaic = mosaics.find(mosaicId);

	//If not found
	if (itMosaic==mosaics.end())
	{
		//Unblock
		lstVideosUse.Unlock();
		//Salimos
		return Error("Mosaic not found\n");
	}

	//Get mosaic
	Mosaic* mosaic = itMosaic->second.get();


	Log("In Mosaic %d VAD participant is  %d.\n", itMosaic->first, mosaic->GetVADParticipant() );
	//Check if it was the VAD
	if (partId == mosaic->GetVADParticipant())
	{
		Log("Participant %d was in VAD slot. Cleaning up.\n", partId);
		//Get VAD position
		pos = mosaic->GetVADPosition();

		//Check logo
		CleanSlot(pos, mosaic, logo);

		//Reset VAD
		mosaic->SetVADParticipant(0,0);
	}

	//Remove participant to the mosaic
	pos = mosaic->RemoveParticipant(partId);

	//If was shown Update mosaic
	if ( pos!= Mosaic::NotFound )
	    UpdateMosaic(mosaic);
	else
	    Log("No participant %d in mosaic %d.\n", partId, mosaicId);
	//Unblock
	lstVideosUse.Unlock();

	//Dump positions
	mosaic->Dump();

	//Correct
	return 1;
}

/***********************
* EndMixer
*	Finaliza un video
*************************/
int VideoMixer::EndMixer(int id)
{
	Log(">Endmixer [id:%d]\n",id);

	//Protegemos la lista
	lstVideosUse.IncUse();

	//Buscamos el video source
	Videos::iterator it = lstVideos.find(id);

	//Si no esta
	if (it == lstVideos.end())
	{
		//Desprotegemos
		lstVideosUse.DecUse();
		//Salimos
		return Error("-EndMixer: participant %d not found\n", id);
	}

	//Obtenemos el video source
	VideoSource *video = (*it).second;

	//Terminamos
	video->input->End();
	video->output->End();

	//Unset mosaic
	video->mosaic = NULL;

	//Dec usage
	lstVideosUse.DecUse();

	//LOck the mixing
	{
		std::lock_guard<std::mutex> mixGuard(mixVideoMutex);

		//If still mixing video
		if (mixingVideo)
		{
			//For all the mosaics
			for (Mosaics::iterator it = mosaics.begin(); it!=mosaics.end(); ++it)
			{
				//Get mosaic
				Mosaic *mosaic = it->second.get();
				//Remove particiapant ande get position for user
				//int pos = mosaic->RemoveParticipant(id);
				int pos = mosaic->GetPosition(id);
				int res = RemoveMosaicParticipant(it->first, id);
				if (pos >= 0)
				{
					Log("-Removed from mosaic [mosaicId:%d,pos:%d]\n",it->first,pos);
				}
			}
		}

		//Signal for new video
		mixVideoCond.notify_one();
	}

	Log("<Endmixer [id:%d]\n",id);

	//Si esta devolvemos el input
	return true;
}

/***********************
* DeleteMixer
*	Borra una fuente de video
************************/
int VideoMixer::DeleteMixer(int id)
{
	Log(">DeleteMixer video [%d]\n",id);

	//Protegemos la lista
	lstVideosUse.WaitUnusedAndLock();

	//Lo buscamos
	Videos::iterator it = lstVideos.find(id);

	//SI no ta
	if (it == lstVideos.end())
	{
		//Desprotegemos la lista
		lstVideosUse.Unlock();
		//Salimos
		return Error("Video mixer not found\n");
	}

	//Obtenemos el video source
	VideoSource *video = (*it).second;

	//Lo quitamos de la lista
	lstVideos.erase(id);

	//Desprotegemos la lista
	lstVideosUse.Unlock();

	//Les pipes sont des shared_ptr : la struct conteneur est libérée ici, mais
	//le pipe reste vivant tant qu'un stream participant en détient une copie
	//(Point 1 / C-4).
	delete video;

	Log("<DeleteMixer video [%d]\n",id);

	return 0;
}

/***********************
* GetInput
*	Obtiene el input para un id
************************/
VideoInput* VideoMixer::GetInput(int id)
{
	//Protegemos la lista
	lstVideosUse.IncUse();

	//Buscamos el video source
	Videos::iterator it = lstVideos.find(id);

	//Obtenemos el input
	VideoInput *input = NULL;

	//Si esta
	if (it != lstVideos.end())
		input = (VideoInput*)(*it).second->input.get();

	//Desprotegemos
	lstVideosUse.DecUse();

	//Si esta devolvemos el input
	return input;
}

/***********************
* GetSharedInput
*	Copie de shared_ptr sur le pipe d'entrée (co-propriété, Point 1 / C-4)
************************/
std::shared_ptr<VideoInput> VideoMixer::GetSharedInput(int id)
{
	lstVideosUse.IncUse();
	Videos::iterator it = lstVideos.find(id);
	std::shared_ptr<VideoInput> input;
	if (it != lstVideos.end())
		input = (*it).second->input;
	lstVideosUse.DecUse();
	return input;
}

/***********************
* GetOutput
*	Termina el mezclado de video
************************/
VideoOutput* VideoMixer::GetOutput(int id)
{
	//Protegemos la lista
	lstVideosUse.IncUse();

	//Buscamos el video source
	Videos::iterator it = lstVideos.find(id);

	//Obtenemos el output
	VideoOutput *output = NULL;

	//Si esta
	if (it != lstVideos.end())
		output = (VideoOutput*)(*it).second->output.get();

	//Desprotegemos
	lstVideosUse.DecUse();

	//Si esta devolvemos el input
	return output;
}

/***********************
* GetSharedOutput
*	Copie de shared_ptr sur le pipe de sortie (co-propriété, Point 1 / C-4)
************************/
std::shared_ptr<VideoOutput> VideoMixer::GetSharedOutput(int id)
{
	lstVideosUse.IncUse();
	Videos::iterator it = lstVideos.find(id);
	std::shared_ptr<VideoOutput> output;
	if (it != lstVideos.end())
		output = (*it).second->output;
	lstVideosUse.DecUse();
	return output;
}

/**************************
* SetCompositionType
*    Pone el modo de mosaico
***************************/
int VideoMixer::SetCompositionType(int mosaicId,Mosaic::Type comp, int size)
{
	Log(">SetCompositionType [id:%d,comp:%d,size:%d]\n",mosaicId,comp,size);

	//Protegemos la lista
	lstVideosUse.WaitUnusedAndLock();

	//Get mosaic from id
	Mosaics::iterator it = mosaics.find(mosaicId);

	//No mosaic
	Mosaic *oldMosaic = NULL;

	//Check if we have found it
	if (it!=mosaics.end())
	{
		//Get the old mosaic
		oldMosaic = it->second.get();
	}
	
	//New mosaic (propriedad en shared_ptr ; mosaic = observador crudo para la logica local)
	std::shared_ptr<Mosaic> mosaicPtr(Mosaic::CreateMosaic(comp,size));
	Mosaic *mosaic = mosaicPtr.get();

	//Type de composition invalide : ne RIEN toucher (la mosaïque en place reste
	//valide et la conférence continue), et surtout RELÂCHER le verrou avant de
	//sortir — sans quoi le mixer se figerait sur la première requête erronée.
	if (!mosaic)
	{
		lstVideosUse.Unlock();
		return Error("-SetCompositionType: type de composition invalide [%d]\n",comp);
	}

	//If we had a previus mosaic
	if (oldMosaic)
	{
	
		//Set old slots
		mosaic->SetSlots(oldMosaic->GetSlots(),oldMosaic->GetNumSlots());	

		//Add all the participants
		for (Videos::iterator it=lstVideos.begin();it!=lstVideos.end();++it)
		{
			//Get id
			DWORD partId = it->first;
			//Check if it was in the mosaic
			if (oldMosaic->HasParticipant(partId))
			{
				int pos = oldMosaic->GetPosition(partId);
				//Add participant to mosaic
				mosaic->AddParticipant(partId);
				
				// If participant is fixed and visible in the new mosaic, nail it.
				if ( pos >= 0 && pos < mosaic->GetNumSlots() && oldMosaic->IsFixed(pos))
				{
					mosaic->SetSlot(pos, partId);
				}
			}
			//Check mosaic
			if (it->second->mosaic==oldMosaic)
				//Update to new one
				it->second->mosaic=mosaic;
		}


		//Find vad slot
		int pos = oldMosaic->GetVADPosition();
		
		//If found
		if (pos>=0 && pos< mosaic->GetNumSlots())
		{
			//Set vad
			mosaic->SetVADParticipant(oldMosaic->GetVADParticipant(),0);
		}
			
		if ( vadMode == FullVAD )
		{
		    int pos = mosaic->GetPosition(mosaic->GetVADParticipant() );
		    mosaic->SetSlot(pos, Mosaic::SlotReset);
		}
		
		mosaic->MoveOverlays(oldMosaic);
		//IF it is the defualt one
		if (oldMosaic==defaultMosaic)
			//Set new one as defautl
			defaultMosaic = mosaic;

		//El antiguo mosaic se destruye al reemplazar el shared_ptr en la map
		//(mosaics[mosaicId] = mosaicPtr), tras haber leido todos sus datos.
	}

	//Recalculate positions
	mosaic->CalculatePositions();

	//Update it
	UpdateMosaic(mosaic);

	//And in the list
	mosaics[mosaicId] = mosaicPtr;

	//Signal for new video (sous verrou : pas de réveil perdu)
	{
		std::lock_guard<std::mutex> g(mixVideoMutex);
		mixVideoCond.notify_one();
	}

	//Unlock (Could this be done earlier??)
	lstVideosUse.Unlock();

	Log("<SetCompositionType\n");

	return 1;
}

/************************
* CalculatePosition
*	Calculate positions for participants
*************************/
int VideoMixer::UpdateMosaic(Mosaic* mosaic)
{

	//Get positions
	int *positions = mosaic->GetPositions();

	//Get number of slots
	int numSlots = mosaic->GetNumSlots();

	//For each one
	for (int i=0;i<numSlots;i++)
	{
		//If it is has a participant
		if (positions[i]>0)
		{
			//Find video
			Videos::iterator it = lstVideos.find(positions[i]);
			//Check we have found it
			if (it!=lstVideos.end())
			{
				//Get output
				PipeVideoOutput *output = it->second->output.get();
				//Update slot
				mosaic->Clean(i);
				mosaic->KeepAspectRatio( output->IsAspectRatioKept() );
				mosaic->Update(i,output->GetFrame());
			} else {
				//Check logo
				CleanSlot(i, mosaic, logo);
			}
		} else {
			//Update with logo
			CleanSlot(i, mosaic, logo);

		}
	}

	Log("<Updated mosaic\n");
	return 0;
}

/************************
* SetSlot
*	Set slot participant
*************************/
int VideoMixer::SetSlot(int mosaicId,int num,int id)
{
	Log(">SetSlot [mosaicId:%d,num:%d,id:%d]\n",mosaicId,num,id);

	//Get mosaic from id
	Mosaics::iterator it = mosaics.find(mosaicId);

	//Check if we have found it
	if (it==mosaics.end())
		//error
		return Error("Mosaic not found [id:%d]\n",mosaicId);


	//Get the old mosaic
	Mosaic *mosaic = it->second.get();

	//If it does not have mosaic
	if (!mosaic)
		//Exit
		return Error("Null mosaic");

	//Protegemos la lista
	lstVideosUse.WaitUnusedAndLock();

	//Get position
	int pos = mosaic->GetPosition(id);

	//If it was shown
	if (pos>=0)
	{
		CleanSlot(pos, mosaic, logo);
	}

	//Set it in the mosaic
	mosaic->SetSlot(num,id);

	//Calculate positions
	mosaic->CalculatePositions();

	//Update it
	UpdateMosaic(mosaic);

	//Dump positions
	mosaic->Dump();

	//Desprotegemos la lista
	lstVideosUse.Unlock();

	Log("<SetSlot\n");

	return 1;
}

int VideoMixer::DeleteMosaic(int mosaicId)
{
	//Protect the video map
	lstVideosUse.WaitUnusedAndLock();

	//Get mosaic from id
	Mosaics::iterator it = mosaics.find(mosaicId);

	//Check if we have found it
	if (it==mosaics.end())
	{
		//Desprotegemos
		lstVideosUse.Unlock();
		//error
		return Error("Mosaic not found [id:%d]\n",mosaicId);
	}

	//Get the old mosaic : conserve una referencia compartida para destruir el
	//Mosaic FUERA del verrou (el destructor puede ser lento).
	std::shared_ptr<Mosaic> mosaic = it->second;

	//For each video
	for (Videos::iterator itv = lstVideos.begin(); itv!= lstVideos.end(); ++itv)
	{
		//Check it it has dis mosaic
		if (itv->second->mosaic == mosaic.get())
			//Set to null
			itv->second->mosaic = NULL;
	}


	//Remove mosaic (la map suelta su shared_ptr ; 'mosaic' mantiene vivo el objeto)
	mosaics.erase(it);

	//Protect the video map
	lstVideosUse.Unlock();

	//El Mosaic se destruye aqui al salir del scope, fuera del verrou.
	//Exit
	return 1;
}

void VideoMixer::SetVADProxy(VADProxy* proxy)
{
	//Lock
	lstVideosUse.IncUse();
	//Set it
	this->proxy = proxy;
	//Unlock
	lstVideosUse.DecUse();
}

void VideoMixer::SetVADMode(VADMode vadMode)
{
	Log("-SetVadMode [%d]\n", vadMode);
	//Set it
	this->vadMode = vadMode;
	lstVideosUse.WaitUnusedAndLock();


	//For each mosaic
	if ( vadMode == 0 )
	{
	    for ( Mosaics::iterator itMosaic=mosaics.begin();itMosaic!=mosaics.end();++itMosaic)
	    {
		Mosaic * mosaic = itMosaic->second.get();
		int vadPos = mosaic->GetVADPosition();
		int vadId = mosaic->GetVADParticipant();			//Set logo and strech it
		
		if (vadPos >= 0 && vadId > 0)
		{
			mosaic->SetSlot(vadPos, Mosaic::SlotVAD);
			mosaic->SetVADParticipant(0,0);
			CleanSlot(vadPos, mosaic, logo);
		}
	    }
	}
	lstVideosUse.Unlock();
}


int VideoMixer::SetDisplayName(int mosaicId, int id, const char *name,int scriptCode)
{
	//Get mosaic from id
	int rez;
	
	lstVideosUse.IncUse();
	if ( mosaicId >= 0 )
	{
	    //Blcok
	    Mosaics::iterator it = mosaics.find(mosaicId);
	    	//Check if we have found it
	    if (it!=mosaics.end())
		rez = it->second->SetOverlayTXT(id, name,scriptCode);
	    else
		//error
		rez = Error("SetDisplayName: Mosaic not found [id:%d]\n",mosaicId);
	}
	else
	{

            for ( Mosaics::iterator itMosaic=mosaics.begin(); itMosaic != mosaics.end(); ++itMosaic)
            {
		Log("-SetDisplayName: mosaic ID %d.\n", itMosaic->first);
		itMosaic->second->SetOverlayTXT(id, name,scriptCode);
            }

	    rez = 1;
	}
	lstVideosUse.DecUse();
	return rez;
}

int VideoMixer::DumpMosaic(DWORD id,Mosaic* mosaic)
{
	char p[16];
	char line1[1024];
	char line2[1024];

	//Empty
	*line1=0;
	*line2=0;

	//Get num slots
	DWORD numSlots = mosaic->GetNumSlots();

	//Get data
	int* mosaicSlots = mosaic->GetSlots();
	int* mosaicPos   = mosaic->GetPositions();

	//Create string from array
	for (int i=0;i<numSlots;++i)
	{
		if (i)
		{
			strcat(line1,",");
			strcat(line2,",");
		}

		sprintf(p,"%.4d",mosaicSlots[i]);
		strcat(line1,p);
		sprintf(p,"%.4d",mosaicPos[i]);
		strcat(line2,p);
	}

	//Log
	Log("-MosaicSlots %d [%s]\n",id,line1);
	Log("-MosaicPos   %d [%s]\n",id,line2);

	//Send event
	eventSource.SendEvent("mosaic","{id:%d,slots:[%s],pos:[%s]}",id,line1,line2);
	//OK
	return 1;
}
