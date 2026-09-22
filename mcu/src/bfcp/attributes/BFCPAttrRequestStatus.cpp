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
