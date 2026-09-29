#ifndef BFCPATTRSUPPORTEDATTRIBUTES_H
#define	BFCPATTRSUPPORTEDATTRIBUTES_H


#include "bfcp/BFCPAttribute.h"
#include <vector>


// 5.2.10.  SUPPORTEDATTRIBUTES
//
//       0                   1                   2                   3
//       0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//      |0 0 0 1 0 1 0|M|    Length     | Code  |R|  Code |R|  Code  |R| ...
//      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+


class BFCPAttrSupportedAttributes : public BFCPAttribute
{
public:
	BFCPAttrSupportedAttributes();
	void Dump();
	size_t Serialize(BYTE* out, size_t max, bool mandatory) const;
	static BFCPAttrSupportedAttributes* Parse(const BYTE* contents, size_t len);

	void Add(int code);
	int Count() const;
	int Get(unsigned int index) const;
	bool Has(int code) const;

private:
	std::vector<int> codes;
};


#endif
