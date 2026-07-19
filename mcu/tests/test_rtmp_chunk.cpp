/**
 * test_rtmp_chunk.cpp — round-trip de la couche chunk RTMP (rtmpchunk.h).
 *
 * REMPLACE le harnais historique mcu/src/rtmptest.cpp : celui-ci rejouait une
 * capture RTMP à travers la machine à états de dé-chunking et se contentait de
 * logguer. On reprend ici EXACTEMENT cette machine à états (états CHUNK_* du
 * harnais, la poignée de main C0/C1/C2 en moins puisqu'on part directement d'un
 * flux de chunks), mais on la pilote avec des données produites en interne par
 * RTMPChunkOutputStream::GetNextChunk() et on ASSERTE le résultat au lieu de le
 * logguer. On couvre :
 *   - le découpage d'un gros message en plusieurs chunks (en-tête type 0 puis
 *     continuations type 3) et son réassemblage ;
 *   - deux messages consécutifs sur le même chunk stream (chemins d'en-tête
 *     type 1/2/3 côté émission) ;
 *   - un message de commande AMF (connect) traversant la couche chunk.
 *
 * Le mediaserver ne possédait aucun test automatisé du binaire mcu ; c'est le
 * premier filet de non-régression du (dé)chunking RTMP.
 */
#include <gtest/gtest.h>
#include <cstring>
#include <map>
#include <vector>

#include "rtmp.h"
#include "rtmpchunk.h"
#include "rtmpmessage.h"

