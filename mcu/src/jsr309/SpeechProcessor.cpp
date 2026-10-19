/*
 * SpeechProcessor.cpp — ports, relais, mix et bruit de confort
 * (SPEC docs/conception/SPEECH-PROCESSOR §4, §9, §10.4).
 */
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
#include <vector>
#include "log.h"
#include "text.h"
#include "redcodec.h"
#include "textmux.h"
#include "audioresampler.h"
#include "AudioDecoderWorker.h"
#include "AudioEncoderWorker.h"
#include "RTPMultiplexer.h"
#include "SpeechProcessor.h"

namespace
{

//Le texte d'un paquet T.140 tient dans un paquet RTP, redondance comprise.
const size_t MaxTextPerPacket = 400;

//Bruit blanc à niveau donné en dBov, sur un tirage à graine fixe : un test
//retrouve la même suite d'échantillons à chaque passage.
class ComfortNoise
{
public:
	void SetLevel(int dbov)
	{
		//Bruit uniforme sur [-a, a] : sa valeur efficace vaut a/sqrt(3)
		amplitude = dbov >= 0 ? 0 : (int)(32768.0 * std::pow(10.0, dbov / 20.0) * std::sqrt(3.0) + 0.5);
	}

	int Next()
	{
		if (!amplitude)
			return 0;
		state = state * 1664525u + 1013904223u;
		return (int)((state >> 8) % (2u * amplitude + 1)) - amplitude;
	}

private:
	uint32_t	state = 0x5EEDu;
	int		amplitude = 0;
};

//Le dernier octet complet d'une coupe : ne jamais séparer une séquence UTF-8.
size_t Utf8Cut(const std::string& text, size_t max)
{
	if (text.size() <= max)
		return text.size();
	size_t cut = max;
	while (cut > 0 && ((unsigned char)text[cut] & 0xC0) == 0x80)
		--cut;
	return cut;
}

}

/**
 * Un côté du SpeechProcessor.
 */
class SpeechProcessor::Port
{
public:
	//Entrée audio : le décodeur dépose ici, sur le thread de la source. Une
	//conversion vers MixRate, un anneau borné, rien d'autre.
	class AudioSink : public AudioOutput
	{
	public:
		explicit AudioSink(Port& port) : port(port) {}

		virtual DWORD GetNativeRate()		{ return MixRate; }
		virtual DWORD GetPlayingRate()		{ return MixRate; }
		virtual int StartPlaying(DWORD rate)	{ return 1; }
		virtual int StopPlaying()		{ port.ClearRelay(); return 1; }
		virtual int PlayFrame(SamplesPtr samples)
		{
			port.PushRelay(samples);
			return 1;
		}

	private:
		Port& port;
	};

	//Entrée texte : RED (RFC 4103) ou T.140 nu, comme Recorder::onTextPacket.
	//Le texte part vers la sortie de l'autre port.
	class TextSink : public Joinable::Listener, public TextOutput
	{
	public:
		explicit TextSink(Port& port) : port(port) {}
		virtual ~TextSink()	{ Dettach(); }

		int Attach(const std::shared_ptr<Joinable>& join)
		{
			Dettach();
			{
				std::lock_guard<std::mutex> lock(mutex);
				joined = join;
			}
			//Hors de notre verrou : la source appelle onEndStream sous le
			//sien, l'ordre inverse serait une étreinte fatale.
			if (join)
				join->AddListener(this);
			return 1;
		}

		int Dettach()
		{
			std::shared_ptr<Joinable> previous;
			{
				std::lock_guard<std::mutex> lock(mutex);
				previous = joined.lock();
				joined.reset();
			}
			//RemoveListener est la barrière : au retour, plus aucun paquet
			//de cette source n'est en vol ici.
			if (previous)
				previous->RemoveListener(this);
			return 1;
		}

