/**
 * test_rtmp_media.cpp — round-trips des trames média RTMP (rtmpmessage.h).
 *
 * RTMPAudioFrame / RTMPVideoFrame encapsulent le premier octet de configuration
 * FLV (codec/rate/type de trame) plus, pour AAC et AVC, un en-tête étendu. Le
 * harnais rtmptest.cpp se contentait de logguer ces trames après parsing d'une
 * capture ; on vérifie ici le contrat Serialize → Parse de bout en bout, y
 * compris les en-têtes étendus AAC (AACPacketType) et AVC (type + composition TS).
 */
#include <gtest/gtest.h>
#include <cstring>
#include <vector>

#include "rtmpmessage.h"

namespace {

std::vector<BYTE> MakePayload(DWORD n, BYTE seed)
{
	std::vector<BYTE> v(n);
	for (DWORD i = 0; i < n; ++i)
		v[i] = (BYTE)(seed + i);
	return v;
}

} // namespace

TEST(RtmpAudio, SpeexRoundTrip)
{
	auto payload = MakePayload(40, 0x10);

	RTMPAudioFrame in(1000, payload.size());
	in.SetAudioCodec(RTMPAudioFrame::SPEEX);
	in.SetSoundRate(RTMPAudioFrame::RATE11khz);
	in.SetSamples16Bits(true);
	in.SetStereo(false);
	ASSERT_EQ(in.SetAudioFrame(payload.data(), payload.size()), payload.size());

	BYTE buffer[256];
	DWORD len = in.Serialize(buffer, sizeof(buffer));
	ASSERT_GT(len, 0u);
	ASSERT_EQ(len, in.GetSize());
	EXPECT_EQ(len, payload.size() + 1); // 1 octet d'en-tête, pas d'AAC

	RTMPAudioFrame out(0, payload.size());
	ASSERT_EQ(out.Parse(buffer, len), len);
	EXPECT_EQ(out.GetAudioCodec(), RTMPAudioFrame::SPEEX);
	EXPECT_EQ(out.GetSoundRate(), RTMPAudioFrame::RATE11khz);
	ASSERT_EQ(out.GetMediaSize(), payload.size());
	EXPECT_EQ(0, memcmp(out.GetMediaData(), payload.data(), payload.size()));
}

TEST(RtmpAudio, AacRoundTripHasExtraHeader)
{
	auto payload = MakePayload(32, 0x40);

	RTMPAudioFrame in(2000, payload.size());
	in.SetAudioCodec(RTMPAudioFrame::AAC);
	in.SetSoundRate(RTMPAudioFrame::RATE44khz);
	in.SetSamples16Bits(true);
	in.SetStereo(true);
	in.SetAACPacketType(RTMPAudioFrame::AACRaw);
	ASSERT_EQ(in.SetAudioFrame(payload.data(), payload.size()), payload.size());

	BYTE buffer[256];
	DWORD len = in.Serialize(buffer, sizeof(buffer));
	ASSERT_GT(len, 0u);
	EXPECT_EQ(len, payload.size() + 2); // en-tête + octet AACPacketType

	RTMPAudioFrame out(0, payload.size());
	ASSERT_EQ(out.Parse(buffer, len), len);
	EXPECT_EQ(out.GetAudioCodec(), RTMPAudioFrame::AAC);
	EXPECT_EQ(out.GetAACPacketType(), RTMPAudioFrame::AACRaw);
	ASSERT_EQ(out.GetMediaSize(), payload.size());
	EXPECT_EQ(0, memcmp(out.GetMediaData(), payload.data(), payload.size()));
}

TEST(RtmpVideo, AvcRoundTrip)
{
	auto payload = MakePayload(80, 0x01);

	RTMPVideoFrame in(3000, payload.size());
	in.SetVideoCodec(RTMPVideoFrame::AVC);
	in.SetFrameType(RTMPVideoFrame::INTRA);
	in.SetAVCType(RTMPVideoFrame::AVCNALU);
	in.SetAVCTS(0x010203);
	ASSERT_EQ(in.SetVideoFrame(payload.data(), payload.size()), payload.size());

	BYTE buffer[256];
	DWORD len = in.Serialize(buffer, sizeof(buffer));
	ASSERT_GT(len, 0u);
	ASSERT_EQ(len, in.GetSize());
	EXPECT_EQ(len, payload.size() + 1 + 4); // en-tête FLV + 4 octets AVC

	RTMPVideoFrame out(0, payload.size());
	ASSERT_EQ(out.Parse(buffer, len), len);
	EXPECT_EQ(out.GetVideoCodec(), RTMPVideoFrame::AVC);
	EXPECT_EQ(out.GetFrameType(), RTMPVideoFrame::INTRA);
	EXPECT_EQ(out.GetAVCType(), RTMPVideoFrame::AVCNALU);
	EXPECT_EQ(out.GetAVCTS(), 0x010203u);
	ASSERT_EQ(out.GetMediaSize(), payload.size());
	EXPECT_EQ(0, memcmp(out.GetMediaData(), payload.data(), payload.size()));
}

TEST(RtmpVideo, NonAvcRoundTripNoExtraHeader)
{
	auto payload = MakePayload(50, 0x80);

	RTMPVideoFrame in(4000, payload.size());
	in.SetVideoCodec(RTMPVideoFrame::VP6);
	in.SetFrameType(RTMPVideoFrame::INTER);
	ASSERT_EQ(in.SetVideoFrame(payload.data(), payload.size()), payload.size());

	BYTE buffer[256];
	DWORD len = in.Serialize(buffer, sizeof(buffer));
	ASSERT_GT(len, 0u);
	EXPECT_EQ(len, payload.size() + 1); // seulement l'en-tête FLV

	RTMPVideoFrame out(0, payload.size());
	ASSERT_EQ(out.Parse(buffer, len), len);
	EXPECT_EQ(out.GetVideoCodec(), RTMPVideoFrame::VP6);
	EXPECT_EQ(out.GetFrameType(), RTMPVideoFrame::INTER);
	ASSERT_EQ(out.GetMediaSize(), payload.size());
	EXPECT_EQ(0, memcmp(out.GetMediaData(), payload.data(), payload.size()));
}