namespace {

// ---- Machine à états de dé-chunking, reprise de rtmptest.cpp -----------------
// Parse un flux de chunks (sans poignée de main) et renvoie les RTMPMessage*
// réassemblés. L'appelant prend possession des messages (à delete).
// maxChunkSize doit correspondre à celui utilisé à l'émission (comme le ferait
// un message SetChunkSize dans un vrai échange).
enum State { CHUNK_HEADER_WAIT, CHUNK_TYPE_WAIT, CHUNK_EXT_TIMESTAMP_WAIT, CHUNK_DATA_WAIT };

void ParseChunkStream(BYTE* data, DWORD size, DWORD maxChunkSize,
		      std::vector<RTMPMessage*>& out)
{
	State state = CHUNK_HEADER_WAIT;

	RTMPChunkBasicHeader header;
	RTMPChunkType0 type0;
	RTMPChunkType1 type1;
	RTMPChunkType2 type2;
	RTMPExtendedTimestamp extts;

	std::map<DWORD, RTMPChunkInputStream*> chunkInputStreams;
	RTMPChunkInputStream* chunkInputStream = NULL;
	DWORD chunkStreamId = 0;
	DWORD chunkLen = 0;
	int len = 0;

	BYTE* buffer = data;
	DWORD bufferSize = size;

	//Garde-fou : chaque octet est consommé au plus par un petit nombre d'itérations
	//(transitions d'état sans consommation incluses). Au-delà, on abandonne — un
	//test échouera alors sur le compte de messages plutôt que de figer la suite.
	long guard = (long)size * 8 + 256;

	while (bufferSize > 0 && guard-- > 0)
	{
		switch (state)
		{
			case CHUNK_HEADER_WAIT:
				len = header.Parse(buffer, bufferSize);
				buffer += len;
				bufferSize -= len;
				if (header.IsParsed())
				{
					type0.Reset();
					type1.Reset();
					type2.Reset();
					extts.Reset();
					state = CHUNK_TYPE_WAIT;
				}
				break;

			case CHUNK_TYPE_WAIT:
			{
				chunkStreamId = header.GetStreamId();
				auto it = chunkInputStreams.find(chunkStreamId);
				if (it == chunkInputStreams.end())
				{
					chunkInputStream = new RTMPChunkInputStream();
					chunkInputStreams[chunkStreamId] = chunkInputStream;
				} else
					chunkInputStream = it->second;

				switch (header.GetFmt())
				{
					case 0:
						len = type0.Parse(buffer, bufferSize);
						if (type0.IsParsed())
						{
							chunkInputStream->SetMessageLength(type0.GetMessageLength());
							chunkInputStream->SetMessageTypeId(type0.GetMessageTypeId());
							chunkInputStream->SetMessageStreamId(type0.GetMessageStreamId());
							if (type0.GetTimestamp() != 0xFFFFFF)
							{
								chunkInputStream->SetTimestamp(type0.GetTimestamp());
								chunkInputStream->SetTimestampDelta(0);
								state = CHUNK_DATA_WAIT;
							} else
								state = CHUNK_EXT_TIMESTAMP_WAIT;
							chunkInputStream->StartChunkData();
							chunkLen = 0;
						}
						break;
					case 1:
						len = type1.Parse(buffer, bufferSize);
						if (type1.IsParsed())
						{
							chunkInputStream->SetMessageLength(type1.GetMessageLength());
							chunkInputStream->SetMessageTypeId(type1.GetMessageTypeId());
							if (type1.GetTimestampDelta() != 0xFFFFFF)
							{
								chunkInputStream->SetTimestampDelta(type1.GetTimestampDelta());
								chunkInputStream->IncreaseTimestampWithDelta();
								state = CHUNK_DATA_WAIT;
							} else
								state = CHUNK_EXT_TIMESTAMP_WAIT;
							chunkInputStream->StartChunkData();
							chunkLen = 0;
						}
						break;
					case 2:
						len = type2.Parse(buffer, bufferSize);
						if (type2.IsParsed())
						{
							if (type2.GetTimestampDelta() != 0xFFFFFF)
							{
								chunkInputStream->SetTimestampDelta(type2.GetTimestampDelta());
								chunkInputStream->IncreaseTimestampWithDelta();
								state = CHUNK_DATA_WAIT;
							} else
								state = CHUNK_EXT_TIMESTAMP_WAIT;
							chunkInputStream->StartChunkData();
							chunkLen = 0;
						}
						break;
					case 3:
						len = 0;
						chunkInputStream->IncreaseTimestampWithDelta();
						chunkInputStream->StartChunkData();
						state = CHUNK_DATA_WAIT;
						chunkLen = 0;
						break;
				}
				buffer += len;
				bufferSize -= len;
				break;
			}

			case CHUNK_EXT_TIMESTAMP_WAIT:
				len = extts.Parse(buffer, bufferSize);
				buffer += len;
				bufferSize -= len;
				if (extts.IsParsed())
				{
					if (header.GetFmt() == 1)
					{
						chunkInputStream->SetTimestamp(extts.GetTimestamp());
						chunkInputStream->SetTimestampDelta(0);
					} else {
						chunkInputStream->SetTimestampDelta(extts.GetTimestamp());
						chunkInputStream->IncreaseTimestampWithDelta();
					}
					state = CHUNK_DATA_WAIT;
				}
				break;

			case CHUNK_DATA_WAIT:
			{
				//On ne consomme au plus que ce qu'il reste dans le chunk courant.
				DWORD remaining = (maxChunkSize && maxChunkSize > chunkLen)
						? maxChunkSize - chunkLen : bufferSize;
				len = bufferSize < remaining ? bufferSize : remaining;
				if (!len)
					break;
				len = chunkInputStream->Parse(buffer, len);
				chunkLen += len;
				buffer += len;
				bufferSize -= len;
				if (chunkInputStream->IsParsed())
				{
					//Message complet : on le récupère et on repart sur un en-tête.
					out.push_back(chunkInputStream->GetMessage());
					state = CHUNK_HEADER_WAIT;
					header.Reset();
				}
				else if (maxChunkSize && chunkLen == maxChunkSize)
				{
					//Fin du chunk courant mais message incomplet : le chunk suivant
					//commence par un nouvel en-tête de base (fmt 3, continuation).
					//C'est la transition qui manquait à rtmptest.cpp (boucle infinie
					//sur les messages plus grands que maxChunkSize).
					state = CHUNK_HEADER_WAIT;
					header.Reset();
				}
				break;
			}
		}
	}

	//Clean chunk streams (the messages are handed to the caller)
	for (auto& kv : chunkInputStreams)
		delete kv.second;
}

// Sérialise tous les messages en attente d'un output stream vers un buffer plat.
DWORD SerializeChunks(RTMPChunkOutputStream& os, BYTE* wire, DWORD wireCap, DWORD maxChunkSize)
{
	DWORD wireLen = 0;
	while (os.HasData())
	{
		DWORD n = os.GetNextChunk(wire + wireLen, wireCap - wireLen, maxChunkSize);
		if (!n)
			break;
		wireLen += n;
	}
	return wireLen;
}

std::vector<BYTE> MakePayload(DWORD n, BYTE seed)
{
	std::vector<BYTE> v(n);
	for (DWORD i = 0; i < n; ++i)
		v[i] = (BYTE)(seed + i);
	return v;
}

RTMPMessage* MakeAudioMessage(DWORD streamId, QWORD ts, const std::vector<BYTE>& payload)
{
	RTMPAudioFrame* audio = new RTMPAudioFrame(ts, payload.size());
	audio->SetAudioCodec(RTMPAudioFrame::SPEEX);
	audio->SetSoundRate(RTMPAudioFrame::RATE11khz);
	audio->SetSamples16Bits(true);
	audio->SetStereo(false);
	audio->SetAudioFrame(payload.data(), payload.size());
	return new RTMPMessage(streamId, ts, (RTMPMediaFrame*)audio);
}

} // namespace

