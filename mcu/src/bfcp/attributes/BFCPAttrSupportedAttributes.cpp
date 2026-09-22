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
