#include "bfcp/attributes/BFCPAttrRequestStatus.h"
#include "log.h"


const char* BFCPAttrRequestStatus::StatusName(enum BFCPAttrRequestStatus::Status status)
{
	switch (status)
	{
		case Pending:	return "Pending";
		case Accepted:	return "Accepted";
		case Granted:	return "Granted";
		case Denied:	return "Denied";
		case Cancelled:	return "Cancelled";
		case Released:	return "Released";
		case Revoked:	return "Revoked";
	}
	return "Unknown";
}


BFCPAttrRequestStatus::BFCPAttrRequestStatus() :
	status(BFCPAttrRequestStatus::Pending),
	queuePosition(0)
{
}


BFCPAttrRequestStatus::BFCPAttrRequestStatus(enum BFCPAttrRequestStatus::Status status, int queuePosition) :
	status(status),
	queuePosition(queuePosition)
{
}


void BFCPAttrRequestStatus::Dump()
{
	::Debug("[BFCPAttrRequestStatus]\n");
	::Debug("- status: %s\n", StatusName(this->status));
	::Debug("- queuePosition: %d\n", this->queuePosition);
	::Debug("[/BFCPAttrRequestStatus]\n");
}


void BFCPAttrRequestStatus::SetStatus(enum BFCPAttrRequestStatus::Status status)
{
	this->status = status;
}


void BFCPAttrRequestStatus::SetQueuePosition(int queuePosition)
{
	this->queuePosition = queuePosition;
}


enum BFCPAttrRequestStatus::Status BFCPAttrRequestStatus::GetStatus() const
{
	return this->status;
}


int BFCPAttrRequestStatus::GetQueuePosition() const
{
	return this->queuePosition;
}


size_t BFCPAttrRequestStatus::Serialize(BYTE* out, size_t max, bool mandatory) const
{
	BYTE contents[2];
	contents[0] = (BYTE)this->status;
	contents[1] = (BYTE)this->queuePosition;
	return Write(out, max, BFCPAttribute::RequestStatus, mandatory, contents, sizeof(contents));
}


BFCPAttrRequestStatus* BFCPAttrRequestStatus::Parse(const BYTE* contents, size_t len)
{
	if (len < 2)
		return NULL;
	// Any status outside the enum is refused here rather than carried around
	// as an int nobody can name.
	if (contents[0] < Pending || contents[0] > Revoked) {
		::Error("BFCPAttrRequestStatus::Parse() | unknown request status %d\n", contents[0]);
		return NULL;
	}
	return new BFCPAttrRequestStatus((enum Status)contents[0], contents[1]);
}