// Un gros message audio, forcé sur plusieurs chunks (type 0 + continuations type 3).
TEST(RtmpChunk, LargeMessageSplitAndReassembled)
{
	const DWORD chunkStreamId = 4;
	const DWORD streamId = 1;
	const DWORD maxChunkSize = 128;
	auto payload = MakePayload(300, 0x20); // > 2 * maxChunkSize → 3 chunks

	RTMPChunkOutputStream os(chunkStreamId);
	os.SendMessage(MakeAudioMessage(streamId, 1000, payload));

	BYTE wire[8192];
	DWORD wireLen = SerializeChunks(os, wire, sizeof(wire), maxChunkSize);
	ASSERT_GT(wireLen, payload.size()); // en-têtes de chunk présents

	std::vector<RTMPMessage*> msgs;
	ParseChunkStream(wire, wireLen, maxChunkSize, msgs);

	ASSERT_EQ(msgs.size(), 1u);
	RTMPMessage* got = msgs[0];
	EXPECT_EQ(got->GetType(), RTMPMessage::Audio);
	EXPECT_EQ(got->GetStreamId(), streamId);
	ASSERT_TRUE(got->IsMedia());
	RTMPMediaFrame* frame = got->GetMediaFrame();
	ASSERT_NE(frame, nullptr);
	ASSERT_EQ(frame->GetMediaSize(), payload.size());
	EXPECT_EQ(0, memcmp(frame->GetMediaData(), payload.data(), payload.size()));

	for (auto* m : msgs) delete m;
}

// Deux messages consécutifs sur le même chunk stream (émission via en-têtes
// compacts type 1/2/3), tous deux réassemblés correctement.
TEST(RtmpChunk, ConsecutiveMessagesSameStream)
{
	const DWORD chunkStreamId = 5;
	const DWORD streamId = 1;
	const DWORD maxChunkSize = 4096; // pas de découpage : messages courts

	auto p1 = MakePayload(60, 0x10);
	auto p2 = MakePayload(60, 0x90);

	RTMPChunkOutputStream os(chunkStreamId);
	os.SendMessage(MakeAudioMessage(streamId, 1000, p1));
	os.SendMessage(MakeAudioMessage(streamId, 1040, p2));

	BYTE wire[8192];
	DWORD wireLen = SerializeChunks(os, wire, sizeof(wire), maxChunkSize);

	std::vector<RTMPMessage*> msgs;
	ParseChunkStream(wire, wireLen, maxChunkSize, msgs);

	ASSERT_EQ(msgs.size(), 2u);
	ASSERT_EQ(msgs[0]->GetMediaFrame()->GetMediaSize(), p1.size());
	EXPECT_EQ(0, memcmp(msgs[0]->GetMediaFrame()->GetMediaData(), p1.data(), p1.size()));
	ASSERT_EQ(msgs[1]->GetMediaFrame()->GetMediaSize(), p2.size());
	EXPECT_EQ(0, memcmp(msgs[1]->GetMediaFrame()->GetMediaData(), p2.data(), p2.size()));

	for (auto* m : msgs) delete m;
}

// Un message de commande AMF (connect) traversant la couche chunk.
TEST(RtmpChunk, CommandMessageRoundTrip)
{
	const DWORD chunkStreamId = 3;
	const DWORD streamId = 0; // les commandes NetConnection vont sur le stream 0
	const DWORD maxChunkSize = 128;

	AMFObject* params = new AMFObject();
	params->AddProperty(L"app", L"live");
	params->AddProperty(L"tcUrl", L"rtmp://localhost/live");
	RTMPCommandMessage* cmd = new RTMPCommandMessage(L"connect", 1.0, params, NULL);

	RTMPChunkOutputStream os(chunkStreamId);
	os.SendMessage(new RTMPMessage(streamId, 0, cmd));

	BYTE wire[8192];
	DWORD wireLen = SerializeChunks(os, wire, sizeof(wire), maxChunkSize);

	std::vector<RTMPMessage*> msgs;
	ParseChunkStream(wire, wireLen, maxChunkSize, msgs);

	ASSERT_EQ(msgs.size(), 1u);
	RTMPMessage* got = msgs[0];
	EXPECT_EQ(got->GetType(), RTMPMessage::Command);
	ASSERT_TRUE(got->IsCommandMessage());
	RTMPCommandMessage* parsed = got->GetCommandMessage();
	ASSERT_NE(parsed, nullptr);
	EXPECT_EQ(parsed->GetName(), std::wstring(L"connect"));
	EXPECT_DOUBLE_EQ(parsed->GetTransId(), 1.0);
	AMFData* gotParams = parsed->GetParams();
	ASSERT_NE(gotParams, nullptr);
	ASSERT_EQ(gotParams->GetType(), AMFData::Object);
	EXPECT_EQ((std::wstring)((AMFObject*)gotParams)->GetProperty(L"app"), std::wstring(L"live"));

	for (auto* m : msgs) delete m;
}
