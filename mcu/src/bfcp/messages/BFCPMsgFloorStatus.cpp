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


size_t BFCPMsgFloorStatus::SerializeAttributes(BYTE* out, size_t max) const
{
	size_t n = 0;

	// Everything here is optional: an empty FloorStatus answers a FloorQuery
	// that named no floor.
	if (this->floorId) {
		const size_t written = this->floorId->Serialize(out + n, max - n, false);
		if (written == Failed)
			return Failed;
		n += written;
	}
	for (size_t i=0; i < this->floorRequestInformations.size(); i++) {
		const size_t written = this->floorRequestInformations[i]->Serialize(out + n, max - n, false);
		if (written == Failed)
			return Failed;
		n += written;
	}

	return n;
}


bool BFCPMsgFloorStatus::ParseAttributes(const BYTE* data, size_t size)
{
	BFCPAttrCursor cursor(data, size);

	while (cursor.Next()) {
		switch (cursor.Type()) {
			case BFCPAttribute::FloorId:
			{
				WORD floorId;
				if (! BFCPAttribute::ReadWord(cursor.Contents(), cursor.ContentsLen(), floorId))
					return false;
				SetFloorId(floorId);
				break;
			}
			case BFCPAttribute::FloorRequestInformation:
			{
				BFCPAttrFloorRequestInformation* info = BFCPAttrFloorRequestInformation::Parse(cursor.Contents(), cursor.ContentsLen());
				if (! info)
					return false;
				AddFloorRequestInformation(info);
				break;
			}
			default:
				if (cursor.IsMandatory())
					return ::Error("BFCPMsgFloorStatus::ParseAttributes() | unknown mandatory attribute %d\n", cursor.Type());
				break;
		}
	}

	return ! cursor.Malformed();
}
