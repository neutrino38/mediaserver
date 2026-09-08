#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <stdio.h>
#ifdef __SSE2__
#include <emmintrin.h>
#endif
#include <set>
#include "log.h"
#include "tools.h"
#include "audiomixer.h"
#include "pipeaudioinput.h"
#include "pipeaudiooutput.h"
#include "sidebar.h"

/***********************
* AudioMixer
*	Constructor
************************/
AudioMixer::AudioMixer()
{
	//Not mixing
	mixingAudio = false;
	//No sidebars
	numSidebars = 0;
	//No default sidebar until Init
	defaultSidebar = NULL;
	//NO vad by default
	vad = false;
}

/***********************
* ~AudioMixer
*	Destructor
************************/
AudioMixer::~AudioMixer()
{
	//Contrat Worker : arreter le thread avant de detruire l'etat derive
	StopThread();
}


//Cadencement du mixeur : outils timespec sur CLOCK_MONOTONIC (insensible aux
//recalages NTP de l'horloge murale, contrairement à gettimeofday/msleep).
static inline bool tsBefore(const timespec &a,const timespec &b)
{
	return a.tv_sec<b.tv_sec || (a.tv_sec==b.tv_sec && a.tv_nsec<b.tv_nsec);
}

static inline QWORD tsDiffUsec(const timespec &from,const timespec &to)
{
	return (QWORD)((to.tv_sec-from.tv_sec)*1000000LL + (to.tv_nsec-from.tv_nsec)/1000);
}

