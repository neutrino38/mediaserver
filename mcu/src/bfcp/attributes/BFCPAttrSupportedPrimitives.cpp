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