		virtual void onRTPPacket(RTPPacket& packet)
		{
			if (packet.GetMedia() != MediaFrame::Text)
				return;
			if ((TextCodec::Type)packet.GetCodec() == TextCodec::T140RED)
				red.Decode((RTPRedundantPacket*)&packet, this);
			else
			{
				TextFrame frame(packet.GetTimestamp(), packet.GetMediaData(), packet.GetMediaLength());
				SendFrame(frame);
			}
		}
		virtual void onResetStream()	{}
		virtual void onEndStream()
		{
			std::lock_guard<std::mutex> lock(mutex);
			joined.reset();
		}

		virtual int SendFrame(TextFrame& frame)
		{
			if (frame.GetLength())
				port.owner->OnTextIn(port.index, std::string((const char*)frame.GetData(), frame.GetLength()));
			return 1;
		}

	private:
		Port&			port;
		std::mutex		mutex;
		std::weak_ptr<Joinable>	joined;
		//Thread de la source seulement
		RedundentCodec		red;
	};

	//Sortie texte : du T.140 RED si l'endpoint l'accepte, du T.140 nu sinon.
	class TextOut : public RTPMultiplexer
	{
	public:
		//Thread du tour seulement
		void Send(const std::string& text, uint64_t nowMs)
		{
			const bool useRed = TryCodec(TextCodec::T140RED) == TextCodec::T140RED;
			if (useRed)
			{
				TextFrame frame((DWORD)nowMs, (const BYTE*)text.data(), text.size());
				Emit(red.Encode(&frame, TextCodec::T140));
			}
			else
			{
				RTPPacket packet(MediaFrame::Text, TextCodec::T140);
				packet.SetTimestamp((DWORD)nowMs);
				packet.SetPayload((BYTE*)text.data(), text.size());
				packet.SetSeqNum(seq++);
				Multiplex(packet);
			}
			lastSentMs = nowMs;
		}

		//La redondance repart tant que la dernière frappe n'a pas été émise
		//deux fois de plus (RFC 4103).
		void SendRedundancy(uint64_t nowMs)
		{
			if (red.EncoderIsIdle() || nowMs - lastSentMs < TextBufferMs)
				return;
			if (TryCodec(TextCodec::T140RED) != TextCodec::T140RED)
				return;
			Emit(red.EncodeNull(TextCodec::T140));
			lastSentMs = nowMs;
		}

		bool CanSend(uint64_t nowMs) const	{ return nowMs - lastSentMs >= TextBufferMs; }
		bool HasListeners()
		{
			std::lock_guard<std::mutex> lock(mutex);
			return !listeners.empty();
		}

	private:
		void Emit(RTPRedundantPacket* packet)
		{
			if (!packet)
				return;
			packet->SetSeqNum(seq++);
			Multiplex(*packet);
			delete packet;
		}

		RedundentCodec	red;
		WORD		seq = 0;
		uint64_t	lastSentMs = 0;
	};

	Port(SpeechProcessor* owner, int index) :
		owner(owner),
		index(index),
		audioSink(*this),
		textSink(*this)
	{
		noise.SetLevel(DefaultComfortNoiseDbov);
		decoder.Init(&audioSink);
		encoder.Init();
	}

	void PushRelay(SamplesPtr samples)
	{
		std::lock_guard<std::mutex> lock(audioMutex);
		SamplesPtr in = resampler.Resample(samples, MixRate);
		if (!in)
			return;
		const SWORD* data = in->GetData();
		for (DWORD i = 0; i < in->GetNbSamples(); ++i)
			relay.push_back(data[i]);
		//Au-delà de l'anneau, le plus ancien part : la latence ne dérive pas
		const size_t max = RelayMaxMs * MixRate / 1000;
		while (relay.size() > max)
			relay.pop_front();
	}

	void ClearRelay()
	{
		std::lock_guard<std::mutex> lock(audioMutex);
		relay.clear();
		primed = false;
		resampler.Reset();
	}

