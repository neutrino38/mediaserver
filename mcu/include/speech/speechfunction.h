#ifndef SPEECH_SPEECHFUNCTION_H
#define SPEECH_SPEECHFUNCTION_H

#include <string>

enum class SpeechFunction { Asr = 0, Tts = 1, Comp = 2 };

constexpr int SpeechFunctionCount = 3;

inline const char* SpeechFunctionName(SpeechFunction function)
{
	switch (function)
	{
		case SpeechFunction::Asr:	return "asr";
		case SpeechFunction::Tts:	return "tts";
		case SpeechFunction::Comp:	return "comp";
	}
	return "?";
}

inline bool ParseSpeechFunction(const std::string& name, SpeechFunction& function)
{
	for (int i = 0; i < SpeechFunctionCount; ++i)
		if (name == SpeechFunctionName((SpeechFunction)i))
		{
			function = (SpeechFunction)i;
			return true;
		}
	return false;
}

#endif
