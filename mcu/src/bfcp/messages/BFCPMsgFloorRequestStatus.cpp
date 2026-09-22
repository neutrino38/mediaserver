#include "bfcp/messages/BFCPMsgFloorRequestStatus.h"
#include "log.h"


BFCPMsgFloorRequestStatus::BFCPMsgFloorRequestStatus(int transactionId, int conferenceId, int userId) :
		BFCPMessage(BFCPMessage::FloorRequestStatus, transactionId, conferenceId, userId),
		floorRequestInformation(NULL)
{
}


BFCPMsgFloorRequestStatus::~BFCPMsgFloorRequestStatus()
{
	if (this->floorRequestInformation)
		delete this->floorRequestInformation;
}


bool BFCPMsgFloorRequestStatus::IsValid()
{
	if (! this->floorRequestInformation) {
		::Error("BFCPMsgFloorRequestStatus::IsValid() | MUST have a FloorRequestInformation attribute\n");
		return false;
	}
	return true;
}


void BFCPMsgFloorRequestStatus::Dump()
{
	::Debug("[BFCPMsgFloorRequestStatus]\n");
	::Debug("- [primitive: FloorRequestStatus, conferenceId: %d, userId: %d, transactionId: %d]\n", this->conferenceId, this->userId, this->transactionId);
	if (this->floorRequestInformation) {
		::Debug("- floorRequestInformation:\n");
		this->floorRequestInformation->Dump();
	}
	::Debug("[/BFCPMsgFloorRequestStatus]\n");
}


void BFCPMsgFloorRequestStatus::SetFloorRequestInformation(BFCPAttrFloorRequestInformation *floorRequestInformation)
{
	if (this->floorRequestInformation)
		delete this->floorRequestInformation;
	this->floorRequestInformation = floorRequestInformation;
}


void BFCPMsgFloorRequestStatus::SetDescription(const std::string& statusInfo)
{
	if (! this->floorRequestInformation)
		return;
	this->floorRequestInformation->SetDescription(statusInfo);
}


const BFCPAttrFloorRequestInformation* BFCPMsgFloorRequestStatus::GetFloorRequestInformation() const
{
	return this->floorRequestInformation;
}
