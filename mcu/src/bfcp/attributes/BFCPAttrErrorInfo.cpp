#include "bfcp/attributes/BFCPAttrErrorInfo.h"
#include "log.h"


BFCPAttrErrorInfo::BFCPAttrErrorInfo(const std::string& value) : value(value)
{
}


void BFCPAttrErrorInfo::Dump()
{
	::Debug("[BFCPAttrErrorInfo]\n");
	::Debug("- value: %s\n", this->value.c_str());
	::Debug("[/BFCPAttrErrorInfo]\n");
}


const std::string& BFCPAttrErrorInfo::GetValue() const
{
	return this->value;
}
