#ifndef BFCPATTRFLOORREQUESTSTATUS
#define	BFCPATTRFLOORREQUESTSTATUS


#include "bfcp/BFCPAttribute.h"
#include "bfcp/attributes/BFCPAttrFloorId.h"
#include "bfcp/attributes/BFCPAttrRequestStatus.h"


// 5.2.17.  FLOOR-REQUEST-STATUS
//
//       0                   1                   2                   3
//       0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//      |0 0 1 0 0 0 1|M|    Length     |           Floor ID            |
//      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
//    FLOOR-REQUEST-STATUS =   (FLOOR-REQUEST-STATUS-HEADER)
//                             [REQUEST-STATUS]
//                             [STATUS-INFO]
//                            *[EXTENSION-ATTRIBUTE]


class BFCPAttrFloorRequestStatus : public BFCPAttribute
{
public:
	BFCPAttrFloorRequestStatus(int floorId);
	~BFCPAttrFloorRequestStatus();
	void Dump();

	void SetRequestStatus(BFCPAttrRequestStatus *requestStatus);
	int GetFloorId() const;
	const BFCPAttrRequestStatus* GetRequestStatus() const;

private:
	BFCPAttrFloorId *floorId;
	BFCPAttrRequestStatus *requestStatus;
};


#endif
