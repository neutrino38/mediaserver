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


size_t BFCPMsgFloorRequestStatus::SerializeAttributes(BYTE* out, size_t max) const
{
	if (! this->floorRequestInformation)
		return Failed;
	return this->floorRequestInformation->Serialize(out, max, true);
}


bool BFCPMsgFloorRequestStatus::ParseAttributes(const BYTE* data, size_t size)
{
	BFCPAttrCursor cursor(data, size);

	while (cursor.Next()) {
		switch (cursor.Type()) {
			case BFCPAttribute::FloorRequestInformation:
			{
				BFCPAttrFloorRequestInformation* info = BFCPAttrFloorRequestInformation::Parse(cursor.Contents(), cursor.ContentsLen());
				if (! info)
					return false;
				SetFloorRequestInformation(info);
				break;
			}
			default:
				if (cursor.IsMandatory())
					return ::Error("BFCPMsgFloorRequestStatus::ParseAttributes() | unknown mandatory attribute %d\n", cursor.Type());
				break;
		}
	}

	return ! cursor.Malformed();
}
