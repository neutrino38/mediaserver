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
#include "vad.h"
#include "speech/speechservice.h"
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

	//Une trame d'entrée, lue une seule fois par tour : elle sert au relais
	//vers l'autre port ET à la reconnaissance de ce port. Rend le nombre
	//d'échantillons lus, 0 tant que l'avance n'est pas prise.
	int TakeInput(SWORD* frame)
	{
		std::lock_guard<std::mutex> lock(audioMutex);
		if (!primed)
		{
			if (relay.size() < (size_t)(RelayStartMs * MixRate / 1000))
				return 0;
			primed = true;
		}
		int taken = 0;
		while (taken < FrameSamples && !relay.empty())
		{
			frame[taken++] = relay.front();
			relay.pop_front();
		}
		//Anneau vide : on reprend l'avance avant de rejouer
		if (relay.empty())
			primed = false;
		return taken;
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

	//Reconnaissance de la voix entrée sur ce port : posée par le plan de
	//contrôle, lue par le tour
	std::mutex			asrMutex;
	std::shared_ptr<Recognizer>	recognizer;
};

/**
 * La reconnaissance d'un port source (§6). Le tour possède tout son état,
 * sauf ce que le flux asr dépose depuis le réacteur, sous `mutex`.
 */
class SpeechProcessor::Recognizer : public SpeechFlow::Listener
{
public:
	Recognizer(int port, bool toEvents, int textPort, bool halfDuplex) :
		port(port), toEvents(toEvents), textPort(textPort), halfDuplex(halfDuplex)
	{
		vad.SetMode(VAD::AGGRESSIVE);
	}

	void Stop()
	{
		if (flow)
			flow->Stop();
	}

	//SpeechFlow::Listener, sur le réacteur ou l'horloge du service
	virtual void onText(SpeechFlow* flow, const std::string& text) override
	{
		std::lock_guard<std::mutex> lock(mutex);
		incoming += text;
	}
	virtual void onState(SpeechFlow* flow, bool up, const std::string& reason) override
	{
		std::lock_guard<std::mutex> lock(mutex);
		states.emplace_back(up, reason);
	}
	//Remplacer le flux au milieu d'une phrase la couperait en deux
	virtual bool CanRotate(SpeechFlow* flow) override
	{
		return !speaking;
	}

	const int			port;
	const bool			toEvents;
	const int			textPort;
	//Effet au lot 4 : il porte sur la synthèse vers ce port
	const bool			halfDuplex;
	std::shared_ptr<SpeechFlow>	flow;

	std::mutex					mutex;
	std::string					incoming;
	std::vector<std::pair<bool, std::string>>	states;

