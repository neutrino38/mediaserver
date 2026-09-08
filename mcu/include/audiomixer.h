#ifndef _AUDIOMIXER_H_
#define _AUDIOMIXER_H_
#include <pthread.h>
#include <use.h>
#include "worker.h"
#include <audio.h>
#include "pipeaudioinput.h"
#include "pipeaudiooutput.h"
#include "sidebar.h"
#include <map>
#include <memory>
#include <atomic>

class AudioMixer : public VADProxy, public Worker
{
public:
	AudioMixer();
	~AudioMixer();

	int Init(bool vad,DWORD rate = 16000);
	void SetVADMode(int vad) { this->vad = vad; }
	int CreateMixer(int id);
	int InitMixer(int id,int sidebarId);
	int SetMixerSidebar(int id,int sidebarId);
        int GetMixerSidebar(int id);
	int EndMixer(int id);
	int DeleteMixer(int id);
	AudioInput*  GetInput(int id);
	AudioOutput* GetOutput(int id);
	//Co-propriété (Point 1 / C-4) : rendent une copie de shared_ptr sur le pipe,
	//pour que le stream participant le maintienne vivant tant qu'il l'utilise.
	std::shared_ptr<AudioInput>  GetSharedInput(int id);
	std::shared_ptr<AudioOutput> GetSharedOutput(int id);
	void Process(void);

	int CreateSidebar();
	int AddSidebarParticipant(int SidebarId,int partId);
	int RemoveSidebarParticipant(int SidebarId,int partId);
	int DeleteSidebar(int SidebarId);
	int End();

	//VAD proxy interface
	virtual DWORD GetVAD(int id);
        int DumpMixerInfo(int sidebarId, std::string & info);
protected:
	//Mix thread (corps du Worker)
	virtual int Run();

private:

	//Tipos
	typedef struct
	{
		std::shared_ptr<PipeAudioInput>  input;
		std::shared_ptr<PipeAudioOutput> output;
		//Alignement exigé par _mm_load/_mm_store_si128 dans le mix-minus
		alignas(16) SWORD buffer[Sidebar::MIXER_BUFFER_SIZE];
		DWORD		len;
		Sidebar*	sidebar;
		DWORD		vad;
	} AudioSource;

	typedef std::map<int,std::unique_ptr<AudioSource>>	Audios;
	typedef std::map<int,std::unique_ptr<Sidebar>>		Sidebars;

private:
	std::atomic<bool> mixingAudio;
	Use		lstAudiosUse;
	
	Audios		audios;
	Sidebars	sidebars;
	Sidebar*	defaultSidebar;
	int		numSidebars;
	bool		vad;
	DWORD		rate;
};

#endif
