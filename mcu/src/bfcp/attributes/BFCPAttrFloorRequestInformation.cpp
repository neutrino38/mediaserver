#include "bfcp/attributes/BFCPAttrFloorRequestInformation.h"
#include "log.h"
#include <algorithm>


BFCPAttrFloorRequestInformation::BFCPAttrFloorRequestInformation(int floorRequestId) :
	floorRequestId(new BFCPAttrFloorRequestId(floorRequestId)),
	overallRequestStatus(NULL),
	beneficiaryInformation(NULL),
	requestedByInformation(NULL)
{
}


BFCPAttrFloorRequestInformation::~BFCPAttrFloorRequestInformation()
{
	delete this->floorRequestId;
	for (size_t i=0; i < this->floorRequestStatuses.size(); i++)
		delete this->floorRequestStatuses[i];
	if (this->overallRequestStatus)
		delete this->overallRequestStatus;
	if (this->beneficiaryInformation)
		delete this->beneficiaryInformation;
	if (this->requestedByInformation)
		delete this->requestedByInformation;
}


void BFCPAttrFloorRequestInformation::Dump()
{
	::Debug("[BFCPAttrFloorRequestInformation]\n");
	::Debug("- floorRequestId: %d\n", this->floorRequestId->GetValue());
	for (size_t i=0; i < this->floorRequestStatuses.size(); i++) {
		::Debug("- floorRequestStatus:\n");
		this->floorRequestStatuses[i]->Dump();
	}
	if (this->overallRequestStatus) {
		::Debug("- overallRequestStatus:\n");
		this->overallRequestStatus->Dump();
	}
	if (this->beneficiaryInformation) {
		::Debug("- beneficiaryInformation:\n");
		this->beneficiaryInformation->Dump();
	}
	if (this->requestedByInformation) {
		::Debug("- requestedByInformation:\n");
		this->requestedByInformation->Dump();
	}
	::Debug("[/BFCPAttrFloorRequestInformation]\n");
}


void BFCPAttrFloorRequestInformation::AddFloorRequestStatus(BFCPAttrFloorRequestStatus *floorRequestStatus)
{
	if (std::find(this->floorRequestStatuses.begin(), this->floorRequestStatuses.end(), floorRequestStatus) != this->floorRequestStatuses.end())
		return;
	this->floorRequestStatuses.push_back(floorRequestStatus);
}


void BFCPAttrFloorRequestInformation::SetOverallRequestStatus(BFCPAttrOverallRequestStatus *overallRequestStatus)
{
	if (this->overallRequestStatus)
		delete this->overallRequestStatus;
	this->overallRequestStatus = overallRequestStatus;
}


void BFCPAttrFloorRequestInformation::SetBeneficiaryInformation(BFCPAttrBeneficiaryInformation *beneficiaryInformation)
{
	if (this->beneficiaryInformation)
		delete this->beneficiaryInformation;
	this->beneficiaryInformation = beneficiaryInformation;
}


void BFCPAttrFloorRequestInformation::SetRequestedByInformation(BFCPAttrRequestedByInformation *requestedByInformation)
{
	if (this->requestedByInformation)
		delete this->requestedByInformation;
	this->requestedByInformation = requestedByInformation;
}


void BFCPAttrFloorRequestInformation::SetDescription(const std::string& statusInfo)
{
	if (! this->overallRequestStatus)
		return;
	this->overallRequestStatus->SetStatusInfo(new BFCPAttrStatusInfo(statusInfo));
}


int BFCPAttrFloorRequestInformation::GetFloorRequestId() const
{
	return this->floorRequestId->GetValue();
}


int BFCPAttrFloorRequestInformation::CountFloorRequestStatuses() const
{
	return this->floorRequestStatuses.size();
}


const BFCPAttrFloorRequestStatus* BFCPAttrFloorRequestInformation::GetFloorRequestStatus(unsigned int index) const
{
	if (index >= this->floorRequestStatuses.size())
		return NULL;
	return this->floorRequestStatuses[index];
}


