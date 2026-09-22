#include "bfcp/attributes/BFCPAttrRequestedByInformation.h"
#include "log.h"


BFCPAttrRequestedByInformation::BFCPAttrRequestedByInformation(int requestedById) :
	requestedById(requestedById)
{
}


void BFCPAttrRequestedByInformation::Dump()
{
	::Debug("[BFCPAttrRequestedByInformation]\n");
	::Debug("- requestedById: %d\n", this->requestedById);
	::Debug("[/BFCPAttrRequestedByInformation]\n");
}


int BFCPAttrRequestedByInformation::GetRequestedById() const
{
	return this->requestedById;
}
