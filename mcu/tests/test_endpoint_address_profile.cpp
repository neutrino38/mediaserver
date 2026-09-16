/**
 * test_endpoint_address_profile.cpp — the addressing profile of a JSR309 port
 * that has no RTP session.
 *
 * The contract (docs/JSR-309-API.md §6.7 bis) has the controller put the SAME
 * profile on every port of a leg, and put it on the `Start*` call that publishes
 * the port. Text over WebSocket is a port with no RTP session: it listens on the
 * server-wide WebSocket listener, so a profile has nothing to bind and nothing to
 * announce there.
 *
 * Refusing it took the whole media down with it. EndpointStartReceiving applies
 * the profile before opening the receive plane, so `"pas de session RTP pour ce
 * media"` failed that RPC, and the controller dropped the `m=text` section of
 * every call carrying real-time text over WebSocket.
 */
#include <gtest/gtest.h>

#include "../src/jsr309/Endpoint.h"

namespace {

// Text only: Init() then binds one RTP text port, which ConfigureMediaConnection
// replaces with a WSEndpoint — the port shape the bug was about.
TEST(EndpointAddressProfile, AWebSocketPortAcceptsTheProfileAndAppliesNothing)
{
	Endpoint endpoint(L"cx-profile-ws",
	                  /*audioSupported=*/false,
	                  /*videoSupported=*/false,
	                  /*textSupported=*/true);

	ASSERT_EQ(0, endpoint.Init());
	ASSERT_EQ(1, endpoint.ConfigureMediaConnection(MediaFrame::Text, MediaFrame::VIDEO_MAIN,
	                                               MediaFrame::WS, "t140"));

	std::string error;

	EXPECT_TRUE(endpoint.SetAddressProfile(MediaFrame::Text, "publicv4", error));
	EXPECT_TRUE(error.empty()) << error;

	endpoint.End();
}

// The other half of the rule: accepting a profile on a port that cannot apply it
// must not turn into accepting anything. A media this endpoint does not carry has
// no port at all, and that stays a refusal — with a message, since the controller
// reads it back as an xmlerror.
TEST(EndpointAddressProfile, AMediaTheEndpointDoesNotCarryIsStillRefused)
{
	Endpoint endpoint(L"cx-profile-no-video",
	                  /*audioSupported=*/false,
	                  /*videoSupported=*/false,
	                  /*textSupported=*/true);

	ASSERT_EQ(0, endpoint.Init());

	std::string error;

	EXPECT_FALSE(endpoint.SetAddressProfile(MediaFrame::Video, "publicv4", error));
	EXPECT_FALSE(error.empty());

	endpoint.End();
}

// An empty profile is "the controller does not know about profiles": the default
// applies and nothing is asked of the port, whatever its shape.
TEST(EndpointAddressProfile, AnEmptyProfileAsksForNothing)
{
	Endpoint endpoint(L"cx-profile-empty",
	                  /*audioSupported=*/false,
	                  /*videoSupported=*/false,
	                  /*textSupported=*/true);

	ASSERT_EQ(0, endpoint.Init());

	std::string error;

	EXPECT_TRUE(endpoint.SetAddressProfile(MediaFrame::Text, "", error));
	EXPECT_TRUE(endpoint.SetAddressProfile(MediaFrame::Video, NULL, error));

	endpoint.End();
}

}  // namespace