/***********************************
* MixAudio
*	Mezcla los audios
************************************/
int AudioMixer::Run()
{
	DWORD step = 10;
	QWORD prev = 0;
	int overtimeCount;
	timespec start,next,now;
	//Logeamos
	Log(">MixAudio\n");

	//Init ts
	clock_gettime(CLOCK_MONOTONIC,&start);
	next = start;
	overtimeCount = 0;
	//Mientras estemos mezclando
	while(mixingAudio)
	{
		//Tick absolu suivant : +step ms exactement, la gigue d'un réveil
		//tardif ne se cumule pas sur les ticks suivants
		next.tv_nsec += step*1000000;
		if (next.tv_nsec>=1000000000)
		{
			next.tv_nsec -= 1000000000;
			next.tv_sec++;
		}

		//check we have not to hurry up
		clock_gettime(CLOCK_MONOTONIC,&now);
		if (tsBefore(now,next))
		{
			//Wait until next tick
			while (clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&next,NULL)==EINTR);
			if (overtimeCount > 0)
			{
				overtimeCount--;
			}
		}
		else
		{
			QWORD late = tsDiffUsec(next,now);
			//Un réveil tardif isolé (gigue d'ordonnanceur, courante en VM)
			//est sans conséquence : numSamples suit l'horloge réelle. On ne
			//signale qu'un retard persistant — le mixage ne tient pas les
			//10 ms — via le solde ticks en retard / ticks à l'heure
			//(plafonné pour ne pas déborder en surcharge permanente)
			if (overtimeCount < 100 && ++overtimeCount == 25)
				Log("-Audiomixer sustained overtime, hurrying up [%llu us late]\n",(unsigned long long)late);
			//Après un gros décrochage (process suspendu), on resynchronise
			//pour éviter une rafale de ticks de rattrapage
			if (late > 100000)
			{
				Log("-Audiomixer resync after stall [%llu us late]\n",(unsigned long long)late);
				next = now;
			}
		}


		//Block list
		lstAudiosUse.WaitUnusedAndLock();

		//Get new time (base monotone : numSamples suit l'horloge réelle)
		clock_gettime(CLOCK_MONOTONIC,&now);
		QWORD curr = tsDiffUsec(start,now);

		//Get num samples at desired rate for the time difference
		DWORD numSamples = (curr*rate)/1000000-(prev*rate)/1000000;

		//Update curr
		prev = curr;

		//At most the maximum
		if (numSamples>Sidebar::MIXER_BUFFER_SIZE)
		{
			//Log
			Log("-AudiMixer num mixing samples bigger than buffer [%d]\n",numSamples);
			//Set it at most (shoudl never happen)
			numSamples = Sidebar::MIXER_BUFFER_SIZE;
		}

		//For each sidepar
		for (Sidebars::iterator sit=sidebars.begin(); sit!=sidebars.end(); ++sit)
			//REset
			sit->second->Reset();
		
		//First pass: Iterate through the audio inputs and calculate the sum of all streams
		for(Audios::iterator it = audios.begin(); it != audios.end(); ++it)
		{
			//Get the source
			AudioSource *audio = it->second.get();
			//Get the samples from the fifo
			audio->len = audio->output->GetSamples(audio->buffer,numSamples);
			//Clean rest
			memset(audio->buffer+audio->len,0,Sidebar::MIXER_BUFFER_SIZE-audio->len);
			//Get VAD value
			audio->vad = audio->output->GetVAD(numSamples);
		}

		for(Audios::iterator it = audios.begin(); it != audios.end(); ++it)
		{
			//Get the source
			AudioSource *audio = it->second.get();
			//Get id
			DWORD id = it->first;

			for (Sidebars::iterator sit = sidebars.begin(); sit!=sidebars.end(); ++sit)
				//Mix it and update length
			    if ( audio->len > 0 && sit->second->HasParticipant(id) )
				audio->len = sit->second->Update(id,audio->buffer,audio->len);
		}
		
		// Second pass: Calculate this stream's output
		for(Audios::iterator it = audios.begin(); it != audios.end(); it++)
		{
			//Get the source
			AudioSource *audio = it->second.get();
			//Get id
			DWORD id = it->first;
			//Check audio
			if (!audio)
				//Next
				continue;
			//Check sidebar
			if (!audio->sidebar)
				//Next
				continue;
			//Get mixed buffer
			SWORD *mixed = audio->sidebar->GetBuffer();
			//And the audio buffer for participant
			SWORD *buffer = audio->buffer;

			//Check if we have been added to the sidebar
			//(un participant muet reçoit le mix tel quel : rien à soustraire)
			if (audio->sidebar->HasParticipant(id) && audio->len > 0)
			{
#ifndef __SSE2__
				// Mixing without SSE
				//Calculate the result (différence saturée, pas de wrap 16 bits)
				for(DWORD i=0; i<audio->len; ++i)
				{
					//We don't want to hear our own signal
					int diff = mixed[i] - buffer[i];
					if (diff > 32767)
						diff = 32767;
					else if (diff < -32768)
						diff = -32768;
					buffer[i] = diff;
				}
#else
				//Get pointers to buffer
				__m128i* b = (__m128i*) buffer;
				__m128i* m = (__m128i*) mixed;

				//Sum 8 ech time
				for(DWORD n = (audio->len + 7) >> 3; n != 0; --n,++b,++m)
				{
					//Load data in SSE registers
					__m128i xmm1 = _mm_load_si128(m);
					__m128i xmm2 = _mm_load_si128(b);
					//Différence saturée (subs) : même coût que sub, sans wrap 16 bits
					_mm_store_si128(b,  _mm_subs_epi16(xmm1,xmm2));
				}
#endif
				//Check length
				if (audio->len<numSamples)
					//Copy the rest
					memcpy(buffer+audio->len,mixed+audio->len,(numSamples-audio->len)*sizeof(SWORD));
				//Put the output
				audio->input->PutSamples(buffer,numSamples);
			} else {
				//Copy everything as it is
				audio->input->PutSamples(mixed,numSamples);
			}
		}

		//Unblock list
		lstAudiosUse.Unlock();
	}

	//Logeamos
	Log("<MixAudio\n");

	return 1;
}

