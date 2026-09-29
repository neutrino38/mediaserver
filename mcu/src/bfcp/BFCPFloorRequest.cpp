#include "bfcp/BFCPFloorRequest.h"
#include "log.h"


BFCPFloorRequest::BFCPFloorRequest(int floorRequestId, int userId, int conferenceId) :
	floorRequestId(floorRequestId),
	userId(userId),
	conferenceId(conferenceId),
	beneficiaryId(userId),  // By default beneficiaryId = userId.
	status(BFCPAttrRequestStatus::Pending),
	queuePosition(0)
{
}


void BFCPFloorRequest::SetBeneficiaryId(int beneficiaryId)
{
	this->beneficiaryId = beneficiaryId;
}


void BFCPFloorRequest::AddFloorId(int floorId)
{
	this->floorIds.insert(floorId);
}


void BFCPFloorRequest::SetStatus(enum BFCPAttrRequestStatus::Status status)
{
	this->status = status;
}


void BFCPFloorRequest::SetQueuePosition(int queuePosition)
{
	this->queuePosition = queuePosition;
}


bool BFCPFloorRequest::HasFloorId(int floorId) const
{
	return this->floorIds.find(floorId) != this->floorIds.end();
}


int BFCPFloorRequest::GetFloorRequestId() const
{
	return this->floorRequestId;
}


int BFCPFloorRequest::GetUserId() const
{
	return this->userId;
}


int BFCPFloorRequest::GetBeneficiaryId() const
{
	return this->beneficiaryId;
}


std::set<int> BFCPFloorRequest::GetFloorIds() const
{
	return this->floorIds;
}


enum BFCPAttrRequestStatus::Status BFCPFloorRequest::GetStatus() const
{
	return this->status;
}


const char* BFCPFloorRequest::GetStatusName() const
{
	return BFCPAttrRequestStatus::StatusName(this->status);
}


bool BFCPFloorRequest::IsGranted() const
{
	return this->status == BFCPAttrRequestStatus::Granted;
}


bool BFCPFloorRequest::CanBeGranted() const
{
	return this->status == BFCPAttrRequestStatus::Pending || this->status == BFCPAttrRequestStatus::Accepted;
}


bool BFCPFloorRequest::CanBeDenied() const
{
	return CanBeGranted();
}


bool BFCPFloorRequest::CanBeCancelled() const
{
	return CanBeGranted();
}


int BFCPFloorRequest::GetQueuePosition() const
{
	return this->queuePosition;
}


void BFCPFloorRequest::Dump()
{
	::Debug("[BFCPFloorRequest]\n");
	::Debug("- floorRequestId: %d\n", this->floorRequestId);
	::Debug("- userId: %d\n", this->userId);
	::Debug("- conferenceId: %d\n", this->conferenceId);
	::Debug("- beneficiaryId: %d\n", this->beneficiaryId);
	for (std::set<int>::const_iterator it = this->floorIds.begin(); it != this->floorIds.end(); ++it)
		::Debug("- floorId: %d\n", *it);
	::Debug("- status: %s\n", GetStatusName());
	::Debug("- queuePosition: %d\n", this->queuePosition);
	::Debug("[/BFCPFloorRequest]\n");
}


BFCPMsgFloorRequestStatus* BFCPFloorRequest::CreateFloorRequestStatus(int transactionId) const
{
	BFCPMsgFloorRequestStatus *floorRequestStatus = new BFCPMsgFloorRequestStatus(transactionId, this->conferenceId, this->userId);
	floorRequestStatus->SetFloorRequestInformation(CreateFloorRequestInformation());
	return floorRequestStatus;
}


BFCPAttrFloorRequestInformation* BFCPFloorRequest::CreateFloorRequestInformation() const
{
	BFCPAttrFloorRequestInformation *floorRequestInformation = new BFCPAttrFloorRequestInformation(this->floorRequestId);

	// One FloorRequestStatus per floor, each with the request status.
	for (std::set<int>::const_iterator it = this->floorIds.begin(); it != this->floorIds.end(); ++it) {
		BFCPAttrFloorRequestStatus *floorRequestStatus = new BFCPAttrFloorRequestStatus(*it);
		floorRequestStatus->SetRequestStatus(new BFCPAttrRequestStatus(this->status, this->queuePosition));
		floorRequestInformation->AddFloorRequestStatus(floorRequestStatus);
	}

	BFCPAttrOverallRequestStatus* overallRequestStatus = new BFCPAttrOverallRequestStatus(this->floorRequestId);
	overallRequestStatus->SetRequestStatus(new BFCPAttrRequestStatus(this->status, this->queuePosition));
	floorRequestInformation->SetOverallRequestStatus(overallRequestStatus);

	floorRequestInformation->SetBeneficiaryInformation(new BFCPAttrBeneficiaryInformation(this->beneficiaryId));
	floorRequestInformation->SetRequestedByInformation(new BFCPAttrRequestedByInformation(this->userId));

	return floorRequestInformation;
}
