#include "bfcp/attributes/BFCPAttrStatusInfo.h"
#include "log.h"


BFCPAttrStatusInfo::BFCPAttrStatusInfo(const std::string& value) : value(value)
{
}


void BFCPAttrStatusInfo::Dump()
{
	::Debug("[BFCPAttrStatusInfo]\n");
	::Debug("- value: %s\n", this->value.c_str());
	::Debug("[/BFCPAttrStatusInfo]\n");
}


const std::string& BFCPAttrStatusInfo::GetValue() const
{
	return this->value;
}
