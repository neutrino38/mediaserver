#include "bfcp/messages/BFCPMsgHelloAck.h"
#include "log.h"


BFCPMsgHelloAck::BFCPMsgHelloAck(int transactionId, int conferenceId, int userId) :
		BFCPMessage(BFCPMessage::HelloAck, transactionId, conferenceId, userId)
{
}


BFCPMsgHelloAck::~BFCPMsgHelloAck()
{
}


bool BFCPMsgHelloAck::IsValid()
{
	// The grammar makes both lists mandatory, and we always send them. We do
	// NOT demand them on receipt: over UDP a HelloAck is what closes our Hello
	// transaction, and that job rests on the transaction id alone. Refusing a
	// peer's acknowledgement over a missing informational list would have us
	// retransmit until we gave up, and drop a participant that is right there.
	if (this->supportedPrimitives.Count() < 1 || this->supportedAttributes.Count() < 1)
		::Debug("BFCPMsgHelloAck::IsValid() | HelloAck without its supported lists, accepted anyway\n");
	return true;
}


void BFCPMsgHelloAck::Dump()
{
	::Debug("[BFCPMsgHelloAck]\n");
	::Debug("- [primitive: HelloAck, conferenceId: %d, userId: %d, transactionId: %d]\n", this->conferenceId, this->userId, this->transactionId);
	this->supportedPrimitives.Dump();
	this->supportedAttributes.Dump();
	::Debug("[/BFCPMsgHelloAck]\n");
}


void BFCPMsgHelloAck::AddSupportedPrimitive(enum BFCPMessage::Primitive primitive)
{
	this->supportedPrimitives.Add(primitive);
}


void BFCPMsgHelloAck::AddSupportedAttribute(enum BFCPAttribute::Name attribute)
{
	this->supportedAttributes.Add(attribute);
}


const BFCPAttrSupportedPrimitives& BFCPMsgHelloAck::GetSupportedPrimitives() const
{
	return this->supportedPrimitives;
}


const BFCPAttrSupportedAttributes& BFCPMsgHelloAck::GetSupportedAttributes() const
{
	return this->supportedAttributes;
}


size_t BFCPMsgHelloAck::SerializeAttributes(BYTE* out, size_t max) const
{
	size_t n = this->supportedPrimitives.Serialize(out, max, true);
	if (n == Failed)
		return Failed;

	const size_t written = this->supportedAttributes.Serialize(out + n, max - n, true);
	if (written == Failed)
		return Failed;

	return n + written;
}


bool BFCPMsgHelloAck::ParseAttributes(const BYTE* data, size_t size)
{
	BFCPAttrCursor cursor(data, size);

	while (cursor.Next()) {
		switch (cursor.Type()) {
			case BFCPAttribute::SupportedPrimitives:
				for (size_t i=0; i < cursor.ContentsLen(); i++)
					AddSupportedPrimitive((enum BFCPMessage::Primitive)cursor.Contents()[i]);
				break;
			case BFCPAttribute::SupportedAttributes:
				// 7-bit type plus a reserved bit, unlike a supported primitive.
				for (size_t i=0; i < cursor.ContentsLen(); i++)
					AddSupportedAttribute((enum BFCPAttribute::Name)(cursor.Contents()[i] >> 1));
				break;
			default:
				if (cursor.IsMandatory())
					return ::Error("BFCPMsgHelloAck::ParseAttributes() | unknown mandatory attribute %d\n", cursor.Type());
				break;
		}
	}

	return ! cursor.Malformed();
}
