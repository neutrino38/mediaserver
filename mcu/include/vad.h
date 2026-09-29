/*
 * File:   vad.h
 * Author: Sergio
 *
 * Created on 13 de agosto de 2012, 10:10
 *
 * Bati sur libfvad (sous-module third_party/libvad) : le moteur VAD de WebRTC,
 * extrait en bibliotheque C autonome. Voir docs/reference/vad.md.
 */

#ifndef VAD_H
#define	VAD_H
#include "config.h"
#include <fvad.h>

class VADProxy
{
public:
	virtual DWORD GetVAD(int id) = 0;
};

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
	bool IsRateSupported(DWORD rate ) { return ( rate == 8000 || rate == 16000 || rate == 32000 || rate == 48000 ); }
private:
	Fvad* fvad;
	DWORD sampleRate;
	int last;
};
#endif	/* VAD_H */
