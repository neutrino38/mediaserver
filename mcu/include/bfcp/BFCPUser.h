#ifndef BFCPUSER_H
#define BFCPUSER_H


#include "bfcp/BFCPMessage.h"
#include "bfcp/BFCPTransport.h"
#include <vector>


/**
 * One user of a BFCP conference. All access is serialised by the lock of the
 * owning BFCPFloorControlServer.
 */
class BFCPUser
{
public:
	BFCPUser(int userId, int conferenceId);

	int GetUserId() const;
	void SetChair();
	void UnsetChair();
	bool IsChair() const;

	void SetTransport(BFCPTransport *transport);
	void UnsetTransport();
	void CloseTransport();
	BFCPTransport* GetTransport() const;
	bool IsConnected() const;
	bool IsReliable() const;
	bool SendMessage(const BFCPMessage& msg);

	void ResetQueriedFloorIds();
	void AddQueriedFloorId(int floorId);
	int CountQueriedFloorIds() const;
	bool HasQueriedFloorId(int floorId) const;
	int GetQueriedFloorId(unsigned int index) const;

	void Dump();

private:
	int userId;
	int conferenceId;
	bool isChair;
	BFCPTransport *transport;
	std::vector<int> queriedFloorIds;
};


#endif
