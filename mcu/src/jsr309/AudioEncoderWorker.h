/* 
 * File:   AudioEncoderWorker.h
 * Author: Sergio
 *
 * Created on 4 de octubre de 2011, 20:42
 */

#ifndef AUDIOENCODERWORKER_H
#define	AUDIOENCODERWORKER_H
#include <atomic>
#include <memory>
#include <mutex>
#include "medkit/codecs.h"
#include "audio.h"
#include "audioresampler.h"
#include "worker.h"
#include "RTPMultiplexer.h"

//Encodeur audio d'une patte émettrice JSR-309. UN SEUL corps de traitement,
//`EncodeSamples`, et deux façons de l'alimenter (lot 3 de
//`jsr309_transcode_sans_thread.md`) :
//  - TIRÉ (port de mixeur, `Init(AudioInput*)`) : un thread pompe le mixeur,
//    qui produit à sa propre cadence ;
//  - POUSSÉ (transcodeur, `Init()`) : le décodeur appelle `EncodeSamples` sur
//    le thread de la source, sans file ni thread intermédiaire.
class AudioEncoderMultiplexerWorker :
	public RTPMultiplexer,
	public Worker
{
public:
	AudioEncoderMultiplexerWorker();
	virtual ~AudioEncoderMultiplexerWorker();

	//Mode TIRÉ : le worker va chercher ses trames dans `input`.
	int Init(AudioInput *input);
	//Mode POUSSÉ : les trames arrivent par EncodeSamples, sans thread.
	int Init();
	int SetCodec(AudioCodec::Type codec);
	int End();

	//Encode et multiplexe une trame décodée, à la fréquence qu'elle porte, et
	//rend le nombre de paquets émis. Corps commun aux deux modes.
	int EncodeSamples(SamplesPtr samples);

	//Joinable interface
	virtual void AddListener(Listener *listener);
	virtual void Update();
	virtual void RemoveListener(Listener *listener);
	//Phase 5 (nego_fmtp §6.3) : bornes négociées par code codec, passées à
	//AudioCodecFactory::CreateEncoder à l'ouverture (Opus : useinbandfec,
	//usedtx, maxaveragebitrate, cbr déclarés par le pair).
	virtual void SetNegotiatedCodecProperties(const std::map<int,Properties>& byCodec);

	int Start();
	int Stop();
protected:
	int Encode();
	//Corps du Worker (mode tiré uniquement)
	virtual int Run() { return Encode(); }

private:
	//Ouvre l'encodeur si besoin : premier appel, codec changé, ou configuration
	//réécrite par le plan de contrôle. À appeler sous `encodeLock`.
	bool EnsureEncoder();

private:
	AudioInput	*input;
	//Aucune source à tirer : les trames sont poussées (transcodeur).
	bool		pushed;
	//Le chemin est-il ouvert ? Écrit par le plan de contrôle, lu par le chemin
	//des paquets, qui n'est plus séparé de lui par l'arrêt d'un thread.
	std::atomic<bool>	encoding;

	//── Plan de contrôle (thread XML-RPC) ────────────────────────────────────
	//Codec et bornes négociées, écrits sous ce verrou court, signalés par un
	//drapeau que le chemin des paquets consomme au tour suivant. Un seul motif
	//pour SetCodec et SetNegotiatedCodecProperties, au lieu du Stop/Start de
	//l'un et du drapeau de l'autre (§4.4).
	std::mutex		configLock;
	AudioCodec::Type	codec;
	std::map<int,Properties> negotiated;
	std::atomic<bool>	configDirty;

	//── Chemin des paquets ───────────────────────────────────────────────────
	//Sans thread, Stop() ne joint plus rien : c'est ce verrou qui empêche le
	//plan de contrôle de détruire l'encodeur sous une trame en cours (§4.3).
	//Ordre : Port(source).mutex -> encodeLock -> configLock. Jamais l'inverse.
	std::mutex	encodeLock;
	AudioEncoder	*audioEncoder;
	//Recréé avec l'encodeur : il porte le code codec, l'horloge et le SSRC du
	//run courant.
	std::unique_ptr<RTPPacket>	packet;
	//L'encodeur travaille à SA fréquence ; la trame porte la sienne.
	AudioResampler	resampler;
	DWORD		encoderRate;
	DWORD		frameTime;
	float		multiplier;
};

#endif	/* AUDIOENCODERWORKER_H */
