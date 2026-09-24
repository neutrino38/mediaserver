#include "bfcp/attributes/BFCPAttrSupportedAttributes.h"
#include "log.h"
#include <algorithm>


BFCPAttrSupportedAttributes::BFCPAttrSupportedAttributes()
{
}


void BFCPAttrSupportedAttributes::Dump()
{
	::Debug("[BFCPAttrSupportedAttributes]\n");
	for (size_t i=0; i < this->codes.size(); i++)
		::Debug("- code: %d\n", this->codes[i]);
	::Debug("[/BFCPAttrSupportedAttributes]\n");
}


void BFCPAttrSupportedAttributes::Add(int code)
{
	if (Has(code))
		return;
	this->codes.push_back(code);
}


int BFCPAttrSupportedAttributes::Count() const
{
	return this->codes.size();
}


int BFCPAttrSupportedAttributes::Get(unsigned int index) const
{
	if (index >= this->codes.size())
		return 0;
	return this->codes[index];
}


bool BFCPAttrSupportedAttributes::Has(int code) const
{
	return std::find(this->codes.begin(), this->codes.end(), code) != this->codes.end();
}


size_t BFCPAttrSupportedAttributes::Serialize(BYTE* out, size_t max, bool mandatory) const
{
	// An attribute type is 7 bits followed by a reserved bit, so it is shifted
	// exactly as in an attribute header. SUPPORTED-PRIMITIVES does not shift.
	BYTE contents[BFCPAttribute::MaxLength];
	size_t n = 0;
	for (size_t i=0; i < this->codes.size() && n < sizeof(contents); i++)
		contents[n++] = (BYTE)(this->codes[i] << 1);
	return Write(out, max, BFCPAttribute::SupportedAttributes, mandatory, contents, n);
}


BFCPAttrSupportedAttributes* BFCPAttrSupportedAttributes::Parse(const BYTE* contents, size_t len)
{
	BFCPAttrSupportedAttributes* attr = new BFCPAttrSupportedAttributes();
	for (size_t i=0; i < len; i++)
		attr->Add(contents[i] >> 1);
	return attr;
}
