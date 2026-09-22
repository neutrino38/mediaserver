#ifndef BFCPATTRSUPPORTEDPRIMITIVES_H
#define	BFCPATTRSUPPORTEDPRIMITIVES_H


#include "bfcp/BFCPAttribute.h"
#include <vector>


// 5.2.11.  SUPPORTEDPRIMITIVES
//
//       0                   1                   2                   3
//       0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//      |0 0 0 1 0 1 1|M|    Length     | Code  |R|  Code |R|  Code  |R| ...
//      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+


class BFCPAttrSupportedPrimitives : public BFCPAttribute
{
public:
	BFCPAttrSupportedPrimitives();
	void Dump();

	void Add(int code);
	int Count() const;
	int Get(unsigned int index) const;
	bool Has(int code) const;

private:
	std::vector<int> codes;
};


#endif
