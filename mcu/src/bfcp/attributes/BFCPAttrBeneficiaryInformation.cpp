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
