#ifndef BFCPATTRPARTICIPANTPROVIDEDINFO_H
#define	BFCPATTRPARTICIPANTPROVIDEDINFO_H


#include "bfcp/BFCPAttribute.h"
#include <string>


class BFCPAttrParticipantProvidedInfo : public BFCPAttribute
{
public:
	BFCPAttrParticipantProvidedInfo(const std::string& value);
	void Dump();
	const std::string& GetValue() const;

private:
	std::string value;
};


#endif
