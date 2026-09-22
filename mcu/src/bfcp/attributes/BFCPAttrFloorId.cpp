#include "bfcp/attributes/BFCPAttrFloorId.h"
#include "log.h"


/* Instance methods */

BFCPAttrFloorId::BFCPAttrFloorId(int value) : value(value)
{
}


void BFCPAttrFloorId::Dump()
{
	::Debug("[BFCPAttrFloorId]\n");
	::Debug("- value: %d\n", this->value);
	::Debug("[/BFCPAttrFloorId]\n");
}


int BFCPAttrFloorId::GetValue() const
{
	return this->value;
}


size_t BFCPAttrFloorId::Serialize(BYTE* out, size_t max, bool mandatory) const
{
	BYTE contents[2];
	set2(contents, 0, this->value);
	return Write(out, max, BFCPAttribute::FloorId, mandatory, contents, sizeof(contents));
}


BFCPAttrFloorId* BFCPAttrFloorId::Parse(const BYTE* contents, size_t len)
{
	WORD value;
	if (! ReadWord(contents, len, value))
		return NULL;
	return new BFCPAttrFloorId(value);
}
