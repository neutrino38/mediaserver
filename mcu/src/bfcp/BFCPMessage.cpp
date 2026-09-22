#include "bfcp/BFCPMessage.h"
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
	userId(userId)
{
}


BFCPMessage::~BFCPMessage()
{
}


bool BFCPMessage::IsValid()
{
	return true;
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