/***********************
* Init
*	Inicializa el mezclado de audio
************************/
int AudioMixer::Init(bool vad,DWORD rate)
{
	Log("-Init audio mixer [vad:%d,rate:%d]\n",vad,rate);

	//Store if we need to use vad or not
	this->vad = vad;

	//Check rate
	if (rate == 8000 || rate == 16000 || rate == 32000 || rate == 48000)
	{
		//Store rate
		this->rate = rate;
	}
	else
	{
		return Error("Unsupported rate. Must be 8000, 16000, 32000 or 48000 Hz.\n");
	}

	// Estamos mzclando
	mixingAudio = true;

	//Create default sidebar
	int id = CreateSidebar();

	//Set default
	defaultSidebar = sidebars[id].get();

	//Y arrancamoe el thread
	StartThread();

	return 1;
}


/***********************
* End
*	Termina el mezclado de audio
************************/
int AudioMixer::End()
{
	Log(">End audiomixer\n");


	//Terminamos con la mezcla
	if (mixingAudio)
	{
		//Terminamos la mezcla
		mixingAudio = false;

		//Y esperamos (reveille aussi le tick via le Wait du Worker)
		StopThread();
	}

	//Lock
	lstAudiosUse.WaitUnusedAndLock();

	//Recorremos la lista
	for (Audios::iterator it =audios.begin();it!=audios.end();++it)
	{
		//Obtenemos el audio source
		AudioSource *audio = it->second.get();

		//Terminamos
		audio->input->End();
		audio->output->End();
	}

	//Clear list : les pipes sont des shared_ptr, la mémoire n'est rendue que
	//quand le dernier détenteur (stream participant) les relâche (Point 1 / C-4).
	audios.clear();

	//Clear list
	sidebars.clear();
	defaultSidebar = NULL;

	//Unlock
	lstAudiosUse.Unlock();
	
	Log("<End audiomixer\n");
	
	return 1;
}

/***********************
* CreateMixer
*	Crea una nuevo source de audio para mezclar
************************/
int AudioMixer::CreateMixer(int id)
{
	Log(">CreateMixer audio [%d]\n",id);

	//Protegemos la lista
	lstAudiosUse.WaitUnusedAndLock();

	//Miramos que si esta
	if (audios.find(id)!=audios.end())
	{
		//Desprotegemos la lista
		lstAudiosUse.Unlock();
		return Error("Audio sourecer already existed\n");
	}

	//Creamos el source
	std::unique_ptr<AudioSource> audio = std::make_unique<AudioSource>();

	//POnemos el input y el output
	audio->input  = std::make_shared<PipeAudioInput>();
	audio->output = std::make_shared<PipeAudioOutput>(vad);
	//No sidebar yet
	audio->sidebar = NULL;
	//Clean buffer
	memset(audio->buffer, 0, Sidebar::MIXER_BUFFER_SIZE*sizeof(SWORD));
	audio->len = 0;
	audio->vad = 0;

	//Y lo a�adimos a la lista
	audios[id] = std::move(audio);

	//Desprotegemos la lista
	lstAudiosUse.Unlock();

	//Y salimos
	Log("<CreateMixer audio\n");

	return true;
}

/***********************
* InitMixer
*	Inicializa un audio
*************************/
int AudioMixer::InitMixer(int id,int sidebarId)
{

	Log(">Audio mixer: partId=%d will listen to sidebar %d\n",id, sidebarId);

	//Verrou exclusif : on mute audio->sidebar et le set du sidebar par défaut,
	//deux requêtes XML-RPC concurrentes ne doivent pas s'entrelacer (§1.3)
	lstAudiosUse.WaitUnusedAndLock();

	//Buscamos el audio source
	Audios::iterator it = audios.find(id);

	//Si no esta
	if (it == audios.end())
	{
		//Desprotegemos
		lstAudiosUse.Unlock();
		//Salimos
		return Error("Mixer not found\n");
	}

	//Obtenemos el audio source
	AudioSource *audio = it->second.get();

	//Get the sidebar for the user
	Sidebars::iterator itSidebar = sidebars.find(sidebarId);

	//If found
	if (itSidebar!=sidebars.end())
		//Set it
		audio->sidebar = itSidebar->second.get();
	else
		//Send only participant
		Log("-No sidebar %d for participant found, will be send only.\n", sidebarId);

	//INiciamos los pipes
	audio->input->Init(rate);
	audio->output->Init(rate);

	//Add participant to default sidebar
	//Choix de conception : tout participant CONTRIBUE au sidebar par défaut
	//(défaut = « tous ») ; ce qu'il ÉCOUTE reste audio->sidebar.
	defaultSidebar->AddParticipant(id);

	//Desprotegemos
	lstAudiosUse.Unlock();

	Log("<Init mixer [%d]\n",id);

	//Si esta devolvemos el input
	return true;
}


