#ifndef BFCPMSGFLOORQUERY_H
#define	BFCPMSGFLOORQUERY_H


#include "bfcp/BFCPMessage.h"
#include "bfcp/attributes.h"
#include <vector>


// 5.3.7.  FloorQuery
//
//    FloorQuery =   (COMMON-HEADER)
//                  *(FLOOR-ID)
//                  *[EXTENSION-ATTRIBUTE]


class BFCPMsgFloorQuery : public BFCPMessage
{
public:
	BFCPMsgFloorQuery(int transactionId, int conferenceId, int userId);
	~BFCPMsgFloorQuery();
	bool IsValid();
	void Dump();

	void AddFloorId(int);
	int GetFloorId(unsigned int) const;
	int CountFloorIds() const;

private:
	std::vector<BFCPAttrFloorId *> floorIds;
};


#endif  /* BFCPMSGFLOORQUERY_H */