	//Une trame de relais, complétée de zéros si l'anneau manque.
	void TakeRelay(std::vector<int>& mix)
	{
		std::lock_guard<std::mutex> lock(audioMutex);
		if (!primed)
		{
			if (relay.size() < (size_t)(RelayStartMs * MixRate / 1000))
				return;
			primed = true;
		}
		for (int i = 0; i < FrameSamples && !relay.empty(); ++i)
		{
			mix[i] += relay.front();
			relay.pop_front();
		}
		//Anneau vide : on reprend l'avance avant de rejouer
		if (relay.empty())
			primed = false;
	}

	int BufferedMs()
	{
		std::lock_guard<std::mutex> lock(audioMutex);
		return relay.size() * 1000 / MixRate;
	}

	SpeechProcessor*	owner;
	const int		index;

	AudioSink			audioSink;
	AudioDecoderJoinableWorker	decoder;
	AudioEncoderMultiplexerWorker	encoder;
	TextSink			textSink;
	TextOut				textOut;

	//Anneau du relais : décodeur (écrit) et tour (lit)
	std::mutex		audioMutex;
	std::deque<SWORD>	relay;
	bool			primed = false;
	AudioResampler		resampler;

	//Bruit de confort : réglé par le plan de contrôle, tiré par le tour
	std::mutex		noiseMutex;
	ComfortNoise		noise;

	//Texte sortant : rempli par le texte entrant de l'autre port, vidé par le tour
	std::mutex		textMutex;
	TextMux			mux;
	std::string		pendingText;
};

SpeechProcessor::SpeechProcessor(const std::wstring& tag) :
	tag(tag)
{
	for (int i = 0; i < Ports; ++i)
		ports[i] = std::make_shared<Port>(this, i);
}

SpeechProcessor::~SpeechProcessor()
{
	End();
}

uint64_t SpeechProcessor::NowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

int SpeechProcessor::Init(bool clocked)
{
	Log("-SpeechProcessor: init [%ls]\n", tag.c_str());
	this->clocked = clocked;
	lastTickMs = NowMs();
	if (clocked)
		StartThread();
	return 1;
}

int SpeechProcessor::End()
{
	//Même sans Init() : un Attach a pu brancher une entrée, qu'il faut retirer
	//avant que l'anneau qu'elle remplit ne soit détruit
	if (ended.exchange(true))
		return 0;

	//Le tour d'abord : plus personne n'encode ni n'émet
	StopThread();

	for (int i = 0; i < Ports; ++i)
	{
		//Retirer les entrées : au retour, plus aucun paquet en vol chez nous
		ports[i]->decoder.End();
		ports[i]->textSink.Dettach();
		ports[i]->encoder.End();
	}

	Log("-SpeechProcessor: ended [%ls]\n", tag.c_str());
	return 1;
}

int SpeechProcessor::SetPortCodec(int port, AudioCodec::Type codec)
{
	if (!IsValidPort(port))
		return Error("-SpeechProcessor: invalid port %d\n", port);
	return ports[port]->encoder.SetCodec(codec);
}

int SpeechProcessor::SetComfortNoise(int port, int dbov)
{
	if (!IsValidPort(port))
		return Error("-SpeechProcessor: invalid port %d\n", port);
	if (dbov > 0 || dbov < MinComfortNoiseDbov)
		return Error("-SpeechProcessor: comfort noise %d dBov out of [%d, 0]\n", dbov, MinComfortNoiseDbov);
	std::lock_guard<std::mutex> lock(ports[port]->noiseMutex);
	ports[port]->noise.SetLevel(dbov);
	return 1;
}

std::shared_ptr<Joinable> SpeechProcessor::GetJoinable(int port, MediaFrame::Type media)
{
	if (!IsValidPort(port) || !IsSupportedMedia(media))
		return nullptr;
	const std::shared_ptr<Port>& p = ports[port];
	if (media == MediaFrame::Audio)
		return std::shared_ptr<Joinable>(p, &p->encoder);
	return std::shared_ptr<Joinable>(p, &p->textOut);
}

