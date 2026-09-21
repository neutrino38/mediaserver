/*
 * File:   vad.h
 * Author: Sergio
 *
 * Created on 13 de agosto de 2012, 10:10
 *
 * Reimplemente sur webrtc-audio-processing (module AudioProcessing/APM).
 * L'ancienne API bas niveau webrtc (WebRtcVad_*, VadInstT, issue du "trunk"
 * clone via webrtc_stack) n'est plus disponible : on passe par le composant
 * VoiceDetection de l'APM fourni par le paquet webrtc-audio-processing-devel.
 */

#ifndef VAD_H
#define	VAD_H
#include "config.h"

class VADProxy
{
public:
	virtual DWORD GetVAD(int id) = 0;
};

#ifdef VADWEBRTC

// Deux APM coexistent selon la distribution, et leurs en-tetes ne se rangent
// pas au meme endroit : webrtc-audio-processing 0.3 (AlmaLinux 9) publie sous
// webrtc/, la 1.x (Debian/Ubuntu) a la racine de son propre includedir.
// WEBRTC_APM_1 est pose par mcu/Makefile selon le module pkg-config trouve.
#ifdef WEBRTC_APM_1
#include <modules/audio_processing/include/audio_processing.h>
#else
#include <webrtc/modules/audio_processing/include/audio_processing.h>
#include <webrtc/modules/interface/module_common_types.h>
#endif

class VAD
{
public:
	typedef enum { QUALITY=0,LOWBITRATE=1,AGGRESSIVE=2,VERYAGGRESIVE=3} Mode;
public:
	VAD();
	~VAD();

	bool SetMode(Mode mode);
	int CalcVad(SWORD* frame,DWORD size, DWORD rate);
	int GetVAD();
	bool IsRateSupported(DWORD rate ) { return ( rate == 8000 || rate == 16000 || rate == 32000 ); }
private:
	webrtc::AudioProcessing* apm;
	int last;
};
#else
class VAD
{
public:
	VAD(){};
	int CalcVad(SWORD* frame,DWORD size, DWORD rate) { return 0; }
	int GetVAD()			 { return 0; }
	bool IsRateSupported(DWORD rate ) { return 0; }
};
#endif
#endif	/* VAD_H */
