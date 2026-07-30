/**
 * test_websocket_frame.cpp — round-trip de l'en-tête de trame WebSocket (RFC 6455).
 *
 * WebSocketFrameHeader encode/décode le premier bloc d'une trame WS : bit FIN,
 * opcode, longueur (7 bits, ou étendue 16/64 bits) et masque optionnel. C'est la
 * brique bas niveau du refactor WebSocket mono-thread (websocket-refactor.md) ;
 * le harnais wstest.cpp l'exerçait de bout en bout via un client Python. On la
 * teste ici de façon autonome : construction → GetData/GetSize → Parser::Parse.
 */
#include <gtest/gtest.h>

#include "websocketconnection.h"

namespace {

// Construit un en-tête, le sérialise puis le reparse via le Parser embarqué.
// Renvoie l'en-tête reconstruit (propriété de l'appelant : à delete).
WebSocketFrameHeader* RoundTrip(WebSocketFrameHeader& in)
{
	WebSocketFrameHeader::Parser parser;
	int consumed = parser.Parse(in.GetData(), in.GetSize());
	EXPECT_EQ((DWORD)consumed, in.GetSize());
	EXPECT_TRUE(parser.IsParsed());
	return parser.ConsumeHeader();
}

} // namespace

TEST(WebSocketFrame, ShortTextUnmasked)
{
	WebSocketFrameHeader in(true, WebSocketFrameHeader::TextFrame, 5, 0);
	EXPECT_EQ(in.GetSize(), 2u); // 1 + 1, pas de masque

	WebSocketFrameHeader* out = RoundTrip(in);
	ASSERT_NE(out, nullptr);
	EXPECT_TRUE(out->IsFin());
	EXPECT_EQ(out->GetOpCode(), WebSocketFrameHeader::TextFrame);
	EXPECT_FALSE(out->IsMasked());
	EXPECT_EQ(out->GetPayloadLength(), 5u);
	delete out;
}

TEST(WebSocketFrame, ShortBinaryMasked)
{
	const DWORD mask = 0x12345678;
	WebSocketFrameHeader in(true, WebSocketFrameHeader::BinaryFrame, 100, mask);
	EXPECT_EQ(in.GetSize(), 6u); // 1 + 1 + 4 octets de masque

	WebSocketFrameHeader* out = RoundTrip(in);
	ASSERT_NE(out, nullptr);
	EXPECT_EQ(out->GetOpCode(), WebSocketFrameHeader::BinaryFrame);
	EXPECT_TRUE(out->IsMasked());
	EXPECT_EQ(out->GetMask(), mask);
	EXPECT_EQ(out->GetPayloadLength(), 100u);
	delete out;
}

TEST(WebSocketFrame, ExtendedLength16Bits)
{
	// 126 <= len < 65536 → longueur sur 2 octets (indicateur 126).
	WebSocketFrameHeader in(true, WebSocketFrameHeader::BinaryFrame, 4096, 0);
	EXPECT_EQ(in.GetSize(), 4u); // octet0 + (octet len=126 + 2 octets étendus)

	WebSocketFrameHeader* out = RoundTrip(in);
	ASSERT_NE(out, nullptr);
	EXPECT_EQ(out->GetPayloadLength(), 4096u);
	EXPECT_FALSE(out->IsMasked());
	delete out;
}

TEST(WebSocketFrame, ExtendedLength64BitsMasked)
{
	// len >= 65536 → longueur sur 8 octets (indicateur 127).
	const QWORD len = 200000;
	const DWORD mask = 0xDEADBEEF;
	WebSocketFrameHeader in(true, WebSocketFrameHeader::BinaryFrame, len, mask);
	EXPECT_EQ(in.GetSize(), 14u); // octet0 + (octet len=127 + 8 octets étendus) + 4 (masque)

	WebSocketFrameHeader* out = RoundTrip(in);
	ASSERT_NE(out, nullptr);
	EXPECT_EQ(out->GetPayloadLength(), len);
	EXPECT_TRUE(out->IsMasked());
	EXPECT_EQ(out->GetMask(), mask);
	delete out;
}

TEST(WebSocketFrame, ControlOpcodes)
{
	for (auto op : {WebSocketFrameHeader::Ping, WebSocketFrameHeader::Pong,
			WebSocketFrameHeader::Close})
	{
		WebSocketFrameHeader in(true, op, 0, 0);
		WebSocketFrameHeader* out = RoundTrip(in);
		ASSERT_NE(out, nullptr);
		EXPECT_EQ(out->GetOpCode(), op);
		EXPECT_EQ(out->GetPayloadLength(), 0u);
		delete out;
	}
}

TEST(WebSocketFrame, NonFinFragment)
{
	WebSocketFrameHeader in(false, WebSocketFrameHeader::TextFrame, 10, 0);
	WebSocketFrameHeader* out = RoundTrip(in);
	ASSERT_NE(out, nullptr);
	EXPECT_FALSE(out->IsFin());
	EXPECT_EQ(out->GetOpCode(), WebSocketFrameHeader::TextFrame);
	delete out;
}

// Le Parser doit reconstituer l'en-tête même en recevant les octets un par un.
TEST(WebSocketFrame, ParsedByteByByte)
{
	const DWORD mask = 0xCAFEBABE;
	WebSocketFrameHeader in(true, WebSocketFrameHeader::BinaryFrame, 300, mask);

	WebSocketFrameHeader::Parser parser;
	BYTE* data = in.GetData();
	DWORD total = in.GetSize();
	for (DWORD i = 0; i < total; ++i)
		parser.Parse(data + i, 1);

	ASSERT_TRUE(parser.IsParsed());
	WebSocketFrameHeader* out = parser.ConsumeHeader();
	ASSERT_NE(out, nullptr);
	EXPECT_EQ(out->GetPayloadLength(), 300u);
	EXPECT_EQ(out->GetMask(), mask);
	delete out;
}
