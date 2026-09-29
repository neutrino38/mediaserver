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


size_t BFCPAttrStatusInfo::Serialize(BYTE* out, size_t max, bool mandatory) const
{
	return Write(out, max, BFCPAttribute::StatusInfo, mandatory, (const BYTE*)this->value.data(), this->value.size());
}


BFCPAttrStatusInfo* BFCPAttrStatusInfo::Parse(const BYTE* contents, size_t len)
{
	return new BFCPAttrStatusInfo(std::string((const char*)contents, len));
}