	//Le tour seulement, sauf `speaking` que lit aussi CanRotate
	VAD			vad;
	std::deque<SWORD>	preRoll;
	std::atomic<bool>	speaking{false};
	uint64_t		lastVoiceMs = 0;
	bool			awaitingFinal = false;
	uint64_t		pausedAtMs = 0;
	uint64_t		lastDeltaMs = 0;
	uint64_t		lastPartialMs = 0;
	std::string		utterance;
	std::string		partial;
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
		StopRecognition(i);

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

//La reconnaissance parle au flux asr dans la fréquence du mix
static_assert(SpeechProcessor::MixRate == 16000, "speech-server takes 16 kHz: resample before the asr flow");

int SpeechProcessor::StartRecognition(int port, bool toEvents, int textPort, bool halfDuplex)
{
	if (!speech)
		return Error("-SpeechProcessor: speech service not configured\n");
	if (!IsValidPort(port) || (textPort != -1 && !IsValidPort(textPort)))
		return Error("-SpeechProcessor: invalid port [port:%d,textPort:%d]\n", port, textPort);

	auto rec = std::make_shared<Recognizer>(port, toEvents, textPort, halfDuplex);
	rec->flow = speech->CreateFlow(SpeechFunction::Asr, rec);
	if (!rec->flow)
		return Error("-SpeechProcessor: speech service not configured\n");
	//En pause jusqu'à la première parole : le silence ne se paie pas
	rec->flow->Pause();

	std::shared_ptr<Recognizer> previous;
	{
		std::lock_guard<std::mutex> lock(ports[port]->asrMutex);
		previous = ports[port]->recognizer;
		ports[port]->recognizer = rec;
	}
	if (previous)
		previous->Stop();

	rec->flow->Start();
	Log("-SpeechProcessor: recognition started [%ls,port:%d,events:%d,textPort:%d]\n",
	    tag.c_str(), port, toEvents, textPort);
	return 1;
}

int SpeechProcessor::StopRecognition(int port)
{
	if (!IsValidPort(port))
		return Error("-SpeechProcessor: invalid port %d\n", port);

	std::shared_ptr<Recognizer> previous;
	{
		std::lock_guard<std::mutex> lock(ports[port]->asrMutex);
		previous.swap(ports[port]->recognizer);
	}
	//Le texte pas encore clos en énoncé est abandonné avec le flux
	if (previous)
		previous->Stop();
	return 1;
}

void SpeechProcessor::Post(JSR309Event* event)
{
	if (poster)
		poster(event);
	else
		delete event;
}

void SpeechProcessor::CloseUtterance(Recognizer& rec)
{
	rec.awaitingFinal = false;
	rec.partial.clear();
	if (rec.utterance.empty())
		return;
	if (rec.toEvents)
		Post(new SpeechRecognizedEvent(rec.port, rec.utterance, true));
	if (rec.textPort != -1)
	{
		//Fin de ligne : la transcription rend la main au texte relayé
		std::string end;
		TextMux::AppendUtf8(end, TextMux::LineSeparator);
		Port& to = *ports[rec.textPort];
		std::lock_guard<std::mutex> lock(to.textMutex);
		to.mux.Push(1, end, lastTickMs);
	}
	rec.utterance.clear();
}

void SpeechProcessor::Recognize(Recognizer& rec, const SWORD* frame, int samples, uint64_t nowMs)
{
	if (samples > 0)
	{
		SWORD full[FrameSamples] = {};
		std::copy(frame, frame + samples, full);
		const bool voiced = rec.vad.CalcVad(full, FrameSamples, MixRate) > 0;

		if (voiced)
		{
			rec.lastVoiceMs = nowMs;
			if (!rec.speaking)
			{
				//Une parole qui reprend avant la fin d'énoncé la clôt
				if (rec.awaitingFinal)
					CloseUtterance(rec);
				rec.speaking = true;
				Post(new SpeechActivityEvent(rec.port, true));
				rec.flow->Resume();
				//Le pré-roulement : la première syllabe précède la détection
				std::vector<SWORD> before(rec.preRoll.begin(), rec.preRoll.end());
				rec.preRoll.clear();
				if (!before.empty())
					rec.flow->SendAudio((const BYTE*)before.data(), before.size() * sizeof(SWORD));
			}
		}

		if (rec.speaking)
			rec.flow->SendAudio((const BYTE*)full, sizeof(full));
		else
		{
			rec.preRoll.insert(rec.preRoll.end(), full, full + FrameSamples);
			while (rec.preRoll.size() > (size_t)(PreRollMs * MixRate / 1000))
				rec.preRoll.pop_front();
		}
	}

	//Silence, ou plus rien en entrée : la pause fait arriver la fin du texte
	if (rec.speaking && nowMs - rec.lastVoiceMs >= (uint64_t)SilenceMs)
	{
		rec.speaking = false;
		rec.flow->Pause();
		rec.awaitingFinal = true;
		rec.pausedAtMs = nowMs;
		Post(new SpeechActivityEvent(rec.port, false));
	}

	std::string delta;
	std::vector<std::pair<bool, std::string>> states;
	{
		std::lock_guard<std::mutex> lock(rec.mutex);
		delta.swap(rec.incoming);
		states.swap(rec.states);
	}

	for (const auto& state : states)
		Post(new SpeechServiceEvent(rec.port, "asr", state.first, state.second));

	if (!delta.empty())
	{
		rec.utterance += delta;
		rec.partial += delta;
		rec.lastDeltaMs = nowMs;
		if (rec.textPort != -1)
		{
			Port& to = *ports[rec.textPort];
			std::lock_guard<std::mutex> lock(to.textMutex);
			to.mux.Push(1, delta, nowMs);
		}
	}

	if (rec.toEvents && !rec.partial.empty() && nowMs - rec.lastPartialMs >= (uint64_t)PartialEventMs)
	{
		Post(new SpeechRecognizedEvent(rec.port, rec.partial, false));
		rec.partial.clear();
		rec.lastPartialMs = nowMs;
	}

	//Le protocole ne marque pas la fin du texte d'une pause : elle se déduit
	//d'un délai sans delta (§10.5)
	if (rec.awaitingFinal && nowMs - std::max(rec.pausedAtMs, rec.lastDeltaMs) >= (uint64_t)UtteranceEndMs)
		CloseUtterance(rec);
}

void SpeechProcessor::Tick(uint64_t nowMs)
{
	lastTickMs = nowMs;

	std::vector<int> mix(FrameSamples);
	std::vector<SWORD> out(FrameSamples);
	SWORD in[Ports][FrameSamples];
	int taken[Ports];

	for (int i = 0; i < Ports; ++i)
	{
		taken[i] = ports[i]->TakeInput(in[i]);

		std::shared_ptr<Recognizer> rec;
		{
			std::lock_guard<std::mutex> lock(ports[i]->asrMutex);
			rec = ports[i]->recognizer;
		}
		if (rec)
			Recognize(*rec, in[i], taken[i], nowMs);
	}

	for (int i = 0; i < Ports; ++i)
	{
		Port& port = *ports[i];
		const int o = 1 - i;

		//Audio : relais de l'autre port + bruit, en addition saturée
		std::fill(mix.begin(), mix.end(), 0);
		for (int k = 0; k < taken[o]; ++k)
			mix[k] += in[o][k];
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

xmlrpc_value* SpeechEvent::GetXmlValue(xmlrpc_env *env)
{
	BYTE sessTag[1024];
	UTF8Parser sessTagParser(sessionTag);
	DWORD sessLen = sessTagParser.Serialize(sessTag,1024);
	sessTag[sessLen] = 0;

	xmlrpc_value* value = xmlrpc_build_value(env,"(isiii)",type,sessTag,joinableId,(int)eventMedia,port);
	xmlrpc_value* tail = GetTail(env);
	for (int i = 0; i < xmlrpc_array_size(env,tail); ++i)
	{
		xmlrpc_value* item = NULL;
		xmlrpc_array_read_item(env,tail,i,&item);
		xmlrpc_array_append_item(env,value,item);
		xmlrpc_DECREF(item);
	}
	xmlrpc_DECREF(tail);
	return value;
}

xmlrpc_value* SpeechRecognizedEvent::GetTail(xmlrpc_env *env)
{
	return xmlrpc_build_value(env,"(sb)",text.c_str(),(xmlrpc_bool)final);
}

xmlrpc_value* SpeechActivityEvent::GetTail(xmlrpc_env *env)
{
	return xmlrpc_build_value(env,"(b)",(xmlrpc_bool)speaking);
}

xmlrpc_value* SpeechServiceEvent::GetTail(xmlrpc_env *env)
{
	return xmlrpc_build_value(env,"(sss)",function.c_str(),up ? "up" : "down",reason.c_str());
}
