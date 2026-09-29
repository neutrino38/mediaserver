#include "bfcp/attributes/BFCPAttrSupportedPrimitives.h"
#include "log.h"
#include <algorithm>


BFCPAttrSupportedPrimitives::BFCPAttrSupportedPrimitives()
{
}


void BFCPAttrSupportedPrimitives::Dump()
{
	::Debug("[BFCPAttrSupportedPrimitives]\n");
	for (size_t i=0; i < this->codes.size(); i++)
		::Debug("- code: %d\n", this->codes[i]);
	::Debug("[/BFCPAttrSupportedPrimitives]\n");
}


void BFCPAttrSupportedPrimitives::Add(int code)
{
	if (Has(code))
		return;
	this->codes.push_back(code);
}


int BFCPAttrSupportedPrimitives::Count() const
{
	return this->codes.size();
}


int BFCPAttrSupportedPrimitives::Get(unsigned int index) const
{
	if (index >= this->codes.size())
		return 0;
	return this->codes[index];
}


bool BFCPAttrSupportedPrimitives::Has(int code) const
{
	return std::find(this->codes.begin(), this->codes.end(), code) != this->codes.end();
}


size_t BFCPAttrSupportedPrimitives::Serialize(BYTE* out, size_t max, bool mandatory) const
{
	// A primitive is a plain octet here. SUPPORTED-ATTRIBUTES is NOT symmetric:
	// it shifts, because an attribute type is 7 bits.
	BYTE contents[BFCPAttribute::MaxLength];
	size_t n = 0;
	for (size_t i=0; i < this->codes.size() && n < sizeof(contents); i++)
		contents[n++] = (BYTE)this->codes[i];
	return Write(out, max, BFCPAttribute::SupportedPrimitives, mandatory, contents, n);
}


BFCPAttrSupportedPrimitives* BFCPAttrSupportedPrimitives::Parse(const BYTE* contents, size_t len)
{
	BFCPAttrSupportedPrimitives* attr = new BFCPAttrSupportedPrimitives();
	for (size_t i=0; i < len; i++)
		attr->Add(contents[i]);
	return attr;
}
