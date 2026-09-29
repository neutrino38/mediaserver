#ifndef BFCPATTRFLOORID_H
#define	BFCPATTRFLOORID_H


#include "bfcp/BFCPAttribute.h"


// 5.2.2.  FLOOR-ID
//
//       0                   1                   2                   3
//       0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//      |0 0 0 0 0 1 0|M|0 0 0 0 0 1 0 0|          Value                |
//      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+


class BFCPAttrFloorId : public BFCPAttribute
{
public:
	BFCPAttrFloorId(int);
	void Dump();
	size_t Serialize(BYTE* out, size_t max, bool mandatory) const;
	static BFCPAttrFloorId* Parse(const BYTE* contents, size_t len);

	int GetValue() const;

private:
	int value;
};


#endif
