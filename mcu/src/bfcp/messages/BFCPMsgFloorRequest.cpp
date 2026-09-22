#include "bfcp/messages/BFCPMsgFloorRequest.h"
#include "log.h"


BFCPMsgFloorRequest::BFCPMsgFloorRequest(int transactionId, int conferenceId, int userId) :
		BFCPMessage(BFCPMessage::FloorRequest, transactionId, conferenceId, userId),
		beneficiaryId(NULL),
		participantProvidedInfo(NULL)
{
}


BFCPMsgFloorRequest::~BFCPMsgFloorRequest()
{
	for (size_t i=0; i < this->floorIds.size(); i++)
		delete this->floorIds[i];
	if (this->beneficiaryId)
		delete this->beneficiaryId;
	if (this->participantProvidedInfo)
		delete this->participantProvidedInfo;
}


bool BFCPMsgFloorRequest::IsValid()
{
	if (this->CountFloorIds() < 1) {
		::Error("BFCPMsgFloorRequest::IsValid() | MUST have at least one FloorId attribute\n");
		return false;
	}
	return true;
}


void BFCPMsgFloorRequest::Dump()
{
	::Debug("[BFCPMsgFloorRequest]\n");
	::Debug("- [primitive: FloorRequest, conferenceId: %d, userId: %d, transactionId: %d]\n", this->conferenceId, this->userId, this->transactionId);
	for (size_t i=0; i < this->floorIds.size(); i++)
		::Debug("- floorId: %d\n", GetFloorId(i));
	if (this->HasBeneficiaryId())
		::Debug("- beneficiaryId: %d\n", GetBeneficiaryId());
	if (this->HasParticipantProvidedInfo())
		::Debug("- participantProvidedInfo: %s\n", GetParticipantProvidedInfo().c_str());
	::Debug("[/BFCPMsgFloorRequest]\n");
}


void BFCPMsgFloorRequest::AddFloorId(int floorId)
{
	this->floorIds.push_back(new BFCPAttrFloorId(floorId));
}


void BFCPMsgFloorRequest::SetBeneficiaryId(int beneficiaryId)
{
	if (this->beneficiaryId)
		delete this->beneficiaryId;
	this->beneficiaryId = new BFCPAttrBeneficiaryId(beneficiaryId);
}


void BFCPMsgFloorRequest::SetParticipantProvidedInfo(const std::string& participantProvidedInfo)
{
	if (this->participantProvidedInfo)
		delete this->participantProvidedInfo;
	this->participantProvidedInfo = new BFCPAttrParticipantProvidedInfo(participantProvidedInfo);
}


bool BFCPMsgFloorRequest::HasBeneficiaryId() const
{
	return this->beneficiaryId != NULL;
}


bool BFCPMsgFloorRequest::HasParticipantProvidedInfo() const
{
	return this->participantProvidedInfo != NULL;
}


int BFCPMsgFloorRequest::GetFloorId(unsigned int index) const
{
	if (index >= this->floorIds.size())
		throw BFCPMessage::AttributeNotFound("'floorId' attribute not in range");
	return this->floorIds[index]->GetValue();
}


int BFCPMsgFloorRequest::CountFloorIds() const
{
	return this->floorIds.size();
}


int BFCPMsgFloorRequest::GetBeneficiaryId() const
{
	if (! this->beneficiaryId)
		throw BFCPMessage::AttributeNotFound("'beneficiaryId' attribute not found");
	return this->beneficiaryId->GetValue();
}


const std::string& BFCPMsgFloorRequest::GetParticipantProvidedInfo() const
{
	if (! this->participantProvidedInfo)
		throw BFCPMessage::AttributeNotFound("'participantProvidedInfo' attribute not found");
	return this->participantProvidedInfo->GetValue();
}
