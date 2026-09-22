#ifndef BFCPATTRSTATUSINFO_H
#define	BFCPATTRSTATUSINFO_H


#include "bfcp/BFCPAttribute.h"
#include <string>


class BFCPAttrStatusInfo : public BFCPAttribute
{
public:
	BFCPAttrStatusInfo(const std::string& value);
	void Dump();
	const std::string& GetValue() const;

private:
	std::string value;
};


#endif
