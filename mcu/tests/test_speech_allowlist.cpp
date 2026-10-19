/**
 * test_speech_allowlist.cpp — the control port allow list (SPEC §6.1).
 */
#include <gtest/gtest.h>

#include <netinet/in.h>
#include <string>
#include "speech/allowlist.h"

TEST(SpeechAllowList, AnAddressWithoutLengthIsASingleHost)
{
	AllowList list;
	std::string error;
	ASSERT_TRUE(list.Add("192.0.2.7", error));

	EXPECT_TRUE(list.Allows(IPAddress::Parse("192.0.2.7")));
	EXPECT_FALSE(list.Allows(IPAddress::Parse("192.0.2.8")));
}

TEST(SpeechAllowList, APrefixAllowsItsWholeRange)
{
	AllowList list;
	std::string error;
	ASSERT_TRUE(list.Add("10.0.0.0/8", error));
	ASSERT_TRUE(list.Add("192.168.1.0/24", error));

	EXPECT_TRUE(list.Allows(IPAddress::Parse("10.255.0.1")));
	EXPECT_TRUE(list.Allows(IPAddress::Parse("192.168.1.200")));
	EXPECT_FALSE(list.Allows(IPAddress::Parse("192.168.2.1")));
	EXPECT_FALSE(list.Allows(IPAddress()));
}

TEST(SpeechAllowList, AnIPv4MappedPeerMatchesTheIPv4Entry)
{
	AllowList list;
	std::string error;
	ASSERT_TRUE(list.Add("127.0.0.0/8", error));

	sockaddr_in6 sa = {};
	sa.sin6_family = AF_INET6;
	sa.sin6_addr.s6_addr[10] = 0xFF;
	sa.sin6_addr.s6_addr[11] = 0xFF;
	sa.sin6_addr.s6_addr[12] = 127;
	sa.sin6_addr.s6_addr[15] = 1;

	EXPECT_TRUE(list.Allows(IPAddress::FromSockaddr((const sockaddr*)&sa)));
}

TEST(SpeechAllowList, AnIPv6PrefixWorks)
{
	AllowList list;
	std::string error;
	ASSERT_TRUE(list.Add("2001:db8::/32", error));

	EXPECT_TRUE(list.HasIPv6());
	EXPECT_TRUE(list.Allows(IPAddress::Parse("2001:db8:1::5")));
	EXPECT_FALSE(list.Allows(IPAddress::Parse("2001:db9::5")));
}

TEST(SpeechAllowList, MalformedEntriesAreRefused)
{
	AllowList list;
	std::string error;

	EXPECT_FALSE(list.Add("10.0.0.0/", error));
	EXPECT_FALSE(list.Add("10.0.0.0/8x", error));
	EXPECT_FALSE(list.Add("10.0.0.0/-1", error));
	EXPECT_FALSE(list.Add("2001:db8::/129", error));
	EXPECT_FALSE(list.Add("localhost", error));
	EXPECT_FALSE(list.Add("", error));
	EXPECT_TRUE(list.Empty());
}
