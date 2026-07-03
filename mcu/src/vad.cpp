#include "vad.h"
#include "log.h"
#ifdef VADWEBRTC

/*
 * L'APM impose des trames de 10 ms exactement, a un "native rate"
 * (8/16/32/48 kHz), en int16 entrelace. CalcVad decoupe le buffer recu en
 * blocs de 10 ms, les fait traiter par ProcessStream() puis agrege la
 * decision voix (OU logique), de facon a renvoyer le meme 0/1 par appel que
 * l'ancienne implementation.
 */

VAD::VAD()
{
	//No vad decision yet
	last = 0;
	//Create the webrtc audio processing module
	apm = webrtc::AudioProcessing::Create();
	if (apm)
	{
		//Operate the VAD on 10ms frames
		apm->voice_detection()->set_frame_size_ms(10);
		//Enable voice detection
		apm->voice_detection()->Enable(true);
		//Set aggressive mode (comportement historique)
		SetMode(VERYAGGRESIVE);
	}
	else
	{
		Error("VAD: could not create webrtc AudioProcessing instance.\n");
	}
}

VAD::~VAD()
{
	if (apm)
		delete apm;
}

int VAD::CalcVad(SWORD* buffer,DWORD size,DWORD rate)
{
	//Check we have an instance
	if (!apm)
		return 0;

	//Only native rates are accepted by the int16 APM interface
	if (rate != 8000 && rate != 16000 && rate != 32000 && rate != 48000)
	{
		Error("VAD: Cannot use sample rate = %u for VAD.\n", rate);
		return 0;
	}

	//Number of samples in a 10ms mono frame
	DWORD chunk = rate / 100;

	//Not enough data for a single frame
	if (chunk == 0 || size < chunk)
		return 0;

	webrtc::AudioFrame frame;
	int voice = 0;

	//Process the buffer in 10ms chunks (le reliquat < 10ms est ignore)
	for (DWORD off = 0; off + chunk <= size; off += chunk)
	{
		//Feed the mono 10ms frame
		frame.UpdateFrame(0, 0, buffer + off, chunk, rate,
				  webrtc::AudioFrame::kNormalSpeech,
				  webrtc::AudioFrame::kVadUnknown, 1);

		//Process it
		if (apm->ProcessStream(&frame) != webrtc::AudioProcessing::kNoError)
			continue;

		//Accumulate voice decision
		if (apm->voice_detection()->stream_has_voice())
			voice = 1;
	}

	//Store and return
	last = voice;
	return voice;
}

bool VAD::SetMode(Mode mode)
{
	if (!apm)
		return false;

	//L'echelle de vraisemblance de l'APM est inverse de l'agressivite :
	//plus la vraisemblance est haute, moins le VAD est agressif.
	webrtc::VoiceDetection::Likelihood likelihood;
	switch (mode)
	{
		case QUALITY:
			likelihood = webrtc::VoiceDetection::kHighLikelihood;
			break;
		case LOWBITRATE:
			likelihood = webrtc::VoiceDetection::kModerateLikelihood;
			break;
		case AGGRESSIVE:
			likelihood = webrtc::VoiceDetection::kLowLikelihood;
			break;
		case VERYAGGRESIVE:
		default:
			likelihood = webrtc::VoiceDetection::kVeryLowLikelihood;
			break;
	}

	return apm->voice_detection()->set_likelihood(likelihood) == webrtc::AudioProcessing::kNoError;
}

int VAD::GetVAD()
{
	return last;
}
#endif
