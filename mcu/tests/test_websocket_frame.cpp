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

#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <string>

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

// Une trame lue sur le fil, en-tête décodé et payload rendu en clair.
struct WireFrame
{
	bool				fin = false;
	WebSocketFrameHeader::OpCode	opCode = WebSocketFrameHeader::ContinuationFrame;
	bool				masked = false;
	DWORD				mask = 0;
	std::string			payload;
	DWORD				consumed = 0;
};

// Décode UNE trame depuis `data`. Le démasquage reproduit la convention du
// serveur (mask[0] = octet de poids fort, offset relatif au début du payload
// de CETTE trame) : c'est précisément ce que le lot 1 doit produire.
WireFrame DecodeFrame(const BYTE* data, DWORD size)
{
	WireFrame out;

	WebSocketFrameHeader::Parser parser;
	DWORD consumed = parser.Parse((BYTE*)data, size);
	EXPECT_TRUE(parser.IsParsed());
	if (!parser.IsParsed())
		return out;

	WebSocketFrameHeader* header = parser.ConsumeHeader();
	out.fin    = header->IsFin();
	out.opCode = header->GetOpCode();
	out.masked = header->IsMasked();
	out.mask   = header->GetMask();

	const QWORD len = header->GetPayloadLength();
	EXPECT_LE(consumed+len, size);

	BYTE maskBytes[4];
	set4(maskBytes, 0, out.mask);
	for (QWORD i = 0; i < len && consumed+i < size; ++i)
	{
		BYTE b = data[consumed+i];
		out.payload.push_back((char)(out.masked ? (b ^ maskBytes[i & 3]) : b));
	}
	out.consumed = consumed+len;

	delete header;
	return out;
}

WireFrame DecodeFrame(const std::string& wire)
{
	return DecodeFrame((const BYTE*)wire.data(), wire.size());
}

// Sérialise une Frame telle que la connexion la pousserait sur le socket.
std::string Serialize(WebSocketConnection::Frame& frame)
{
	return std::string((const char*)frame.GetData(), frame.GetSize());
}

// Listener minimal : la connexion en exige un, rien ici n'a besoin d'agir.
class SilentConnectionListener : public WebSocketConnection::Listener
{
public:
	virtual void onUpgradeRequest(WebSocketConnection*)	{ upgrades++; }
	virtual void onWakeupNeeded()				{ wakeups++;  }

	int upgrades = 0;
	int wakeups  = 0;
};

