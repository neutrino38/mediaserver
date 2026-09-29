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


size_t BFCPAttrOverallRequestStatus::Serialize(BYTE* out, size_t max, bool mandatory) const
{
	BYTE contents[BFCPAttribute::MaxLength];
	set2(contents, 0, this->floorRequestId->GetValue());
	size_t n = 2;

	if (this->requestStatus) {
		const size_t written = this->requestStatus->Serialize(contents + n, sizeof(contents) - n, false);
		if (written == Failed)
			return Failed;
		n += written;
	}
	if (this->statusInfo) {
		const size_t written = this->statusInfo->Serialize(contents + n, sizeof(contents) - n, false);
		if (written == Failed)
			return Failed;
		n += written;
	}

	return Write(out, max, BFCPAttribute::OverallRequestStatus, mandatory, contents, n);
}


BFCPAttrOverallRequestStatus* BFCPAttrOverallRequestStatus::Parse(const BYTE* contents, size_t len)
{
	WORD floorRequestId;
	if (! ReadWord(contents, len, floorRequestId))
		return NULL;

	BFCPAttrOverallRequestStatus* attr = new BFCPAttrOverallRequestStatus(floorRequestId);
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
			case BFCPAttribute::StatusInfo:
				attr->SetStatusInfo(BFCPAttrStatusInfo::Parse(cursor.Contents(), cursor.ContentsLen()));
				break;
			default:
				if (cursor.IsMandatory()) {
					::Error("BFCPAttrOverallRequestStatus::Parse() | unknown mandatory attribute %d\n", cursor.Type());
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
