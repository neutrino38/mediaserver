#include "bfcp/messages/BFCPMsgFloorStatus.h"
#include "log.h"
#include <algorithm>


BFCPMsgFloorStatus::BFCPMsgFloorStatus(int transactionId, int conferenceId, int userId) :
		BFCPMessage(BFCPMessage::FloorStatus, transactionId, conferenceId, userId),
		floorId(NULL)
{
}


BFCPMsgFloorStatus::~BFCPMsgFloorStatus()
{
	if (this->floorId)
		delete this->floorId;
	for (size_t i=0; i < this->floorRequestInformations.size(); i++)
		delete this->floorRequestInformations[i];
}


void BFCPMsgFloorStatus::Dump()
{
	::Debug("[BFCPMsgFloorStatus]\n");
	::Debug("- [primitive: FloorStatus, conferenceId: %d, userId: %d, transactionId: %d]\n", this->conferenceId, this->userId, this->transactionId);
	if (this->floorId)
		::Debug("- floorId: %d\n", this->floorId->GetValue());
	for (size_t i=0; i < this->floorRequestInformations.size(); i++) {
		::Debug("- floorRequestInformation:\n");
		this->floorRequestInformations[i]->Dump();
	}
	::Debug("[/BFCPMsgFloorStatus]\n");
}


void BFCPMsgFloorStatus::SetFloorId(int floorId)
{
	if (this->floorId)
		delete this->floorId;
	this->floorId = new BFCPAttrFloorId(floorId);
}


void BFCPMsgFloorStatus::AddFloorRequestInformation(BFCPAttrFloorRequestInformation* floorRequestInformation)
{
	if (std::find(this->floorRequestInformations.begin(), this->floorRequestInformations.end(), floorRequestInformation) != this->floorRequestInformations.end())
		return;
	this->floorRequestInformations.push_back(floorRequestInformation);
}


bool BFCPMsgFloorStatus::HasFloorId() const
{
	return this->floorId != NULL;
}


int BFCPMsgFloorStatus::GetFloorId() const
{
	if (! this->floorId)
		throw BFCPMessage::AttributeNotFound("'floorId' attribute not found");
	return this->floorId->GetValue();
}


int BFCPMsgFloorStatus::CountFloorRequestInformations() const
{
	return this->floorRequestInformations.size();
}


const BFCPAttrFloorRequestInformation* BFCPMsgFloorStatus::GetFloorRequestInformation(unsigned int index) const
{
	if (index >= this->floorRequestInformations.size())
		return NULL;
	return this->floorRequestInformations[index];
}