int SpeechProcessor::Attach(int port, MediaFrame::Type media, const std::shared_ptr<Joinable>& join)
{
	if (!IsValidPort(port))
		return Error("-SpeechProcessor: invalid port %d\n", port);
	if (!IsSupportedMedia(media))
		return Error("-SpeechProcessor: %s cannot go through a SpeechProcessor\n", MediaFrame::TypeToString(media));
	if (!join)
		return Error("-SpeechProcessor: nothing to attach\n");
	if (media == MediaFrame::Audio)
		return ports[port]->decoder.Attach(join);
	return ports[port]->textSink.Attach(join);
}

int SpeechProcessor::Dettach(int port, MediaFrame::Type media)
{
	if (!IsValidPort(port))
		return Error("-SpeechProcessor: invalid port %d\n", port);
	if (!IsSupportedMedia(media))
		return Error("-SpeechProcessor: %s cannot go through a SpeechProcessor\n", MediaFrame::TypeToString(media));
	if (media == MediaFrame::Audio)
		ports[port]->decoder.Dettach();
	else
		ports[port]->textSink.Dettach();
	return 1;
}

int SpeechProcessor::GetRelayBufferedMs(int port)
{
	return IsValidPort(port) ? ports[port]->BufferedMs() : 0;
}

void SpeechProcessor::OnTextIn(int fromPort, const std::string& utf8)
{
	Port& to = *ports[1 - fromPort];
	std::lock_guard<std::mutex> lock(to.textMutex);
	//Source 0 du multiplexeur : le texte tapé de l'autre côté
	to.mux.Push(0, utf8, lastTickMs);
}

void SpeechProcessor::Tick(uint64_t nowMs)
{
	lastTickMs = nowMs;

	std::vector<int> mix(FrameSamples);
	std::vector<SWORD> out(FrameSamples);

	for (int i = 0; i < Ports; ++i)
	{
		Port& port = *ports[i];
		Port& other = *ports[1 - i];

		//Audio : relais de l'autre port + bruit, en addition saturée
		std::fill(mix.begin(), mix.end(), 0);
		other.TakeRelay(mix);
		{
			std::lock_guard<std::mutex> lock(port.noiseMutex);
			for (int k = 0; k < FrameSamples; ++k)
				mix[k] += port.noise.Next();
		}
		for (int k = 0; k < FrameSamples; ++k)
			out[k] = (SWORD)std::max(-32768, std::min(32767, mix[k]));
		//Sans endpoint ni codec, l'encodeur ne fait rien
		port.encoder.EncodeSamples(Samples::FromBuffer(out.data(), FrameSamples, MixRate));

		//Texte
		std::string text;
		{
			std::lock_guard<std::mutex> lock(port.textMutex);
			port.pendingText += port.mux.Pull(nowMs);
			if (!port.textOut.HasListeners())
				//Personne n'écoute : rien ne s'accumule pour plus tard
				port.pendingText.clear();
			else if (!port.pendingText.empty() && port.textOut.CanSend(nowMs))
			{
				const size_t cut = Utf8Cut(port.pendingText, MaxTextPerPacket);
				text = port.pendingText.substr(0, cut);
				port.pendingText.erase(0, cut);
			}
		}
		if (!text.empty())
			port.textOut.Send(text, nowMs);
		else
			port.textOut.SendRedundancy(nowMs);
	}
}

int SpeechProcessor::Run()
{
	Log(">SpeechProcessor: run [%ls]\n", tag.c_str());

	uint64_t next = NowMs();
	while (IsThreadRunning())
	{
		Tick(next);
		next += FrameMs;

		const uint64_t now = NowMs();
		//Trop en retard (machine chargée) : on se recale plutôt que de rafaler
		if (now > next + 5 * FrameMs)
			next = now;
		else if (next > now)
			wait.WaitSignal(next - now);
	}

	Log("<SpeechProcessor: run [%ls]\n", tag.c_str());
	return 0;
}