/***********************
* EndMixer
*	Finaliza un audio
*************************/
int AudioMixer::EndMixer(int id)
{
	//Protegemos la lista
	lstAudiosUse.WaitUnusedAndLock();

	//Buscamos el audio source
	Audios::iterator it = audios.find(id);

	//Si no esta	
	if (it == audios.end())
	{
		//Desprotegemos
		lstAudiosUse.Unlock();
		//Salimos
		return false;
	}

	//Remvoe participant to default sidebar
	defaultSidebar->RemoveParticipant(id);

	//Obtenemos el audio source
	AudioSource *audio = it->second.get();

	//Terminamos
	audio->input->End();
	audio->output->End();

	//Unset sidebar
	audio->sidebar = NULL;

	//For all the sidebars
	for (Sidebars::iterator it = sidebars.begin(); it!=sidebars.end(); ++it)
		//Remove particiapant
		it->second->RemoveParticipant(id);

	//Desprotegemos
	lstAudiosUse.Unlock();

	//Si esta devolvemos el input
	return true;;
}

/***********************
* DeleteMixer
*	Borra una fuente de audio
************************/
int AudioMixer::DeleteMixer(int id)
{
	Log("-DeleteMixer audio [%d]\n",id);

	//Protegemos la lista
	lstAudiosUse.WaitUnusedAndLock();

	//Lo buscamos
	Audios::iterator it = audios.find(id);

	//SI no ta
	if (it == audios.end())
	{
		//DDesprotegemos la lista
		lstAudiosUse.Unlock();
		//Salimos
		return Error("Audio source not found\n");
	}

	//Obtenemos el audio source
	//Les pipes sont des shared_ptr : la destruction de la struct conteneur ne
	//libère le pipe que si aucun stream participant n'en détient encore une
	//copie (Point 1 / C-4). Elle a lieu à la sortie, donc hors verrou.
	std::unique_ptr<AudioSource> audio = std::move(it->second);

	//Lo quitamos de la lista
	audios.erase(it);

	//Desprotegemos la lista
	lstAudiosUse.Unlock();

	return 0;
}

/***********************
* GetInput
*	Obtiene el input para un id
************************/
AudioInput* AudioMixer::GetInput(int id)
{
	//Protegemos la lista
	lstAudiosUse.IncUse();

	//Buscamos el audio source
	Audios::iterator it = audios.find(id);

	//Obtenemos el input
	AudioInput *input = NULL;

	//Si esta
	if (it != audios.end())
		input = it->second->input.get();

	//Desprotegemos
	lstAudiosUse.DecUse();

	//Si esta devolvemos el input
	return input;
}

/***********************
* GetSharedInput
*	Copie de shared_ptr sur le pipe d'entrée (co-propriété, Point 1 / C-4)
************************/
std::shared_ptr<AudioInput> AudioMixer::GetSharedInput(int id)
{
	lstAudiosUse.IncUse();
	Audios::iterator it = audios.find(id);
	std::shared_ptr<AudioInput> input;
	if (it != audios.end())
		input = it->second->input;
	lstAudiosUse.DecUse();
	return input;
}

/***********************
* GetOutput
*	Obtiene el output para un id
************************/
AudioOutput* AudioMixer::GetOutput(int id)
{
	//Protegemos la lista
	lstAudiosUse.IncUse();

	//Buscamos el audio source
	Audios::iterator it = audios.find(id);

	//Obtenemos el output
	AudioOutput *output = NULL;

	//Si esta
	if (it != audios.end())
		output = it->second->output.get();

	//Desprotegemos
	lstAudiosUse.DecUse();

	//Si esta devolvemos el input
	return output;
}