const BFCPAttrOverallRequestStatus* BFCPAttrFloorRequestInformation::GetOverallRequestStatus() const
{
	return this->overallRequestStatus;
}


const BFCPAttrBeneficiaryInformation* BFCPAttrFloorRequestInformation::GetBeneficiaryInformation() const
{
	return this->beneficiaryInformation;
}


const BFCPAttrRequestedByInformation* BFCPAttrFloorRequestInformation::GetRequestedByInformation() const
{
	return this->requestedByInformation;
}


size_t BFCPAttrFloorRequestInformation::Serialize(BYTE* out, size_t max, bool mandatory) const
{
	// Everything below has to fit in 255 octets: that is all the Length field
	// of a grouped attribute can say. Write() refuses beyond, it never trims.
	BYTE contents[BFCPAttribute::MaxLength];
	set2(contents, 0, this->floorRequestId->GetValue());
	size_t n = 2;

	if (this->overallRequestStatus) {
		const size_t written = this->overallRequestStatus->Serialize(contents + n, sizeof(contents) - n, false);
		if (written == Failed)
			return Failed;
		n += written;
	}

	// 1*(FLOOR-REQUEST-STATUS): mandatory by the grammar, hence the M bit.
	for (size_t i=0; i < this->floorRequestStatuses.size(); i++) {
		const size_t written = this->floorRequestStatuses[i]->Serialize(contents + n, sizeof(contents) - n, true);
		if (written == Failed)
			return Failed;
		n += written;
	}

	if (this->beneficiaryInformation) {
		const size_t written = this->beneficiaryInformation->Serialize(contents + n, sizeof(contents) - n, false);
		if (written == Failed)
			return Failed;
		n += written;
	}
	if (this->requestedByInformation) {
		const size_t written = this->requestedByInformation->Serialize(contents + n, sizeof(contents) - n, false);
		if (written == Failed)
			return Failed;
		n += written;
	}

	return Write(out, max, BFCPAttribute::FloorRequestInformation, mandatory, contents, n);
}


BFCPAttrFloorRequestInformation* BFCPAttrFloorRequestInformation::Parse(const BYTE* contents, size_t len)
{
	WORD floorRequestId;
	if (! ReadWord(contents, len, floorRequestId))
		return NULL;

	BFCPAttrFloorRequestInformation* attr = new BFCPAttrFloorRequestInformation(floorRequestId);
	BFCPAttrCursor cursor(contents + 2, len - 2);

	while (cursor.Next()) {
		switch (cursor.Type()) {
			case BFCPAttribute::OverallRequestStatus:
			{
				BFCPAttrOverallRequestStatus* overall = BFCPAttrOverallRequestStatus::Parse(cursor.Contents(), cursor.ContentsLen());
				if (! overall) {
					delete attr;
					return NULL;
				}
				attr->SetOverallRequestStatus(overall);
				break;
			}
			case BFCPAttribute::FloorRequestStatus:
			{
				BFCPAttrFloorRequestStatus* status = BFCPAttrFloorRequestStatus::Parse(cursor.Contents(), cursor.ContentsLen());
				if (! status) {
					delete attr;
					return NULL;
				}
				attr->AddFloorRequestStatus(status);
				break;
			}
			case BFCPAttribute::BeneficiaryInformation:
			{
				BFCPAttrBeneficiaryInformation* beneficiary = BFCPAttrBeneficiaryInformation::Parse(cursor.Contents(), cursor.ContentsLen());
				if (! beneficiary) {
					delete attr;
					return NULL;
				}
				attr->SetBeneficiaryInformation(beneficiary);
				break;
			}
			case BFCPAttribute::RequestedByInformation:
			{
				BFCPAttrRequestedByInformation* requestedBy = BFCPAttrRequestedByInformation::Parse(cursor.Contents(), cursor.ContentsLen());
				if (! requestedBy) {
					delete attr;
					return NULL;
				}
				attr->SetRequestedByInformation(requestedBy);
				break;
			}
			default:
				if (cursor.IsMandatory()) {
					::Error("BFCPAttrFloorRequestInformation::Parse() | unknown mandatory attribute %d\n", cursor.Type());
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
