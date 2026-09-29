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


size_t BFCPMsgFloorQuery::SerializeAttributes(BYTE* out, size_t max) const
{
	size_t n = 0;

	// *(FLOOR-ID): zero or more, none of them mandatory. An empty FloorQuery
	// is how a subscriber unsubscribes.
	for (size_t i=0; i < this->floorIds.size(); i++) {
		const size_t written = this->floorIds[i]->Serialize(out + n, max - n, false);
		if (written == Failed)
			return Failed;
		n += written;
	}

	return n;
}


bool BFCPMsgFloorQuery::ParseAttributes(const BYTE* data, size_t size)
{
	BFCPAttrCursor cursor(data, size);

	while (cursor.Next()) {
		switch (cursor.Type()) {
			case BFCPAttribute::FloorId:
			{
				WORD floorId;
				if (! BFCPAttribute::ReadWord(cursor.Contents(), cursor.ContentsLen(), floorId))
					return false;
				AddFloorId(floorId);
				break;
			}
			default:
				if (cursor.IsMandatory())
					return ::Error("BFCPMsgFloorQuery::ParseAttributes() | unknown mandatory attribute %d\n", cursor.Type());
				break;
		}
	}

	return ! cursor.Malformed();
}
