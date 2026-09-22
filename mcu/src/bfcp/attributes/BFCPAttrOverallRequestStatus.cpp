#include "bfcp/attributes/BFCPAttrOverallRequestStatus.h"
#include "log.h"


BFCPAttrOverallRequestStatus::BFCPAttrOverallRequestStatus(int floorRequestId) :
	floorRequestId(new BFCPAttrFloorRequestId(floorRequestId)),
	requestStatus(NULL),
	statusInfo(NULL)
{
}


BFCPAttrOverallRequestStatus::~BFCPAttrOverallRequestStatus()
{
	delete this->floorRequestId;
	if (this->requestStatus)
		delete this->requestStatus;
	if (this->statusInfo)
		delete this->statusInfo;
}


void BFCPAttrOverallRequestStatus::Dump()
{
	::Debug("[BFCPAttrOverallRequestStatus]\n");
	::Debug("- floorRequestId: %d\n", this->floorRequestId->GetValue());
	if (this->requestStatus) {
		::Debug("- requestStatus:\n");
		this->requestStatus->Dump();
	}
	if (this->statusInfo) {
		::Debug("- statusInfo: %s\n", this->statusInfo->GetValue().c_str());
	}
	::Debug("[/BFCPAttrOverallRequestStatus]\n");
}


void BFCPAttrOverallRequestStatus::SetRequestStatus(BFCPAttrRequestStatus *requestStatus)
{
	if (this->requestStatus)
		delete this->requestStatus;
	this->requestStatus = requestStatus;
}


void BFCPAttrOverallRequestStatus::SetStatusInfo(BFCPAttrStatusInfo *statusInfo)
{
	if (this->statusInfo)
		delete this->statusInfo;
	this->statusInfo = statusInfo;
}


int BFCPAttrOverallRequestStatus::GetFloorRequestId() const
{
	return this->floorRequestId->GetValue();
}


const BFCPAttrRequestStatus* BFCPAttrOverallRequestStatus::GetRequestStatus() const
{
	return this->requestStatus;
}


const BFCPAttrStatusInfo* BFCPAttrOverallRequestStatus::GetStatusInfo() const
{
	return this->statusInfo;
}
