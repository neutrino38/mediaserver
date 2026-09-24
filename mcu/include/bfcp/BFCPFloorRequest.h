#ifndef BFCPFLOORREQUEST_H
#define BFCPFLOORREQUEST_H


#include "bfcp/messages/BFCPMsgFloorRequestStatus.h"
#include <set>


class BFCPFloorRequest
{
public:
	BFCPFloorRequest(int floorRequestId, int userId, int conferenceId);

	void SetBeneficiaryId(int beneficiaryId);
	void AddFloorId(int floorId);
	void SetStatus(enum BFCPAttrRequestStatus::Status status);
	void SetQueuePosition(int queuePosition);
	bool HasFloorId(int floorId) const;
	int GetFloorRequestId() const;
	int GetUserId() const;
	int GetBeneficiaryId() const;
	std::set<int> GetFloorIds() const;
	enum BFCPAttrRequestStatus::Status GetStatus() const;
	const char* GetStatusName() const;
	bool IsGranted() const;
	bool CanBeGranted() const;
	bool CanBeDenied() const;
	bool CanBeCancelled() const;
	int GetQueuePosition() const;
	void Dump();

	// Generates a FloorRequestStatus (transactionId = 0 for a notification).
	BFCPMsgFloorRequestStatus* CreateFloorRequestStatus(int transactionId) const;
	// Generates a FloorRequestInformation attribute.
	BFCPAttrFloorRequestInformation* CreateFloorRequestInformation() const;

private:
	int floorRequestId;
	int userId;
	int conferenceId;
	int beneficiaryId;
	std::set<int> floorIds;
	enum BFCPAttrRequestStatus::Status status;
	int queuePosition;
};


#endif
