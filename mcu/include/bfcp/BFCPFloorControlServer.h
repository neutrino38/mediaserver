#ifndef BFCPFLOORCONTROLSERVER_H
#define BFCPFLOORCONTROLSERVER_H


#include "bfcp/BFCPMessage.h"
#include "bfcp/messages.h"
#include "bfcp/attributes.h"
#include "bfcp/BFCPTransport.h"
#include "bfcp/BFCPUser.h"
#include "bfcp/BFCPFloorRequest.h"
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>


/**
 * A BFCPFloorControlServer handles a BFCP conference.
 *
 * Two threads call it: the transport (MessageReceived, UserDisconnected) and
 * the chair (Grant, Deny, Revoke, AddUser...). One lock serialises them. The
 * lock is never held while the Listener runs, so a listener may call back
 * into the server from inside a notification.
 */
class BFCPFloorControlServer
{
public:
	/**
	 * The chair of the conference (the media conference inserts itself here).
	 */
	class Listener
	{
	public:
		virtual ~Listener() {}

		/**
		 * A FloorRequest was received. The chair MUST later call
		 * GrantFloorRequest() or DenyFloorRequest() with the same floorRequestId.
		 */
		virtual void onFloorRequest(int floorRequestId, int userId, int beneficiaryId, std::set<int> floorIds) = 0;

		/**
		 * A FloorRequest has been granted (after GrantFloorRequest()).
		 */
		virtual void onFloorGranted(int floorRequestId, int beneficiaryId, std::set<int> floorIds) = 0;

		/**
		 * A granted FloorRequest is no longer held: released by its owner,
		 * revoked by the chair, or lost with the user (removed, disconnected,
		 * Goodbye).
		 */
		virtual void onFloorReleased(int floorRequestId, int beneficiaryId, std::set<int> floorIds) = 0;

		/**
		 * A user has completed its Hello (HelloAck sent).
		 */
		virtual void onUserConnected(int userId) = 0;
	};

public:
	BFCPFloorControlServer(int conferenceId, Listener *listener);
	~BFCPFloorControlServer();

	int GetConferenceId() const;

	// Called by the chair.
	bool AddUser(int userId);
	bool RemoveUser(int userId);
	bool SetChair(int userId);
	bool AddFloor(int floorId);
	bool GrantFloorRequest(int floorRequestId);
	bool DenyFloorRequest(int floorRequestId, const std::string& statusInfo = "");
	bool RevokeFloorRequest(int floorRequestId, const std::string& statusInfo = "");
	// Returns 0 if there is no Granted FloorRequest owning the given floor.
	int GetGrantedFloorRequestId(int floorId);
	// Sends the state of a floor to one user, unsolicited.
	bool NotifyFloorStatus(int userId, int floorId);
	void End();

	// Called by the transport.
	bool UserConnected(int userId, BFCPTransport *transport);
	void UserDisconnected(int userId, BFCPTransport *transport);
	void MessageReceived(BFCPMessage *msg, BFCPTransport *from);

private:
	typedef std::vector<std::function<void()> > Notifications;

	BFCPUser* GetUserLocked(int userId);
	BFCPFloorRequest* GetFloorRequestLocked(int floorRequestId);
	std::vector<BFCPFloorRequest*> GetFloorRequestsForFloorLocked(int floorId);
	std::vector<BFCPFloorRequest*> GetFloorRequestsForFloorsLocked(const std::set<int>& floorIds);
	bool HasFloorLocked(int floorId) const;
	int NextTransactionIdLocked();
	int NotificationTransactionIdLocked(const BFCPUser* user);

	bool AttachTransportLocked(BFCPUser* user, BFCPTransport* transport, Notifications& pending);
	bool RemoveUserLocked(int userId, bool sendGoodbye, Notifications& pending);
	bool GrantFloorRequestLocked(int floorRequestId, Notifications& pending);
	bool DenyFloorRequestLocked(int floorRequestId, const std::string& statusInfo, Notifications& pending);
	bool RevokeFloorRequestLocked(int floorRequestId, const std::string& statusInfo, Notifications& pending);
	void RevokeUserFloorRequestsLocked(BFCPUser* user, Notifications& pending);
	void NotifyForFloorRequestLocked(const BFCPFloorRequest* floorRequest);
	std::unique_ptr<BFCPMsgFloorStatus> BuildFloorStatusLocked(int floorId, int transactionId, int userId);

	void ProcessFloorRequestLocked(BFCPMsgFloorRequest *req, BFCPUser* user, Notifications& pending);
	void ProcessFloorReleaseLocked(BFCPMsgFloorRelease *req, BFCPUser* user, Notifications& pending);
	void ProcessFloorQueryLocked(BFCPMsgFloorQuery *req, BFCPUser* user);
	void ProcessHelloLocked(BFCPMsgHello *req, BFCPUser* user, Notifications& pending);
	void ProcessGoodbyeLocked(BFCPMessage *req, BFCPUser* user, Notifications& pending);

	void SendFloorRequestStatusLocked(const BFCPFloorRequest* floorRequest, int transactionId, int toUserId, const std::string& statusInfo);
	void SendMessageLocked(const BFCPMessage& msg);
	void ReplyErrorLocked(const BFCPMessage *msg, BFCPTransport *to, BFCPAttrErrorCode::ErrorCode errorCode, const std::string& errorInfo);

	static void Fire(Notifications& pending);

private:
	typedef std::map<int, std::unique_ptr<BFCPUser> > Users;
	typedef std::set<int> Floors;
	typedef std::map<int, std::unique_ptr<BFCPFloorRequest> > FloorRequests;

private:
	Listener *listener;
	int conferenceId;
	std::mutex mutex;
	Users users;
	Floors floors;
	FloorRequests floorRequests;
	int floorRequestCounter;
	int transactionCounter;
	bool ending;
};


#endif /* BFCPFLOORCONTROLSERVER_H */