// Une connexion posée sur un socketpair : on lit à l'autre bout ce qu'elle écrit.
class ConnectionOnPipe
{
public:
	ConnectionOnPipe(WebSocketConnection::Role role)
	{
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
		{
			fds[0] = fds[1] = -1;
			return;
		}
		timeval tv{2, 0};
		setsockopt(fds[1], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

		conn = std::make_shared<WebSocketConnection>(&listener, 1);
		conn->Init(fds[0], std::unique_ptr<WebSocketTransport>(new WebSocketPlainTransport()), role);
	}

	~ConnectionOnPipe()
	{
		conn.reset();		//ferme fds[0]
		if (fds[1] != -1) close(fds[1]);
	}

	bool IsUsable() const	{ return fds[0] != -1; }

	// Écoule tout ce que la connexion a en attente, puis lit le fil.
	std::string Drain()
	{
		for (int i = 0; i < 64 && (conn->GetPollEvents() & POLLOUT); ++i)
			conn->OnWritable();

		std::string acc;
		BYTE buf[4096];
		while (true)
		{
			ssize_t n = read(fds[1], buf, sizeof(buf));
			if (n <= 0)
				break;
			acc.append((const char*)buf, n);
			if ((size_t)n < sizeof(buf))
				break;
		}
		return acc;
	}

	SilentConnectionListener		listener;
	std::shared_ptr<WebSocketConnection>	conn;
	int					fds[2];
};

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

/***********************************************************************
 * Masquage selon le rôle (lot 1 de docs/conception/WS-CLIENT/SPEC.md)
 *
 * RFC 6455 §5.3 : un client masque TOUTES ses trames sortantes, un serveur
 * n'en masque aucune. Jusqu'ici le mediaserver ne savait qu'être serveur :
 * Frame posait toujours `mask = 0` et ne chiffrait jamais son corps. Un pair
 * conforme ferme la connexion sur une trame cliente non masquée — le mode
 * client échouerait donc juste après le 101, sans rien dire.
 *
 * Le versant réception du même §5.1 — un client ferme sur une trame masquée —
 * est écrit dans ProcessData mais N'EST PAS exercé ici : il faut une connexion
 * cliente ouverte, donc la poignée de main du lot 2.
 ***********************************************************************/

TEST(WebSocketFrameMasking, UneTrameClienteEstMasqueeEtSeDemasque)
{
	const std::string payload = "bonjour";

	WebSocketConnection::Frame frame(true, WebSocketFrameHeader::TextFrame,
					 (const BYTE*)payload.data(), payload.size(), true);
	const std::string wire = Serialize(frame);

	// 1 + 1 + 4 octets de masque + payload
	ASSERT_EQ(2u+4u+payload.size(), wire.size());
	// Le corps est brouillé sur le fil : la clé n'est jamais nulle
	EXPECT_EQ(std::string::npos, wire.find(payload));

	WireFrame decoded = DecodeFrame(wire);
	EXPECT_TRUE(decoded.fin);
	EXPECT_EQ(WebSocketFrameHeader::TextFrame, decoded.opCode);
	EXPECT_TRUE(decoded.masked);
	EXPECT_NE(0u, decoded.mask);
	EXPECT_EQ(payload, decoded.payload);
}

TEST(WebSocketFrameMasking, UneTrameServeurResteEnClair)
{
	const std::string payload = "bonjour";

	WebSocketConnection::Frame frame(true, WebSocketFrameHeader::TextFrame,
					 (const BYTE*)payload.data(), payload.size(), false);
	const std::string wire = Serialize(frame);

	ASSERT_EQ(2u+payload.size(), wire.size());
	EXPECT_NE(std::string::npos, wire.find(payload));

	WireFrame decoded = DecodeFrame(wire);
	EXPECT_FALSE(decoded.masked);
	EXPECT_EQ(payload, decoded.payload);
}

// « Imprévisible » (§5.3) veut dire tirée par trame : un masque constant
// rendrait le flux XOR-triviaux à retrouver.
TEST(WebSocketFrameMasking, ChaqueTramePorteSaPropreCle)
{
	const std::string payload = "meme contenu a chaque fois";

	DWORD first = 0;
	int distincts = 0;
	for (int i = 0; i < 16; ++i)
	{
		WebSocketConnection::Frame frame(true, WebSocketFrameHeader::TextFrame,
						 (const BYTE*)payload.data(), payload.size(), true);
		WireFrame decoded = DecodeFrame(Serialize(frame));
		ASSERT_EQ(payload, decoded.payload);
		if (!i)
			first = decoded.mask;
		else if (decoded.mask != first)
			distincts++;
	}
	EXPECT_GT(distincts, 0);
}

// Le corps du pong est recopié APRÈS la construction, morceau par morceau, au
// rythme où arrive le ping. L'offset du masque doit donc suivre la position
// dans le payload, et non repartir de zéro à chaque Append.
TEST(WebSocketFrameMasking, LePongMasqueSonCorpsAjouteEnPlusieursFois)
{
	const std::string body = "0123456789abcdef";

	WebSocketConnection::Frame pong(true, WebSocketFrameHeader::Pong,
					NULL, body.size(), true);
	ASSERT_TRUE(pong.Append((const BYTE*)body.data(), 3));
	ASSERT_TRUE(pong.Append((const BYTE*)body.data()+3, 1));
	ASSERT_TRUE(pong.Append((const BYTE*)body.data()+4, body.size()-4));

	WireFrame decoded = DecodeFrame(Serialize(pong));
	EXPECT_EQ(WebSocketFrameHeader::Pong, decoded.opCode);
	EXPECT_TRUE(decoded.masked);
	EXPECT_EQ(body, decoded.payload);
}

TEST(WebSocketFrameMasking, UneConnexionClienteMasqueCeQuElleEcritSurLeFil)
{
	ConnectionOnPipe pipe(WebSocketConnection::Client);
	if (!pipe.IsUsable())
		GTEST_SKIP() << "socketpair indisponible";

	EXPECT_TRUE(pipe.conn->IsClient());

	pipe.conn->SendMessage(std::string("salut"));
	const std::string wire = pipe.Drain();

	ASSERT_GE(wire.size(), 2u+4u+5u);
	WireFrame decoded = DecodeFrame(wire);
	EXPECT_TRUE(decoded.masked);
	EXPECT_EQ("salut", decoded.payload);
}

TEST(WebSocketFrameMasking, UneConnexionServeurNeMasqueRien)
{
	ConnectionOnPipe pipe(WebSocketConnection::Server);
	if (!pipe.IsUsable())
		GTEST_SKIP() << "socketpair indisponible";

	EXPECT_FALSE(pipe.conn->IsClient());

	pipe.conn->SendMessage(std::string("salut"));
	const std::string wire = pipe.Drain();

	ASSERT_EQ(2u+5u, wire.size());
	WireFrame decoded = DecodeFrame(wire);
	EXPECT_FALSE(decoded.masked);
	EXPECT_EQ("salut", decoded.payload);
}

// Piège 5 du SPEC : SendMessage(BYTE*,DWORD) découpe en fragments de 1300
// octets avec continuation. Chaque fragment est une trame à part entière,
// donc porte son propre masque dont l'offset repart de zéro.
TEST(WebSocketFrameMasking, ChaqueFragmentPorteSonMasqueDepuisLOffsetZero)
{
	ConnectionOnPipe pipe(WebSocketConnection::Client);
	if (!pipe.IsUsable())
		GTEST_SKIP() << "socketpair indisponible";

	std::string payload;
	for (size_t i = 0; i < 2000; ++i)
		payload.push_back((char)('a'+(i%26)));

	pipe.conn->SendMessage((const BYTE*)payload.data(), payload.size());
	const std::string wire = pipe.Drain();

	WireFrame first = DecodeFrame((const BYTE*)wire.data(), wire.size());
	ASSERT_EQ(1300u, first.payload.size());
	EXPECT_FALSE(first.fin);
	EXPECT_EQ(WebSocketFrameHeader::BinaryFrame, first.opCode);
	EXPECT_TRUE(first.masked);

	ASSERT_LT(first.consumed, wire.size());
	WireFrame second = DecodeFrame((const BYTE*)wire.data()+first.consumed,
				       wire.size()-first.consumed);
	ASSERT_EQ(700u, second.payload.size());
	EXPECT_TRUE(second.fin);
	EXPECT_EQ(WebSocketFrameHeader::ContinuationFrame, second.opCode);
	EXPECT_TRUE(second.masked);

	// Le message se reconstitue à l'identique, fragment par fragment
	EXPECT_EQ(payload, first.payload+second.payload);
}
