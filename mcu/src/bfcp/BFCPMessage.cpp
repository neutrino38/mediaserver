#include "bfcp/BFCPMessage.h"
#include "bfcp/messages.h"
#include "log.h"


BFCPMessage::AttributeNotFound::AttributeNotFound(const char* description) :
	std::runtime_error(description)
{
}


const char* BFCPMessage::PrimitiveName(enum BFCPMessage::Primitive primitive)
{
	switch (primitive)
	{
		case FloorRequest:		return "FloorRequest";
		case FloorRelease:		return "FloorRelease";
		case FloorRequestQuery:		return "FloorRequestQuery";
		case FloorRequestStatus:	return "FloorRequestStatus";
		case UserQuery:			return "UserQuery";
		case UserStatus:		return "UserStatus";
		case FloorQuery:		return "FloorQuery";
		case FloorStatus:		return "FloorStatus";
		case ChairAction:		return "ChairAction";
		case ChairActionAck:		return "ChairActionAck";
		case Hello:			return "Hello";
		case HelloAck:			return "HelloAck";
		case Error:			return "Error";
		case FloorRequestStatusAck:	return "FloorRequestStatusAck";
		case FloorStatusAck:		return "FloorStatusAck";
		case Goodbye:			return "Goodbye";
		case GoodbyeAck:		return "GoodbyeAck";
	}
	return "Unknown";
}


BFCPMessage::BFCPMessage(enum BFCPMessage::Primitive primitive, int transactionId, int conferenceId, int userId) :
	primitive(primitive),
	transactionId(transactionId),
	conferenceId(conferenceId),
	userId(userId),
	version(VersionReliable),
	responder(false)
{
}


BFCPMessage::~BFCPMessage()
{
}


bool BFCPMessage::IsValid()
{
	return true;
}


size_t BFCPMessage::SerializeAttributes(BYTE* out, size_t max) const
{
	return 0;
}


bool BFCPMessage::ParseAttributes(const BYTE* data, size_t size)
{
	// A primitive we do not model: walk its attributes to prove they are well
	// formed, keep none.
	BFCPAttrCursor cursor(data, size);
	while (cursor.Next())
		;
	return ! cursor.Malformed();
}


size_t BFCPMessage::Serialize(BYTE* out, size_t max) const
{
	if (max < HeaderLen)
		return Failed;

	const size_t payload = SerializeAttributes(out + HeaderLen, max - HeaderLen);
	if (payload == Failed)
		return Failed;

	// Every attribute is padded to 4 octets, so the payload always is too. If
	// it were not, Payload Length could not say its size.
	if (payload % 4) {
		::Error("BFCPMessage::Serialize() | payload of %zu octets is not a multiple of 4\n", payload);
		return Failed;
	}
	if (payload / 4 > 0xFFFF) {
		::Error("BFCPMessage::Serialize() | payload of %zu octets overflows Payload Length\n", payload);
		return Failed;
	}

	out[0] = (BYTE)((this->version << 5) | (this->responder ? 0x10 : 0x00));
	out[1] = (BYTE)this->primitive;
	set2(out, 2, payload / 4);
	set4(out, 4, this->conferenceId);
	set2(out, 8, this->transactionId);
	set2(out, 10, this->userId);

	return HeaderLen + payload;
}


