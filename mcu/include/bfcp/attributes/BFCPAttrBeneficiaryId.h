#ifndef BFCPATTRBENEFICIARYIDA_H
#define	BFCPATTRBENEFICIARYIDA_H


#include "bfcp/BFCPAttribute.h"


// 5.2.1.  BENEFICIARY-ID
//
//       0                   1                   2                   3
//       0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//      |0 0 0 0 0 0 1|M|0 0 0 0 0 1 0 0|          Value                |
//      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+


class BFCPAttrBeneficiaryId : public BFCPAttribute
{
public:
	BFCPAttrBeneficiaryId(int);
	void Dump();
	size_t Serialize(BYTE* out, size_t max, bool mandatory) const;
	static BFCPAttrBeneficiaryId* Parse(const BYTE* contents, size_t len);

	int GetValue() const;

private:
	int value;
};


#endif
