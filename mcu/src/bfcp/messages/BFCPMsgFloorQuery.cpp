#include "bfcp/messages/BFCPMsgFloorQuery.h"
#include "log.h"


BFCPMsgFloorQuery::BFCPMsgFloorQuery(int transactionId, int conferenceId, int userId) :
		BFCPMessage(BFCPMessage::FloorQuery, transactionId, conferenceId, userId)
{
}


BFCPMsgFloorQuery::~BFCPMsgFloorQuery()
{
	for (size_t i=0; i < this->floorIds.size(); i++)
		delete this->floorIds[i];
}


bool BFCPMsgFloorQuery::IsValid()
{
	return true;
}


void BFCPMsgFloorQuery::Dump()
{
	::Debug("[BFCPMsgFloorQuery]\n");
	::Debug("- [primitive: FloorQuery, conferenceId: %d, userId: %d, transactionId: %d]\n", this->conferenceId, this->userId, this->transactionId);
	for (size_t i=0; i < this->floorIds.size(); i++)
		::Debug("- floorId: %d\n", GetFloorId(i));
	::Debug("[/BFCPMsgFloorQuery]\n");
}


void BFCPMsgFloorQuery::AddFloorId(int floorId)
{
	this->floorIds.push_back(new BFCPAttrFloorId(floorId));
}


int BFCPMsgFloorQuery::GetFloorId(unsigned int index) const
{
	if (index >= this->floorIds.size())
		throw BFCPMessage::AttributeNotFound("'floorId' attribute not in range");
	return this->floorIds[index]->GetValue();
}


int BFCPMsgFloorQuery::CountFloorIds() const
{
	return this->floorIds.size();
}
