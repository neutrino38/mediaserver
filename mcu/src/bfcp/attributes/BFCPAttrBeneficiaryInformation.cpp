#include "bfcp/attributes/BFCPAttrBeneficiaryInformation.h"
#include "log.h"


BFCPAttrBeneficiaryInformation::BFCPAttrBeneficiaryInformation(int beneficiaryId) :
	beneficiaryId(new BFCPAttrBeneficiaryId(beneficiaryId))
{
}


BFCPAttrBeneficiaryInformation::~BFCPAttrBeneficiaryInformation()
{
	delete this->beneficiaryId;
}


void BFCPAttrBeneficiaryInformation::Dump()
{
	::Debug("[BFCPAttrBeneficiaryInformation]\n");
	::Debug("- beneficiaryId: %d\n", this->beneficiaryId->GetValue());
	::Debug("[/BFCPAttrBeneficiaryInformation]\n");
}


int BFCPAttrBeneficiaryInformation::GetBeneficiaryId() const
{
	return this->beneficiaryId->GetValue();
}


size_t BFCPAttrBeneficiaryInformation::Serialize(BYTE* out, size_t max, bool mandatory) const
{
	// A grouped attribute carries its id in the header, then its children.
	// USER-DISPLAY-NAME and USER-URI are optional and we hold neither.
	BYTE contents[2];
	set2(contents, 0, this->beneficiaryId->GetValue());
	return Write(out, max, BFCPAttribute::BeneficiaryInformation, mandatory, contents, sizeof(contents));
}


BFCPAttrBeneficiaryInformation* BFCPAttrBeneficiaryInformation::Parse(const BYTE* contents, size_t len)
{
	WORD beneficiaryId;
	if (! ReadWord(contents, len, beneficiaryId))
		return NULL;
	// The children we do not keep still have to walk cleanly, or the whole
	// attribute is malformed.
	BFCPAttrCursor cursor(contents + 2, len - 2);
	while (cursor.Next())
		;
	if (cursor.Malformed())
		return NULL;
	return new BFCPAttrBeneficiaryInformation(beneficiaryId);
}
