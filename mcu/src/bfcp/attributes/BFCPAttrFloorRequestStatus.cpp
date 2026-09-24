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


size_t BFCPAttrFloorRequestStatus::Serialize(BYTE* out, size_t max, bool mandatory) const
{
	BYTE contents[BFCPAttribute::MaxLength];
	set2(contents, 0, this->floorId->GetValue());
	size_t n = 2;

	if (this->requestStatus) {
		const size_t written = this->requestStatus->Serialize(contents + n, sizeof(contents) - n, false);
		if (written == Failed)
			return Failed;
		n += written;
	}

	return Write(out, max, BFCPAttribute::FloorRequestStatus, mandatory, contents, n);
}


BFCPAttrFloorRequestStatus* BFCPAttrFloorRequestStatus::Parse(const BYTE* contents, size_t len)
{
	WORD floorId;
	if (! ReadWord(contents, len, floorId))
		return NULL;

	BFCPAttrFloorRequestStatus* attr = new BFCPAttrFloorRequestStatus(floorId);
	BFCPAttrCursor cursor(contents + 2, len - 2);

	while (cursor.Next()) {
		switch (cursor.Type()) {
			case BFCPAttribute::RequestStatus:
			{
				BFCPAttrRequestStatus* status = BFCPAttrRequestStatus::Parse(cursor.Contents(), cursor.ContentsLen());
				if (! status) {
					delete attr;
					return NULL;
				}
				attr->SetRequestStatus(status);
				break;
			}
			default:
				if (cursor.IsMandatory()) {
					::Error("BFCPAttrFloorRequestStatus::Parse() | unknown mandatory attribute %d\n", cursor.Type());
					delete attr;
					return NULL;
				}
				break;
		}
	}

	if (cursor.Malformed()) {
		delete attr;
		return NULL;
	}
	return attr;
}
