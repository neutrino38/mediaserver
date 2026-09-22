#include "bfcp/attributes/BFCPAttrFloorRequestId.h"
#include "log.h"


/* Instance methods */

BFCPAttrFloorRequestId::BFCPAttrFloorRequestId(int value) : value(value)
{
}


void BFCPAttrFloorRequestId::Dump()
{
	::Debug("[BFCPAttrFloorRequestId]\n");
	::Debug("- value: %d\n", this->value);
	::Debug("[/BFCPAttrFloorRequestId]\n");
}


int BFCPAttrFloorRequestId::GetValue() const
{
	return this->value;
}


size_t BFCPAttrFloorRequestId::Serialize(BYTE* out, size_t max, bool mandatory) const
{
	BYTE contents[2];
	set2(contents, 0, this->value);
	return Write(out, max, BFCPAttribute::FloorRequestId, mandatory, contents, sizeof(contents));
}


BFCPAttrFloorRequestId* BFCPAttrFloorRequestId::Parse(const BYTE* contents, size_t len)
{
	WORD value;
	if (! ReadWord(contents, len, value))
		return NULL;
	return new BFCPAttrFloorRequestId(value);
}
