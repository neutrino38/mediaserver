#include "bfcp/messages/BFCPMsgHello.h"
#include "log.h"


BFCPMsgHello::BFCPMsgHello(int transactionId, int conferenceId, int userId) :
		BFCPMessage(BFCPMessage::Hello, transactionId, conferenceId, userId)
{
}


BFCPMsgHello::~BFCPMsgHello()
{
}


void BFCPMsgHello::Dump()
{
	::Debug("[BFCPMsgHello]\n");
	::Debug("- [primitive: Hello, conferenceId: %d, userId: %d, transactionId: %d]\n", this->conferenceId, this->userId, this->transactionId);
	::Debug("[/BFCPMsgHello]\n");
}
