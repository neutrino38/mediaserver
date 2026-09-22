#include "bfcp/attributes/BFCPAttrBeneficiaryId.h"
#include "log.h"


/* Instance methods */

BFCPAttrBeneficiaryId::BFCPAttrBeneficiaryId(int value) : value(value)
{
}


void BFCPAttrBeneficiaryId::Dump()
{
	::Debug("[BFCPAttrBeneficiaryId]\n");
	::Debug("- value: %d\n", this->value);
	::Debug("[/BFCPAttrBeneficiaryId]\n");
}


int BFCPAttrBeneficiaryId::GetValue() const
{
	return this->value;
}


size_t BFCPAttrBeneficiaryId::Serialize(BYTE* out, size_t max, bool mandatory) const
{
	BYTE contents[2];
	set2(contents, 0, this->value);
	return Write(out, max, BFCPAttribute::BeneficiaryId, mandatory, contents, sizeof(contents));
}


BFCPAttrBeneficiaryId* BFCPAttrBeneficiaryId::Parse(const BYTE* contents, size_t len)
{
	WORD value;
	if (! ReadWord(contents, len, value))
		return NULL;
	return new BFCPAttrBeneficiaryId(value);
}
