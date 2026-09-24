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


size_t BFCPAttrErrorInfo::Serialize(BYTE* out, size_t max, bool mandatory) const
{
	return Write(out, max, BFCPAttribute::ErrorInfo, mandatory, (const BYTE*)this->value.data(), this->value.size());
}


BFCPAttrErrorInfo* BFCPAttrErrorInfo::Parse(const BYTE* contents, size_t len)
{
	return new BFCPAttrErrorInfo(std::string((const char*)contents, len));
}
