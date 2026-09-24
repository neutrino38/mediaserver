#include "bfcp/attributes/BFCPAttrParticipantProvidedInfo.h"
#include "log.h"


BFCPAttrParticipantProvidedInfo::BFCPAttrParticipantProvidedInfo(const std::string& value) : value(value)
{
}


void BFCPAttrParticipantProvidedInfo::Dump()
{
	::Debug("[BFCPAttrParticipantProvidedInfo]\n");
	::Debug("- value: %s\n", this->value.c_str());
	::Debug("[/BFCPAttrParticipantProvidedInfo]\n");
}


const std::string& BFCPAttrParticipantProvidedInfo::GetValue() const
{
	return this->value;
}


size_t BFCPAttrParticipantProvidedInfo::Serialize(BYTE* out, size_t max, bool mandatory) const
{
	return Write(out, max, BFCPAttribute::ParticipantProvidedInfo, mandatory, (const BYTE*)this->value.data(), this->value.size());
}


BFCPAttrParticipantProvidedInfo* BFCPAttrParticipantProvidedInfo::Parse(const BYTE* contents, size_t len)
{
	return new BFCPAttrParticipantProvidedInfo(std::string((const char*)contents, len));
}
