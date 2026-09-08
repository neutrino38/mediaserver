/* 
 * File:   AudioTranscoder.cpp
 * Author: ebuu
 * 
 * Created on 7 août 2014, 00:07
 */

#include "AudioTranscoder.h"

AudioTranscoder::AudioTranscoder(const std::wstring & name) : tag(name)
{
    state = 0;
    recCodec = -1;
    allowBridging = false;
    nativeRate = 0;
}

AudioTranscoder::~AudioTranscoder()
{
	End();
}

int AudioTranscoder::Init(bool allowBriding)
{
    int ret = 1;
    this->allowBridging = allowBriding;
    //Encodeur POUSSÉ : plus de PipeAudioInput entre le décodeur et lui. La trame
    //décodée traverse l'encodeur sur le thread de la source (lot 3).
    ret = encoder.Init();
    if (ret > 0)
    {
        // Le décodeur nous livre ses trames : nous sommes son AudioOutput.
        ret = decoder.Init((AudioOutput*)this);
        if ( !ret )
        {
            Error("-JSR309 AudioTranscoder: failed to init audio decoder.");
            encoder.End();
        }
        if (allowBriding)
            state = 0; // Probing
        else
            state = 2; // Transcoding
    }
    else
        Error("-JSR309 AudioTranscoder: failed to init audio encoder.");


    return ret;
}

int AudioTranscoder::End()
{
    //AVANT d'arrêter quoi que ce soit : en mode pont la source tient un
    //Joinable::Listener* sur NOUS. MediaSession::AudioTranscoderDelete appelle
    //End() sans passer par Dettach(), et le destructeur l'appelle aussi — le
    //shared_ptr détruit ensuite l'objet, et la source publierait dans de la
    //mémoire libérée. La sûreté mémoire ne doit pas dépendre de l'ordre des
    //appels du contrôleur. Même correctif que VideoTranscoder (bad033e).
    UnlistenSource();
    decoder.End();
    encoder.End();
    return 1;
}

//── AudioOutput : la sortie du décodeur EST l'entrée de l'encodeur ───────────
//Le thread qui a livré le paquet RTP porte toute la chaîne, décodage compris :
//PlayFrame s'exécute donc sous le verrou de multiplexage du port source.
int AudioTranscoder::PlayFrame(SamplesPtr samples)
{
    return encoder.EncodeSamples(samples);
}

int AudioTranscoder::StartPlaying(DWORD samplerate)
{
    //Le décodeur annonce la fréquence native du flux. Rien à ouvrir : la trame
    //porte la sienne, et c'est l'encodeur qui rééchantillonne vers la sienne.
    nativeRate = samplerate;
    return 1;
}

int AudioTranscoder::StopPlaying()
{
    return 1;
}
             
void AudioTranscoder::AddListener(Joinable::Listener *listener)
{
	encoder.AddListener(listener);
}

//Phase 5 (nego_fmtp §6.3) : l'endpoint écoute le transcodeur, mais c'est son
//encodeur qui produit — les bornes descendent d'un cran. Sans objet en mode
//pont (bridging) : aucun encodeur dans le chemin.
void AudioTranscoder::SetNegotiatedCodecProperties(const std::map<int,Properties>& byCodec)
{
	encoder.SetNegotiatedCodecProperties(byCodec);
}

void AudioTranscoder::Update()
{
    //Do nothing - update not relevant for audio
}

void AudioTranscoder::SetREMB(DWORD estimation)
{
    //encoder.SetREMB(estimation);
}

void AudioTranscoder::RemoveListener(Joinable::Listener *listener)
{
	encoder.RemoveListener(listener);
}

void AudioTranscoder::onRTPPacket(RTPPacket &packet)
{
    if (allowBridging)
    {
        if ( recCodec != packet.GetCodec() || state == 0)
        {
            int ret = encoder.TryCodec(packet.GetCodec());

            if ( ret == packet.GetCodec() )
            {
                // endpoints support codes - no neet to transcode
                state = 2;
                Log("-AudioTranscoder: switched to bridged mode for codec %s.\n",
                    AudioCodec::GetNameFor( (AudioCodec::Type) packet.GetCodec()) );
            }
            else
            {
                state = 1;
                Log("-AudioTranscoder: switched to transcoder mode for codec %s.\n",
                    AudioCodec::GetNameFor( (AudioCodec::Type) packet.GetCodec()) );

            }
			
			recCodec = packet.GetCodec();
        }
        
        switch(state)
        {
            case 2: // Bridging
                encoder.Multiplex(packet);
                break;

            case 1:
            default:
                decoder.onRTPPacket(packet);
                break;

        }
    }
    else
    {
	decoder.onRTPPacket(packet);
    }
}
void AudioTranscoder::onResetStream()
{
	decoder.onResetStream();
}
void AudioTranscoder::onEndStream()
{
	decoder.onEndStream();
}


void AudioTranscoder::UnlistenSource()
{
	//lock() : la source est-elle encore vivante ?
	if (std::shared_ptr<Joinable> j = joined.lock())
		j->RemoveListener(this);

	joined.reset();
}

//Returning 0 here made every AudioTranscoderAttachToEndpoint/Dettach XML-RPC
//call answer an error while the attach had in fact happened.
int AudioTranscoder::Attach(const std::shared_ptr<Joinable> & join)
{

	if (!allowBridging)
    {
		decoder.Attach(join);
		return 1;
    }

	//Une source précédente ne doit pas continuer à nous publier des paquets : sans
	//ce retrait, un ré-attachement laisse le transcodeur inscrit auprès des DEUX,
	//et chaque paquet de l'ancienne traverse encore le pont.
	UnlistenSource();

	joined = join;

	//Le mode se rejuge sur le premier paquet de la NOUVELLE source : son codec
	//n'a aucune raison d'être celui de la précédente.
	state = 0;
	recCodec = -1;

	decoder.Start();
	if (join)
		join->AddListener(this);

	return 1;
}

int AudioTranscoder::Dettach()
{
	//En mode pont, c'est nous qui sommes inscrit auprès de la source : sans ce
	//retrait elle garderait un pointeur sur cet objet, et continuerait à publier
	//dedans après le détachement.
	UnlistenSource();

	//En mode transcodage seul, c'est le décodeur qui était inscrit et qui se
	//retire ; en mode pont, il n'était pas attaché et Dettach() se réduit à
	//l'arrêt de son worker — ce qu'on veut dans les deux cas.
	decoder.Dettach();
	return 1;
}

int AudioTranscoder::SetCodec(int codec)
{
    return encoder.SetCodec( (AudioCodec::Type) codec);
}

