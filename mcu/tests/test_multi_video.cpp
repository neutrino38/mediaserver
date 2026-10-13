/**
 * test_multi_video.cpp — plusieurs flux vidéo par participant de conférence,
 * chacun source du mixeur sous son propre sourceId.
 *
 * Conception : docs/conception/UNIFIED-PLAN/SPEC.md §4.2, lot 1. Le mixeur ne
 * connaît que des identifiants de source : un flux de rôle >= 2 en reçoit un,
 * et prend un slot de mosaïque comme un participant de plus. Ces tests figent :
 *   - deux flux → deux sources distinctes du partId, deux slots occupés ;
 *   - un flux supprimé libère son slot, et son rôle n'est jamais réattribué ;
 *   - le participant supprimé emporte toutes ses sources ;
 *   - les rôles 0 et 1 ne se suppriment pas, un rôle inconnu est une erreur ;
 *   - la jambe d'un flux >= 2 s'ouvre sur son propre port.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <list>
#include <memory>

#include "mcu.h"
#include "multiconf.h"
#include "xmlstreaminghandler.h"

namespace {

bool InMosaic(MultiConf& conf, int mosaicId, int sourceId)
{
	std::list<int> positions;
	conf.GetMosaicPositions(mosaicId, positions);
	return std::find(positions.begin(), positions.end(), sourceId) != positions.end();
}

class MultiVideo : public ::testing::Test
{
protected:
	void SetUp() override
	{
		mcu.Init(&handler, 0);
		confId = mcu.CreateConference(L"multi-video", 0);
		ASSERT_GT(confId, 0);
		ASSERT_TRUE(mcu.GetConferenceRef(confId, conf));
		ASSERT_TRUE(conf->Init(0, 8000));
		partId = conf->CreateParticipant(0, 0, L"alice", Participant::RTP);
		ASSERT_GT(partId, 0);
	}

	void TearDown() override
	{
		conf.reset();
		mcu.End();
	}

	XmlStreamingHandler handler;
	MCU mcu;
	int confId = 0;
	std::shared_ptr<MultiConf> conf;
	int partId = 0;
};

TEST_F(MultiVideo, DeuxFluxDeuxSourcesDeuxSlots)
{
	int role1 = 0, source1 = 0, role2 = 0, source2 = 0;
	ASSERT_TRUE(conf->CreateVideoStream(partId, role1, source1));
	ASSERT_TRUE(conf->CreateVideoStream(partId, role2, source2));

	EXPECT_EQ(role1, MediaFrame::VIDEO_EXTRA);
	EXPECT_EQ(role2, MediaFrame::VIDEO_EXTRA + 1);
	EXPECT_NE(source1, source2);
	EXPECT_NE(source1, partId);
	EXPECT_NE(source2, partId);

	EXPECT_EQ(conf->GetVideoStreamSource(partId, MediaFrame::VIDEO_MAIN), partId);
	EXPECT_EQ(conf->GetVideoStreamSource(partId, role1), source1);
	EXPECT_EQ(conf->GetVideoStreamSource(partId, role2), source2);

	//SetMixerMosaic échoue sur une source que le mixeur ne connaît pas.
	EXPECT_TRUE(conf->SetVideoStreamMosaic(partId, role1, -1));
	EXPECT_TRUE(conf->SetVideoStreamMosaic(partId, role2, -1));

	ASSERT_TRUE(conf->AddMosaicParticipant(0, source1));
	ASSERT_TRUE(conf->AddMosaicParticipant(0, source2));
	EXPECT_TRUE(InMosaic(*conf, 0, source1));
	EXPECT_TRUE(InMosaic(*conf, 0, source2));
}

TEST_F(MultiVideo, SupprimerUnFluxLibereSonSlot)
{
	int role1 = 0, source1 = 0, role2 = 0, source2 = 0;
	ASSERT_TRUE(conf->CreateVideoStream(partId, role1, source1));
	ASSERT_TRUE(conf->CreateVideoStream(partId, role2, source2));
	ASSERT_TRUE(conf->AddMosaicParticipant(0, source1));
	ASSERT_TRUE(conf->AddMosaicParticipant(0, source2));

	ASSERT_TRUE(conf->DeleteVideoStream(partId, role1));

	EXPECT_FALSE(InMosaic(*conf, 0, source1));
	EXPECT_TRUE(InMosaic(*conf, 0, source2));
	EXPECT_EQ(conf->GetVideoStreamSource(partId, role1), 0);
	EXPECT_FALSE(conf->SetVideoStreamMosaic(partId, role1, -1));
	EXPECT_FALSE(conf->DeleteVideoStream(partId, role1));

	int role3 = 0, source3 = 0;
	ASSERT_TRUE(conf->CreateVideoStream(partId, role3, source3));
	EXPECT_EQ(role3, role2 + 1);
	EXPECT_NE(source3, source1);
}

TEST_F(MultiVideo, SupprimerLeParticipantRetireToutesSesSources)
{
	int role1 = 0, source1 = 0, role2 = 0, source2 = 0;
	ASSERT_TRUE(conf->CreateVideoStream(partId, role1, source1));
	ASSERT_TRUE(conf->CreateVideoStream(partId, role2, source2));
	ASSERT_TRUE(conf->AddMosaicParticipant(0, source1));
	ASSERT_TRUE(conf->AddMosaicParticipant(0, source2));

	ASSERT_TRUE(conf->DeleteParticipant(partId));

	EXPECT_FALSE(InMosaic(*conf, 0, source1));
	EXPECT_FALSE(InMosaic(*conf, 0, source2));
	EXPECT_EQ(conf->GetVideoStreamSource(partId, role1), 0);
	EXPECT_EQ(conf->GetVideoStreamSource(partId, role2), 0);
	EXPECT_EQ(conf->GetVideoStreamSource(partId, MediaFrame::VIDEO_MAIN), 0);
}

TEST_F(MultiVideo, LesRolesZeroEtUnNeSeSupprimentPas)
{
	EXPECT_FALSE(conf->DeleteVideoStream(partId, MediaFrame::VIDEO_MAIN));
	EXPECT_FALSE(conf->DeleteVideoStream(partId, MediaFrame::VIDEO_SLIDES));

	int role = 0, source = 0;
	EXPECT_FALSE(conf->CreateVideoStream(partId + 1000, role, source));
}

TEST_F(MultiVideo, UnRoleInconnuEstUneErreur)
{
	const MediaFrame::MediaRole unknown = (MediaFrame::MediaRole)7;
	RTPMap map;
	map[96] = VideoCodec::VP8;

	EXPECT_EQ(conf->StartReceiving(partId, MediaFrame::Video, map, unknown), 0);
	EXPECT_EQ(conf->StopSending(partId, MediaFrame::Video, unknown), 0);
	EXPECT_EQ(conf->GetVideoStreamSource(partId, unknown), 0);
	EXPECT_FALSE(conf->SetVideoStreamMosaic(partId, unknown, 0));
}

TEST_F(MultiVideo, LaJambeDUnFluxSupplementaireAOnProprePort)
{
	int role = 0, source = 0;
	ASSERT_TRUE(conf->CreateVideoStream(partId, role, source));

	RTPMap mainMap;
	mainMap[96] = VideoCodec::VP8;
	RTPMap extraMap = mainMap;

	const int mainPort = conf->StartReceiving(partId, MediaFrame::Video, mainMap, MediaFrame::VIDEO_MAIN);
	const int extraPort = conf->StartReceiving(partId, MediaFrame::Video, extraMap, (MediaFrame::MediaRole)role);

	EXPECT_GT(mainPort, 0);
	EXPECT_GT(extraPort, 0);
	EXPECT_NE(mainPort, extraPort);
	EXPECT_TRUE(conf->StopReceiving(partId, MediaFrame::Video, (MediaFrame::MediaRole)role));
}

}
