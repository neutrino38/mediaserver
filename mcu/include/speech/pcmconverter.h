#ifndef SPEECH_PCMCONVERTER_H
#define SPEECH_PCMCONVERTER_H

#include <string>

struct SwrContext;

/**
 * Mistral TTS audio (PCM float32 little-endian, mono) to the streaming format
 * (PCM S16LE mono 16 kHz), with libswresample. Input may be cut anywhere,
 * even inside a sample.
 */
class PcmConverter
{
public:
	static constexpr int OutputRate = 16000;

	explicit PcmConverter(int inputRate);
	~PcmConverter();
	PcmConverter(const PcmConverter&) = delete;
	PcmConverter& operator=(const PcmConverter&) = delete;

	bool IsValid() const	{ return swr != nullptr; }

	// S16LE samples for these float32 bytes; "" while they make no sample yet.
	std::string Convert(const std::string& float32);

	// What the resampler still holds, at the end of a phrase.
	std::string Drain();

private:
	std::string Run(const float* samples, int count);

	SwrContext*	swr = nullptr;
	std::string	partial;	// bytes of an incomplete float
};

#endif
