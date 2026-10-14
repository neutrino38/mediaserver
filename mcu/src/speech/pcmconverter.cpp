#include "speech/pcmconverter.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}
#include "log.h"

PcmConverter::PcmConverter(int inputRate)
{
	AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
	if (swr_alloc_set_opts2(&swr, &mono, AV_SAMPLE_FMT_S16, OutputRate,
				&mono, AV_SAMPLE_FMT_FLT, inputRate, 0, nullptr) < 0
	    || swr_init(swr) < 0)
	{
		Error("-PcmConverter: cannot resample %d Hz to %d Hz\n", inputRate, OutputRate);
		swr_free(&swr);
	}
}

PcmConverter::~PcmConverter()
{
	swr_free(&swr);
}

std::string PcmConverter::Convert(const std::string& float32)
{
	partial += float32;
	const size_t usable = partial.size() - partial.size() % sizeof(float);
	if (!usable)
		return "";

	const std::string samples = partial.substr(0, usable);
	partial.erase(0, usable);
	return Run((const float*)samples.data(), usable / sizeof(float));
}

std::string PcmConverter::Drain()
{
	partial.clear();
	return Run(nullptr, 0);
}

std::string PcmConverter::Run(const float* samples, int count)
{
	if (!swr)
		return "";

	const int capacity = swr_get_out_samples(swr, count);
	if (capacity <= 0)
		return "";

	std::string out(capacity * sizeof(int16_t), '\0');
	uint8_t* output = (uint8_t*)&out[0];
	const uint8_t* input = (const uint8_t*)samples;

	const int produced = swr_convert(swr, &output, capacity, samples ? &input : nullptr, count);
	if (produced < 0)
	{
		Error("-PcmConverter: swr_convert failed (%d)\n", produced);
		return "";
	}
	out.resize(produced * sizeof(int16_t));
	return out;
}
