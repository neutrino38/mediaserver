#ifndef BFCPMSGFLOORREQUEST_H
#define	BFCPMSGFLOORREQUEST_H


#include "bfcp/BFCPMessage.h"
#include "bfcp/attributes.h"
#include <vector>
#include <string>


// 5.3.1.  FloorRequest
//
//    FloorRequest =   (COMMON-HEADER)
//                   1*(FLOOR-ID)
//                     [BENEFICIARY-ID]
//                     [PARTICIPANT-PROVIDED-INFO]
//                     [PRIORITY]
//                    *[EXTENSION-ATTRIBUTE]


class BFCPMsgFloorRequest : public BFCPMessage
{
public:
	BFCPMsgFloorRequest(int transactionId, int conferenceId, int userId);
	~BFCPMsgFloorRequest();
	bool IsValid();
	void Dump();

protected:
	size_t SerializeAttributes(BYTE* out, size_t max) const;
	bool ParseAttributes(const BYTE* data, size_t size);

public:

	void AddFloorId(int);
	void SetBeneficiaryId(int);
	void SetParticipantProvidedInfo(const std::string&);
	bool HasBeneficiaryId() const;
	bool HasParticipantProvidedInfo() const;
	int GetFloorId(unsigned int) const;
	int CountFloorIds() const;
	int GetBeneficiaryId() const;
	const std::string& GetParticipantProvidedInfo() const;

private:
	// Mandatory attributes.
	std::vector<BFCPAttrFloorId *> floorIds;
	// Optional attributes.
	BFCPAttrBeneficiaryId *beneficiaryId;
	BFCPAttrParticipantProvidedInfo *participantProvidedInfo;
};


#endif  /* BFCPMSGFLOORREQUEST_H */
