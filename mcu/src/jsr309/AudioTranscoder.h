/* 
 * File:   AudioTranscoder.h
 * Author: ebuu
 *
 * Created on 7 août 2014, 00:07
 */

#include "AudioEncoderWorker.h"
#include "AudioDecoderWorker.h"
#include "medkit/codecs.h"

#ifndef AUDIOTRANSCODER_H
#define	AUDIOTRANSCODER_H

class AudioEncoderWorker;
class AudioDecoderWorker;

//Transcodeur audio d'une jambe JSR-309. Depuis le lot 3 de
//`jsr309_transcode_sans_thread.md`, il est LUI-MÊME l'AudioOutput de son
//décodeur : la trame décodée traverse l'encodeur sur le thread de la source,
//sans PipeAudioInput ni thread entre les deux.
class AudioTranscoder :
	public Joinable,
	public Joinable::Listener,
	public AudioOutput
{
public:
    AudioTranscoder(const std::wstring & name);
    virtual ~AudioTranscoder();
    
    
    int Init(bool allowBriding= false);
    int End();
    int SetCodec(int c); 

    const std::wstring& GetName() { return tag;	}
    
    
	//Joinable interface
	virtual void AddListener(Joinable::Listener *listener);
	virtual void Update();
	virtual void SetREMB(DWORD estimation);
	virtual void RemoveListener(Joinable::Listener *listener);
	//Phase 5 : les bornes négociées de la patte émettrice descendent à l'encodeur.
	virtual void SetNegotiatedCodecProperties(const std::map<int,Properties>& byCodec);

	//Virtuals from AudioOutput : le décodeur nous livre ici ses trames, et nous
	//les passons à l'encodeur sans les mettre en file.
	virtual int PlayFrame(SamplesPtr samples);
	virtual int StartPlaying(DWORD samplerate);
	virtual int StopPlaying();
	virtual DWORD GetNativeRate()	{ return nativeRate;	}
	virtual DWORD GetPlayingRate()	{ return nativeRate;	}

	//Virtuals from Joinable::Listener
	virtual void onRTPPacket(RTPPacket &packet);
	virtual void onResetStream();
	virtual void onEndStream();

	//Attach
	int Attach(const std::shared_ptr<Joinable> & join);
	int Dettach();


private:
	//Retire le transcodeur des listeners de la source courante, s'il y est.
	//Appelé par Attach, Dettach et End : en mode pont c'est LUI qui est inscrit
	//comme listener, donc c'est lui qui doit se retirer.
	void UnlistenSource();

    int state; // 0 = probbing, 1= transcoding, 2=forwarding
    int recCodec;
    bool allowBridging;
    AudioDecoderJoinableWorker decoder;
    AudioEncoderMultiplexerWorker encoder;
    std::wstring tag;
    //Fréquence native annoncée par le décodeur (StartPlaying). Purement
    //informative : c'est la trame qui porte sa fréquence, et l'encodeur
    //rééchantillonne vers la sienne.
    DWORD nativeRate;
    //Source écoutée en mode pont (lien retour non possédant, comme
    //AudioDecoderJoinableWorker::joined). Vide en mode transcodage seul : c'est
    //alors le décodeur qui est inscrit auprès de la source, et lui qui se retire.
    std::weak_ptr<Joinable> joined;
};

#endif	/* AUDIOTRANSCODER_H */