/***********************
* GetSharedOutput
*	Copie de shared_ptr sur le pipe de sortie (co-propriété, Point 1 / C-4)
************************/
std::shared_ptr<AudioOutput> AudioMixer::GetSharedOutput(int id)
{
	lstAudiosUse.IncUse();
	Audios::iterator it = audios.find(id);
	std::shared_ptr<AudioOutput> output;
	if (it != audios.end())
		output = it->second->output;
	lstAudiosUse.DecUse();
	return output;
}

/***********************************
 * SetMixerSidebar
 *	Add a participant to be shown in a sidebar
 ************************************/
int AudioMixer::SetMixerSidebar(int id,int sidebarId)
{
	Log(">SetMixerSidebar [id:%d,sidebar:%d]\n",id,sidebarId);

	//Verrou exclusif : mutation d'état, cf. §1.3
	lstAudiosUse.WaitUnusedAndLock();

	//Buscamos el audio source
	Audios::iterator it = audios.find(id);

	//Si no esta
	if (it == audios.end())
	{
		//Desprotegemos
		lstAudiosUse.Unlock();
		//Salimos
		return Error("Mixer not found\n");
	}

	//Obtenemos el audio source
	AudioSource *audio = it->second.get();

	//Get the sidebar for the user
	Sidebars::iterator itSidebar = sidebars.find(sidebarId);

	//If found
	if (itSidebar!=sidebars.end())
		//Set sidebar
		audio->sidebar = itSidebar->second.get();
	else
		//Send only participant
		Log("-No sidebar %d for participant found, will be send only.\n",
                    sidebarId );

	//Desprotegemos
	lstAudiosUse.Unlock();

	Log("<SetMixerSidebar [%d]\n",id);

	//Si esta devolvemos el input
	return true;
}


int AudioMixer::GetMixerSidebar(int id)
{
	Log(">GetMixerSidebar [id:%d]\n",id);

	//Protegemos la lista
	lstAudiosUse.IncUse();

	//Buscamos el audio source
	Audios::iterator it = audios.find(id);

	//Si no esta
	if (it == audios.end())
	{
		//Desprotegemos
		lstAudiosUse.DecUse();
		//Salimos
		Error("Mixer not found\n");
                return -1;
	}

	//Obtenemos el audio source
	AudioSource *audio = it->second.get();

        for ( Sidebars::iterator it = sidebars.begin();
              it != sidebars.end();
              it++ )
        {
            if (audio->sidebar ==  it->second.get())
            {
                lstAudiosUse.DecUse();
                return it->first;
            }
        }

        lstAudiosUse.DecUse();

        return -1;
}


/***********************************
 * AddSidebarParticipant
 *	Add a participant to be shown in a sidebar
 ************************************/
int AudioMixer::AddSidebarParticipant(int sidebarId, int partId)
{
	Log("-AddSidebarParticipant [sidebar:%d,partId:%d]\n",sidebarId,partId);

	//Verrou exclusif : mutation du set du sidebar, cf. §1.3
	lstAudiosUse.WaitUnusedAndLock();

	//Get the sidebar for the user
	Sidebars::iterator itSidebar = sidebars.find(sidebarId);

	//If not found
	if (itSidebar==sidebars.end())
	{
		//UnBlock
		lstAudiosUse.Unlock();
		//Salimos
		return Error("Sidebar not found\n");
	}

	//Add participant to the sidebar
	itSidebar->second->AddParticipant(partId);

	//UnBlock
	lstAudiosUse.Unlock();

	//Everything ok
	return 1;
}

/***********************************
 * RemoveSidebarParticipant
 *	Remove a participant to be shown in a sidebar
 ************************************/