BFCPMessage* BFCPMessage::Parse(const BYTE* data, size_t size)
{
	if (size < HeaderLen) {
		::Error("BFCPMessage::Parse() | %zu octets, too few for a common header\n", size);
		return NULL;
	}

	const int version	= data[0] >> 5;
	const bool responder	= (data[0] & 0x10) != 0;
	const bool fragmented	= (data[0] & 0x08) != 0;

	if (version != VersionReliable && version != VersionUnreliable) {
		::Error("BFCPMessage::Parse() | unsupported version %d\n", version);
		return NULL;
	}

	// We never fragment and never reassemble: a fragment is dropped, loudly,
	// rather than half-read (docs/conception/BFCP-INTERNE/SPEC.md §4.3).
	if (fragmented) {
		::Error("BFCPMessage::Parse() | fragmented message dropped, reassembly is not supported\n");
		return NULL;
	}

	const size_t payload = get2(data, 2) * 4;
	if (HeaderLen + payload > size) {
		::Error("BFCPMessage::Parse() | header claims %zu octets of payload, %zu held\n", payload, size - HeaderLen);
		return NULL;
	}

	const int primitive	= data[1];
	const int conferenceId	= get4(data, 4);
	const int transactionId	= get2(data, 8);
	const int userId	= get2(data, 10);

	BFCPMessage* msg = NULL;
	switch (primitive)
	{
		case FloorRequest:	msg = new BFCPMsgFloorRequest(transactionId, conferenceId, userId);	break;
		case FloorRelease:	msg = new BFCPMsgFloorRelease(transactionId, conferenceId, userId);	break;
		case FloorQuery:	msg = new BFCPMsgFloorQuery(transactionId, conferenceId, userId);	break;
		case FloorRequestStatus:msg = new BFCPMsgFloorRequestStatus(transactionId, conferenceId, userId);break;
		case FloorStatus:	msg = new BFCPMsgFloorStatus(transactionId, conferenceId, userId);	break;
		case Hello:		msg = new BFCPMsgHello(transactionId, conferenceId, userId);		break;
		case HelloAck:		msg = new BFCPMsgHelloAck(transactionId, conferenceId, userId);		break;
		case Error:		msg = new BFCPMsgError(transactionId, conferenceId, userId);		break;
		case Goodbye:
		case GoodbyeAck:
		case FloorRequestStatusAck:
		case FloorStatusAck:
			// These carry no attribute: the base class is the whole message.
			msg = new BFCPMessage((enum Primitive)primitive, transactionId, conferenceId, userId);
			break;
		default:
			// A primitive we do not model is still handed up, so the server can
			// answer UnknownPrimitive rather than stay silent.
			msg = new BFCPMessage((enum Primitive)primitive, transactionId, conferenceId, userId);
			break;
	}

	msg->version	= version;
	msg->responder	= responder;

	if (! msg->ParseAttributes(data + HeaderLen, payload)) {
		::Error("BFCPMessage::Parse() | bad attributes in a %s\n", PrimitiveName(msg->GetPrimitive()));
		delete msg;
		return NULL;
	}

	if (! msg->IsValid()) {
		::Error("BFCPMessage::Parse() | a %s is missing a mandatory attribute\n", PrimitiveName(msg->GetPrimitive()));
		delete msg;
		return NULL;
	}

	return msg;
}


void BFCPMessage::SetVersion(int version)
{
	this->version = version;
}


int BFCPMessage::GetVersion() const
{
	return this->version;
}


void BFCPMessage::SetResponder(bool responder)
{
	this->responder = responder;
}


bool BFCPMessage::IsResponder() const
{
	return this->responder;
}


void BFCPMessage::SetUserId(int userId)
{
	this->userId = userId;
}


void BFCPMessage::SetTransactionId(int transactionId)
{
	this->transactionId = transactionId;
}


enum BFCPMessage::Primitive BFCPMessage::GetPrimitive() const
{
	return this->primitive;
}


int BFCPMessage::GetTransactionId() const
{
	return this->transactionId;
}


int BFCPMessage::GetConferenceId() const
{
	return this->conferenceId;
}


int BFCPMessage::GetUserId() const
{
	return this->userId;
}


void BFCPMessage::Dump()
{
	::Debug("[BFCPMessage]\n");
	::Debug("- [primitive: %s, conferenceId: %d, userId: %d, transactionId: %d]\n", PrimitiveName(this->primitive), this->conferenceId, this->userId, this->transactionId);
	::Debug("[/BFCPMessage]\n");
}
