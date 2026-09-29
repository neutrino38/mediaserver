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


size_t BFCPAttrRequestedByInformation::Serialize(BYTE* out, size_t max, bool mandatory) const
{
	BYTE contents[2];
	set2(contents, 0, this->requestedById);
	return Write(out, max, BFCPAttribute::RequestedByInformation, mandatory, contents, sizeof(contents));
}


BFCPAttrRequestedByInformation* BFCPAttrRequestedByInformation::Parse(const BYTE* contents, size_t len)
{
	WORD requestedById;
	if (! ReadWord(contents, len, requestedById))
		return NULL;
	BFCPAttrCursor cursor(contents + 2, len - 2);
	while (cursor.Next())
		;
	if (cursor.Malformed())
		return NULL;
	return new BFCPAttrRequestedByInformation(requestedById);
}
