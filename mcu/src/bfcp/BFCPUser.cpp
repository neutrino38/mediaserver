#include "bfcp/BFCPUser.h"
#include "log.h"
#include <algorithm>


BFCPUser::BFCPUser(int userId, int conferenceId) :
	userId(userId),
	conferenceId(conferenceId),
	isChair(false),
	transport(NULL)
{
}


int BFCPUser::GetUserId() const
{
	return userId;
}


void BFCPUser::SetChair()
{
	isChair = true;
}


void BFCPUser::UnsetChair()
{
	isChair = false;
}


bool BFCPUser::IsChair() const
{
	return isChair;
}


void BFCPUser::SetTransport(BFCPTransport *transport)
{
	this->transport = transport;
}


void BFCPUser::UnsetTransport()
{
	transport = NULL;
}


void BFCPUser::CloseTransport()
{
	if (transport)
		transport->Close();
	transport = NULL;
}


BFCPTransport* BFCPUser::GetTransport() const
{
	return transport;
}


bool BFCPUser::IsConnected() const
{
	return transport != NULL;
}


bool BFCPUser::IsReliable() const
{
	return transport ? transport->IsReliable() : true;
}


bool BFCPUser::SendMessage(const BFCPMessage& msg)
{
	if (! transport) {
		::Debug("BFCPUser::SendMessage() | user '%d' not connected, cannot send %s\n", userId, BFCPMessage::PrimitiveName(msg.GetPrimitive()));
		return false;
	}
	return transport->Send(msg);
}


void BFCPUser::ResetQueriedFloorIds()
{
	queriedFloorIds.clear();
}


void BFCPUser::AddQueriedFloorId(int floorId)
{
	if (HasQueriedFloorId(floorId))
		return;
	queriedFloorIds.push_back(floorId);
}


int BFCPUser::CountQueriedFloorIds() const
{
	return queriedFloorIds.size();
}


bool BFCPUser::HasQueriedFloorId(int floorId) const
{
	return std::find(queriedFloorIds.begin(), queriedFloorIds.end(), floorId) != queriedFloorIds.end();
}


int BFCPUser::GetQueriedFloorId(unsigned int index) const
{
	if (index >= queriedFloorIds.size())
		throw BFCPMessage::AttributeNotFound("'queriedFloorId' not in range");
	return queriedFloorIds[index];
}


void BFCPUser::Dump()
{
	::Debug("[BFCPUser]\n");
	::Debug("- userId: %d\n", userId);
	::Debug("- conferenceId: %d\n", conferenceId);
	::Debug("- chair: %s\n", isChair ? "yes" : "no");
	::Debug("- %s\n", transport ? "connected" : "not connected");
	for (size_t i=0; i<queriedFloorIds.size(); i++)
		::Debug("- queriedFloorId: %d\n", queriedFloorIds[i]);
	::Debug("[/BFCPUser]\n");
}
