#ifndef BFCPMSGFLOORSTATUS_H
#define	BFCPMSGFLOORSTATUS_H


#include "bfcp/BFCPMessage.h"
#include "bfcp/attributes.h"
#include <vector>


// 5.3.8.  FloorStatus
//
//    FloorStatus =   (COMMON-HEADER)
//                    [FLOOR-ID]
//                   *[FLOOR-REQUEST-INFORMATION]
//                   *[EXTENSION-ATTRIBUTE]


class BFCPMsgFloorStatus :  public BFCPMessage
{
public:
	BFCPMsgFloorStatus(int transactionId, int conferenceId, int userId);
	~BFCPMsgFloorStatus();
	void Dump();

protected:
	size_t SerializeAttributes(BYTE* out, size_t max) const;
	bool ParseAttributes(const BYTE* data, size_t size);

public:

	void SetFloorId(int);
	void AddFloorRequestInformation(BFCPAttrFloorRequestInformation* floorRequestInformation);
	bool HasFloorId() const;
	int GetFloorId() const;
	int CountFloorRequestInformations() const;
	const BFCPAttrFloorRequestInformation* GetFloorRequestInformation(unsigned int index) const;

private:
	// Optional attributes.
	BFCPAttrFloorId* floorId;
	std::vector<BFCPAttrFloorRequestInformation*> floorRequestInformations;
};


#endif
