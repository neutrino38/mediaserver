#include "bfcp/attributes/BFCPAttrFloorRequestInformation.h"
#include "log.h"
#include <algorithm>


BFCPAttrFloorRequestInformation::BFCPAttrFloorRequestInformation(int floorRequestId) :
	floorRequestId(new BFCPAttrFloorRequestId(floorRequestId)),
	overallRequestStatus(NULL),
	beneficiaryInformation(NULL),
	requestedByInformation(NULL)
{
}


BFCPAttrFloorRequestInformation::~BFCPAttrFloorRequestInformation()
{
	delete this->floorRequestId;
	for (size_t i=0; i < this->floorRequestStatuses.size(); i++)
		delete this->floorRequestStatuses[i];
	if (this->overallRequestStatus)
		delete this->overallRequestStatus;
	if (this->beneficiaryInformation)
		delete this->beneficiaryInformation;
	if (this->requestedByInformation)
		delete this->requestedByInformation;
}


void BFCPAttrFloorRequestInformation::Dump()
{
	::Debug("[BFCPAttrFloorRequestInformation]\n");
	::Debug("- floorRequestId: %d\n", this->floorRequestId->GetValue());
	for (size_t i=0; i < this->floorRequestStatuses.size(); i++) {
		::Debug("- floorRequestStatus:\n");
		this->floorRequestStatuses[i]->Dump();
	}
	if (this->overallRequestStatus) {
		::Debug("- overallRequestStatus:\n");
		this->overallRequestStatus->Dump();
	}
	if (this->beneficiaryInformation) {
		::Debug("- beneficiaryInformation:\n");
		this->beneficiaryInformation->Dump();
	}
	if (this->requestedByInformation) {
		::Debug("- requestedByInformation:\n");
		this->requestedByInformation->Dump();
	}
	::Debug("[/BFCPAttrFloorRequestInformation]\n");
}


void BFCPAttrFloorRequestInformation::AddFloorRequestStatus(BFCPAttrFloorRequestStatus *floorRequestStatus)
{
	if (std::find(this->floorRequestStatuses.begin(), this->floorRequestStatuses.end(), floorRequestStatus) != this->floorRequestStatuses.end())
		return;
	this->floorRequestStatuses.push_back(floorRequestStatus);
}


void BFCPAttrFloorRequestInformation::SetOverallRequestStatus(BFCPAttrOverallRequestStatus *overallRequestStatus)
{
	if (this->overallRequestStatus)
		delete this->overallRequestStatus;
	this->overallRequestStatus = overallRequestStatus;
}


void BFCPAttrFloorRequestInformation::SetBeneficiaryInformation(BFCPAttrBeneficiaryInformation *beneficiaryInformation)
{
	if (this->beneficiaryInformation)
		delete this->beneficiaryInformation;
	this->beneficiaryInformation = beneficiaryInformation;
}


void BFCPAttrFloorRequestInformation::SetRequestedByInformation(BFCPAttrRequestedByInformation *requestedByInformation)
{
	if (this->requestedByInformation)
		delete this->requestedByInformation;
	this->requestedByInformation = requestedByInformation;
}


void BFCPAttrFloorRequestInformation::SetDescription(const std::string& statusInfo)
{
	if (! this->overallRequestStatus)
		return;
	this->overallRequestStatus->SetStatusInfo(new BFCPAttrStatusInfo(statusInfo));
}


int BFCPAttrFloorRequestInformation::GetFloorRequestId() const
{
	return this->floorRequestId->GetValue();
}


int BFCPAttrFloorRequestInformation::CountFloorRequestStatuses() const
{
	return this->floorRequestStatuses.size();
}


const BFCPAttrFloorRequestStatus* BFCPAttrFloorRequestInformation::GetFloorRequestStatus(unsigned int index) const
{
	if (index >= this->floorRequestStatuses.size())
		return NULL;
	return this->floorRequestStatuses[index];
}


const BFCPAttrOverallRequestStatus* BFCPAttrFloorRequestInformation::GetOverallRequestStatus() const
{
	return this->overallRequestStatus;
}


const BFCPAttrBeneficiaryInformation* BFCPAttrFloorRequestInformation::GetBeneficiaryInformation() const
{
	return this->beneficiaryInformation;
}


const BFCPAttrRequestedByInformation* BFCPAttrFloorRequestInformation::GetRequestedByInformation() const
{
	return this->requestedByInformation;
}
