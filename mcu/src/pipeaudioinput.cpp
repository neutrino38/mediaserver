#include "log.h"
#include "pipeaudioinput.h"

PipeAudioInput::PipeAudioInput()
{
	inited = false;
	recording = false;
	canceled = false;
	recordRate = 0;
	nativeRate = 0;
}

DWORD PipeAudioInput::QueuedMs() const
{
	DWORD ms = 0;
	for (std::deque<SamplesPtr>::const_iterator it=queue.begin(); it!=queue.end(); ++it)
	{
		DWORD rate = (*it)->GetRate();
		if (rate) ms += (*it)->GetNbSamples() * 1000 / rate;
	}
	return ms;
}

SamplesPtr PipeAudioInput::RecFrame(DWORD timeoutMs)
{
	std::unique_lock<std::mutex> lock(mutex);

	if (!cond.wait_for(lock, std::chrono::milliseconds(timeoutMs),
			[this]{ return !recording || canceled || !queue.empty(); }))
		//Timeout : rien n'est arrivé
		return NULL;

	if (canceled)
	{
		canceled = false;
		Log("PipeAudioInput: RecFrame cancelled.\n");
		return NULL;
	}

	if (queue.empty())
		return NULL;

	SamplesPtr samples = queue.front();
	queue.pop_front();
	return samples;
}

int PipeAudioInput::StartRecording(DWORD rate)
{
	Log("-PipeAudioInput start recording [rate:%d Hz]\n",rate);

	std::lock_guard<std::mutex> lock(mutex);

	//Changer de fréquence de sortie invalide ce qui est en file et le resampler.
	if (recordRate != rate)
	{
		resampler.Reset();
		queue.clear();
	}

	recordRate = rate;
	recording = true;

	return true;
}

int PipeAudioInput::StopRecording()
{
	Log("-PipeAudioInput stop recording\n");

	std::lock_guard<std::mutex> lock(mutex);

	recording = false;

	cond.notify_one();

	return true;
}

int PipeAudioInput::PutFrame(SamplesPtr samples)
{
	if (!samples || samples->GetNbSamples() == 0)
		return 0;

	std::lock_guard<std::mutex> lock(mutex);

	//Personne n'écoute : inutile de rééchantillonner.
	if (!recording)
		return true;

	SamplesPtr out = resampler.Resample(samples, recordRate);
	if (!out)
		return Error("-PipeAudioInput could not transrate\n");

	//Entrée mise en tampon par le convertisseur : rien à mettre en file.
	if (out->GetNbSamples() == 0)
		return true;

	//Débordement : on vide plutôt que de bloquer le producteur (politique
	//historique), la latence primant sur l'intégrité du flux.
	if (QueuedMs() + out->GetNbSamples()*1000/out->GetRate() > MaxQueuedMs)
	{
		Log("-PipeAudioInput: file pleine (%u ms), on la vide\n", QueuedMs());
		queue.clear();
	}

	queue.push_back(out);

	cond.notify_one();

	return true;
}

int PipeAudioInput::Init(DWORD rate)
{
	Log("-PipeAudioInput init [rate:%d]\n",rate);

	std::lock_guard<std::mutex> lock(mutex);

	inited = true;
	//Ne sert plus qu'à PutSamples, qui n'a pas d'autre moyen de dire sa
	//fréquence. Les producteurs migrés la portent sur leurs trames.
	nativeRate = rate;

	return true;
}

int PipeAudioInput::PutSamples(SWORD *buffer,DWORD size)
{
	if (!buffer || size == 0)
		return 0;

	DWORD rate = nativeRate;
	if (!rate)
		return Error("-PipeAudioInput: rate unknown, Init() missing\n");

	SamplesPtr samples = Samples::FromBuffer(buffer, size, rate);
	if (!samples)
		return Error("-PipeAudioInput: could not allocate %u samples\n", size);

	return PutFrame(samples);
}

int PipeAudioInput::End()
{
	{
		std::lock_guard<std::mutex> lock(mutex);

		inited = false;
		recording = false;
		queue.clear();

		//Libère le resampler sous mutex (PutFrame l'utilise sous ce même mutex)
		resampler.Reset();

		cond.notify_one();
	}

	return true;
}

void PipeAudioInput::CancelRecFrame()
{
	std::lock_guard<std::mutex> lock(mutex);

	canceled = true;

	cond.notify_one();
}
