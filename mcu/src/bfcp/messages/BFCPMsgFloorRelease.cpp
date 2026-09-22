#include "bfcp/messages/BFCPMsgFloorRelease.h"
#include "log.h"


BFCPMsgFloorRelease::BFCPMsgFloorRelease(int transactionId, int conferenceId, int userId) :
		BFCPMessage(BFCPMessage::FloorRelease, transactionId, conferenceId, userId),
		floorRequestId(NULL)
{
}


BFCPMsgFloorRelease::~BFCPMsgFloorRelease()
{
	if (this->floorRequestId)
		delete this->floorRequestId;
}


bool BFCPMsgFloorRelease::IsValid()
{
	if (! this->floorRequestId) {
		::Error("BFCPMsgFloorRelease::IsValid() | MUST have a FloorRequestId\n");
		return false;
	}
	return true;
}


void BFCPMsgFloorRelease::Dump()
{
	::Debug("[BFCPMsgFloorRelease]\n");
	::Debug("- [primitive: FloorRelease, conferenceId: %d, userId: %d, transactionId: %d]\n", this->conferenceId, this->userId, this->transactionId);
	if (this->floorRequestId)
		::Debug("- floorRequestId: %d\n", this->floorRequestId->GetValue());
	::Debug("[/BFCPMsgFloorRelease]\n");
}


void BFCPMsgFloorRelease::SetFloorRequestId(int floorRequestId)
{
	if (this->floorRequestId)
		delete this->floorRequestId;
	this->floorRequestId = new BFCPAttrFloorRequestId(floorRequestId);
}


bool BFCPMsgFloorRelease::HasFloorRequestId() const
{
	return this->floorRequestId != NULL;
}


int BFCPMsgFloorRelease::GetFloorRequestId() const
{
	if (! this->floorRequestId)
		throw BFCPMessage::AttributeNotFound("'floorRequestId' attribute not found");
	return this->floorRequestId->GetValue();
}


size_t BFCPMsgFloorRelease::SerializeAttributes(BYTE* out, size_t max) const
{
	if (! this->floorRequestId)
		return Failed;
	return this->floorRequestId->Serialize(out, max, true);
}


bool BFCPMsgFloorRelease::ParseAttributes(const BYTE* data, size_t size)
{
	BFCPAttrCursor cursor(data, size);

	while (cursor.Next()) {
		switch (cursor.Type()) {
			case BFCPAttribute::FloorRequestId:
			{
				WORD floorRequestId;
				if (! BFCPAttribute::ReadWord(cursor.Contents(), cursor.ContentsLen(), floorRequestId))
					return false;
				SetFloorRequestId(floorRequestId);
				break;
			}
			default:
				if (cursor.IsMandatory())
					return ::Error("BFCPMsgFloorRelease::ParseAttributes() | unknown mandatory attribute %d\n", cursor.Type());
				break;
		}
	}

	return ! cursor.Malformed();
}
