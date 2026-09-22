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
