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
	if (this->supportedPrimitives.Count() < 1 || this->supportedAttributes.Count() < 1) {
		::Error("BFCPMsgHelloAck::IsValid() | MUST have SupportedPrimitives and SupportedAttributes\n");
		return false;
	}
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
