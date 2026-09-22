#include "bfcp/attributes/BFCPAttrFloorRequestStatus.h"
#include "log.h"


BFCPAttrFloorRequestStatus::BFCPAttrFloorRequestStatus(int floorId) :
	floorId(new BFCPAttrFloorId(floorId)),
	requestStatus(NULL)
{
}


BFCPAttrFloorRequestStatus::~BFCPAttrFloorRequestStatus()
{
	delete this->floorId;
	if (this->requestStatus)
		delete this->requestStatus;
}


void BFCPAttrFloorRequestStatus::Dump()
{
	::Debug("[BFCPAttrFloorRequestStatus]\n");
	::Debug("- floorId: %d\n", this->floorId->GetValue());
	if (this->requestStatus) {
		::Debug("- requestStatus:\n");
		this->requestStatus->Dump();
	}
	::Debug("[/BFCPAttrFloorRequestStatus]\n");
}


void BFCPAttrFloorRequestStatus::SetRequestStatus(BFCPAttrRequestStatus *requestStatus)
{
	if (this->requestStatus)
		delete this->requestStatus;
	this->requestStatus = requestStatus;
}


int BFCPAttrFloorRequestStatus::GetFloorId() const
{
	return this->floorId->GetValue();
}


const BFCPAttrRequestStatus* BFCPAttrFloorRequestStatus::GetRequestStatus() const
{
	return this->requestStatus;
}