int AudioMixer::RemoveSidebarParticipant(int sidebarId, int partId)
{
	Log(">-RemoveSidebarParticipant [sidebar:%d,partId:%d]\n",sidebarId,partId);

	//Verrou exclusif : mutation du set du sidebar, cf. §1.3
	lstAudiosUse.WaitUnusedAndLock();

	//Get the sidebar for the user
	Sidebars::iterator itSidebar = sidebars.find(sidebarId);

	//If not found
	if (itSidebar==sidebars.end())
	{
		//UnBlock
		lstAudiosUse.Unlock();
		//Salimos
		return Error("Sidebar not found\n");
	}

	//Get sidebar
	Sidebar* sidebar = itSidebar->second.get();

	//Remove participant to the sidebar
	sidebar->RemoveParticipant(partId);

	//UnBlock
	lstAudiosUse.Unlock();
		
	//Correct
	return 1;
}

int AudioMixer::CreateSidebar()
{
	//Verrou exclusif : mutation de la map, cf. §1.3
	lstAudiosUse.WaitUnusedAndLock();

	//Get id (sous le verrou : deux appels concurrents ne partagent plus un id)
	int id = numSidebars++;

	//add it
	sidebars[id] = std::make_unique<Sidebar>();

	//UnBlock
	lstAudiosUse.Unlock();

	return id;
}

int AudioMixer::DeleteSidebar(int sidebarId)
{

	//Block
	lstAudiosUse.WaitUnusedAndLock();

	//Get sidebar from id
	Sidebars::iterator it = sidebars.find(sidebarId);

	//Check if we have found it
	if (it==sidebars.end())
	{
		//UnBlock
		lstAudiosUse.Unlock();
		//error
		return Error("Sidebar not found [id:%d]\n",sidebarId);
	}

	//Get the old sidebar
	Sidebar *sidebar = it->second.get();

	//Le sidebar par défaut ne doit jamais être détruit : InitMixer/EndMixer
	//écrivent dans defaultSidebar sans re-vérifier son existence.
	if (sidebar == defaultSidebar)
	{
		//UnBlock
		lstAudiosUse.Unlock();
		//error
		return Error("Cannot delete default sidebar [id:%d]\n",sidebarId);
	}

	//For each audio
	for (Audios::iterator ita = audios.begin(); ita!= audios.end(); ++ita)
	{
		//Check it it has dis sidebar
		if (ita->second->sidebar == sidebar)
			//Set to null
			ita->second->sidebar = NULL;
	}

	//Remove sidebar (la destruction a lieu à la sortie, donc hors verrou)
	std::unique_ptr<Sidebar> owned = std::move(it->second);
	sidebars.erase(it);

	//UnBlock
	lstAudiosUse.Unlock();

	//Exit
	return 1;
}

DWORD AudioMixer::GetVAD(int id)
{
	DWORD acuVAD = 0;

	//Lock
	lstAudiosUse.IncUse();

	//Find it
	Audios::iterator it = audios.find(id);

	//If found
	if (it!=audios.end())
		//Get vad
		acuVAD = it->second->vad;

	//Unlock
	lstAudiosUse.DecUse();

	//Return VAD acumulated
	return acuVAD;
}

int AudioMixer::DumpMixerInfo(int sidebarId, std::string & info)
{
        lstAudiosUse.IncUse();
        char partId[40];
        
	Sidebars::iterator it = sidebars.find(sidebarId);

	//Check if we have found it
	if (it==sidebars.end())
	{
		//UnBlock
		lstAudiosUse.DecUse();
		//error
                snprintf(partId, sizeof(partId), "Sidebar %d: no such sidebar\n", sidebarId);
                info += partId;
		return 200;
	}

        std::set<int> parts = it->second->GetParticipants();
        
        
        snprintf(partId, sizeof(partId), "Sidebar %d (vadmode=%d): ", sidebarId, vad);
        info += partId;
        if (parts.empty()) info += "no participant";

        for (std::set<int>::iterator it2 = parts.begin(); 
             it2 != parts.end();
             it2++)
        {
            snprintf(partId, sizeof(partId), "%d ", *it2);
            info += partId;
        }

        info += "\n";

        lstAudiosUse.DecUse();
        return 200;
}
