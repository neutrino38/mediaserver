#ifndef BFCPATTRERRORINFO_H
#define	BFCPATTRERRORINFO_H


#include "bfcp/BFCPAttribute.h"
#include <string>


class BFCPAttrErrorInfo : public BFCPAttribute
{
public:
	BFCPAttrErrorInfo(const std::string& value);
	void Dump();
	const std::string& GetValue() const;

private:
	std::string value;
};


#endif
