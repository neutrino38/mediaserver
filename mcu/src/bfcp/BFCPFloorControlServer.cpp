#include "bfcp/BFCPFloorControlServer.h"
#include "log.h"


BFCPFloorControlServer::BFCPFloorControlServer(int conferenceId, BFCPFloorControlServer::Listener *listener) :
	listener(listener),
	conferenceId(conferenceId),
	floorRequestCounter(1),
	transactionCounter(0),
	ending(false)
{
}


BFCPFloorControlServer::~BFCPFloorControlServer()
{
	::Debug("BFCPFloorControlServer::~BFCPFloorControlServer() | conference '%d'\n", this->conferenceId);
}


int BFCPFloorControlServer::GetConferenceId() const
{
	return this->conferenceId;
}


void BFCPFloorControlServer::Fire(Notifications& pending)
{
	for (size_t i=0; i<pending.size(); i++)
		pending[i]();
	pending.clear();
}


/* Chair API */

bool BFCPFloorControlServer::AddUser(int userId)
{
	std::lock_guard<std::mutex> lock(mutex);

	if (this->ending)
		return ::Error("BFCPFloorControlServer::AddUser() | already ended\n");

	if (userId < 1 || userId > 0xFFFF)
		return ::Error("BFCPFloorControlServer::AddUser() | userId '%d' out of range\n", userId);

	if (users.find(userId) != users.end())
		return ::Error("BFCPFloorControlServer::AddUser() | userId '%d' already exists in conference '%d'\n", userId, this->conferenceId);

	users[userId].reset(new BFCPUser(userId, this->conferenceId));

	::Log("BFCPFloorControlServer::AddUser() | user '%d' added to conference '%d'\n", userId, this->conferenceId);
	return true;
}


bool BFCPFloorControlServer::RemoveUser(int userId)
{
	Notifications pending;
	bool ok;
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (this->ending)
			return ::Error("BFCPFloorControlServer::RemoveUser() | already ended\n");
		ok = RemoveUserLocked(userId, true, pending);
	}
	Fire(pending);
	return ok;
}


bool BFCPFloorControlServer::RemoveUserLocked(int userId, bool sendGoodbye, Notifications& pending)
{
	Users::iterator it = users.find(userId);
	if (it == users.end())
		return ::Error("BFCPFloorControlServer::RemoveUser() | userId '%d' does not exist in conference '%d'\n", userId, this->conferenceId);

	// Revoke BEFORE erasing: the Revoked notifications are routed by userId, so
	// the user must still be reachable for its own transport to be found.
	BFCPUser* user = it->second.get();
	RevokeUserFloorRequestsLocked(user, pending);

	if (sendGoodbye && user->IsConnected())
		user->SendMessage(BFCPMessage(BFCPMessage::Goodbye, NotificationTransactionIdLocked(user), this->conferenceId, userId));
	user->CloseTransport();

	users.erase(it);

	::Log("BFCPFloorControlServer::RemoveUser() | user '%d' removed from conference '%d'\n", userId, this->conferenceId);
	return true;
}


bool BFCPFloorControlServer::SetChair(int userId)
{
	std::lock_guard<std::mutex> lock(mutex);

	if (this->ending)
		return false;

	BFCPUser* user = GetUserLocked(userId);
	if (! user)
		return ::Error("BFCPFloorControlServer::SetChair() | user '%d' does not exist\n", userId);

	for (Users::iterator it=users.begin(); it!=users.end(); ++it)
		it->second->UnsetChair();
	user->SetChair();

	::Log("BFCPFloorControlServer::SetChair() | user '%d' becomes chair\n", userId);
	return true;
}


bool BFCPFloorControlServer::AddFloor(int floorId)
{
	std::lock_guard<std::mutex> lock(mutex);

	if (this->ending)
		return false;

	if (floorId < 1 || floorId > 0xFFFF)
		return ::Error("BFCPFloorControlServer::AddFloor() | floorId '%d' out of range\n", floorId);

	if (HasFloorLocked(floorId))
		return ::Error("BFCPFloorControlServer::AddFloor() | floor '%d' already exists\n", floorId);

	this->floors.insert(floorId);

	::Log("BFCPFloorControlServer::AddFloor() | floor '%d' added to conference '%d'\n", floorId, this->conferenceId);
	return true;
}


bool BFCPFloorControlServer::GrantFloorRequest(int floorRequestId)
{
	Notifications pending;
	bool ok;
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (this->ending)
			return false;
		ok = GrantFloorRequestLocked(floorRequestId, pending);
	}
	Fire(pending);
	return ok;
}


bool BFCPFloorControlServer::GrantFloorRequestLocked(int floorRequestId, Notifications& pending)
{
	BFCPFloorRequest *floorRequest = GetFloorRequestLocked(floorRequestId);
	if (! floorRequest)
		return ::Error("BFCPFloorControlServer::GrantFloorRequest() | FloorRequest '%d' does not exist\n", floorRequestId);

	if (! floorRequest->CanBeGranted())
		return ::Error("BFCPFloorControlServer::GrantFloorRequest() | cannot grant FloorRequest '%d' in status '%s'\n", floorRequestId, floorRequest->GetStatusName());

	// Revoke whoever holds any of the requested floors.
	std::vector<BFCPFloorRequest*> others = GetFloorRequestsForFloorsLocked(floorRequest->GetFloorIds());
	for (size_t i=0; i<others.size(); i++)
		if (others[i] != floorRequest && others[i]->IsGranted())
			RevokeFloorRequestLocked(others[i]->GetFloorRequestId(), "granted to other user", pending);

	// The sequence the endpoints in service know: Accepted, then Granted.
	// Each gets its OWN transaction id: they are two transactions of ours, and
	// over UDP an acknowledgement names one. Sharing an id would let the peer
	// close both by answering either.
	BFCPUser* requester = GetUserLocked(floorRequest->GetUserId());

	floorRequest->SetStatus(BFCPAttrRequestStatus::Accepted);
	SendFloorRequestStatusLocked(floorRequest, NotificationTransactionIdLocked(requester), floorRequest->GetUserId(), "", false);

	floorRequest->SetStatus(BFCPAttrRequestStatus::Granted);
	::Log("BFCPFloorControlServer::GrantFloorRequest() | FloorRequest '%d' has been granted\n", floorRequestId);
	SendFloorRequestStatusLocked(floorRequest, NotificationTransactionIdLocked(requester), floorRequest->GetUserId(), "", false);

	NotifyForFloorRequestLocked(floorRequest);

	int beneficiaryId = floorRequest->GetBeneficiaryId();
	std::set<int> floorIds = floorRequest->GetFloorIds();
	pending.push_back([this, floorRequestId, beneficiaryId, floorIds]() {
		listener->onFloorGranted(floorRequestId, beneficiaryId, floorIds);
	});

	return true;
}


bool BFCPFloorControlServer::DenyFloorRequest(int floorRequestId, const std::string& statusInfo)
{
	Notifications pending;
	bool ok;
	{
		std::lock_guard<std::mutex> lock(mutex);
		ok = DenyFloorRequestLocked(floorRequestId, statusInfo, pending);
	}
	Fire(pending);
	return ok;
}


bool BFCPFloorControlServer::DenyFloorRequestLocked(int floorRequestId, const std::string& statusInfo, Notifications& pending)
{
	BFCPFloorRequest *floorRequest = GetFloorRequestLocked(floorRequestId);
	if (! floorRequest)
		return ::Error("BFCPFloorControlServer::DenyFloorRequest() | FloorRequest '%d' does not exist\n", floorRequestId);

	if (! floorRequest->CanBeDenied())
		return ::Error("BFCPFloorControlServer::DenyFloorRequest() | cannot deny FloorRequest '%d' in status '%s'\n", floorRequestId, floorRequest->GetStatusName());

	::Log("BFCPFloorControlServer::DenyFloorRequest() | FloorRequest '%d' has been denied\n", floorRequestId);
	floorRequest->SetStatus(BFCPAttrRequestStatus::Denied);

	BFCPUser* requester = GetUserLocked(floorRequest->GetUserId());
	SendFloorRequestStatusLocked(floorRequest, NotificationTransactionIdLocked(requester), floorRequest->GetUserId(), statusInfo, false);

	NotifyForFloorRequestLocked(floorRequest);

	this->floorRequests.erase(floorRequestId);
	return true;
}


bool BFCPFloorControlServer::RevokeFloorRequest(int floorRequestId, const std::string& statusInfo)
{
	Notifications pending;
	bool ok;
	{
		std::lock_guard<std::mutex> lock(mutex);
		ok = RevokeFloorRequestLocked(floorRequestId, statusInfo, pending);
	}
	Fire(pending);
	return ok;
}


bool BFCPFloorControlServer::RevokeFloorRequestLocked(int floorRequestId, const std::string& statusInfo, Notifications& pending)
{
	BFCPFloorRequest *floorRequest = GetFloorRequestLocked(floorRequestId);
	if (! floorRequest)
		return ::Error("BFCPFloorControlServer::RevokeFloorRequest() | FloorRequest '%d' does not exist\n", floorRequestId);

	if (! floorRequest->IsGranted())
		return ::Error("BFCPFloorControlServer::RevokeFloorRequest() | cannot revoke FloorRequest '%d' in status '%s'\n", floorRequestId, floorRequest->GetStatusName());

	::Log("BFCPFloorControlServer::RevokeFloorRequest() | FloorRequest '%d' has been revoked\n", floorRequestId);
	floorRequest->SetStatus(BFCPAttrRequestStatus::Revoked);

	BFCPUser* requester = GetUserLocked(floorRequest->GetUserId());
	SendFloorRequestStatusLocked(floorRequest, NotificationTransactionIdLocked(requester), floorRequest->GetUserId(), statusInfo, false);

	NotifyForFloorRequestLocked(floorRequest);

	int beneficiaryId = floorRequest->GetBeneficiaryId();
	std::set<int> floorIds = floorRequest->GetFloorIds();
	this->floorRequests.erase(floorRequestId);

	if (! this->ending)
		pending.push_back([this, floorRequestId, beneficiaryId, floorIds]() {
			listener->onFloorReleased(floorRequestId, beneficiaryId, floorIds);
		});

	return true;
}


int BFCPFloorControlServer::GetGrantedFloorRequestId(int floorId)
{
	std::lock_guard<std::mutex> lock(mutex);

	if (this->ending)
		return 0;

	if (! HasFloorLocked(floorId)) {
		::Error("BFCPFloorControlServer::GetGrantedFloorRequestId() | floor '%d' does not exist\n", floorId);
		return 0;
	}

	std::vector<BFCPFloorRequest*> requests = GetFloorRequestsForFloorLocked(floorId);
	for (size_t i=0; i<requests.size(); i++)
		if (requests[i]->IsGranted())
			return requests[i]->GetFloorRequestId();

	return 0;
}


bool BFCPFloorControlServer::IsUserConnected(int userId)
{
	std::lock_guard<std::mutex> lock(mutex);

	BFCPUser* user = GetUserLocked(userId);
	return user && user->IsConnected();
}


bool BFCPFloorControlServer::NotifyFloorStatus(int userId, int floorId)
{
	std::lock_guard<std::mutex> lock(mutex);

	if (this->ending)
		return false;

	BFCPUser* user = GetUserLocked(userId);
	if (! user)
		return ::Error("BFCPFloorControlServer::NotifyFloorStatus() | user '%d' does not exist\n", userId);

	if (! HasFloorLocked(floorId))
		return ::Error("BFCPFloorControlServer::NotifyFloorStatus() | floor '%d' does not exist\n", floorId);

	std::unique_ptr<BFCPMsgFloorStatus> floorStatus = BuildFloorStatusLocked(floorId, NotificationTransactionIdLocked(user), userId);
	return user->SendMessage(*floorStatus);
}


void BFCPFloorControlServer::End()
{
	Notifications pending;
	{
		std::lock_guard<std::mutex> lock(mutex);

		if (this->ending)
			return;

		::Log("BFCPFloorControlServer::End() | terminating conference '%d'\n", this->conferenceId);
		this->ending = true;

		std::vector<int> ids;
		for (FloorRequests::iterator it = floorRequests.begin(); it != floorRequests.end(); ++it)
			ids.push_back(it->first);
		for (size_t i=0; i<ids.size(); i++) {
			BFCPFloorRequest* floorRequest = GetFloorRequestLocked(ids[i]);
			if (! floorRequest)
				continue;
			if (floorRequest->IsGranted())
				RevokeFloorRequestLocked(ids[i], "conference ended", pending);
			else
				DenyFloorRequestLocked(ids[i], "conference ended", pending);
		}
		floorRequests.clear();

		for (Users::iterator it = users.begin(); it != users.end(); ++it) {
			BFCPUser* user = it->second.get();
			if (user->IsConnected())
				user->SendMessage(BFCPMessage(BFCPMessage::Goodbye, NotificationTransactionIdLocked(user), this->conferenceId, user->GetUserId()));
			user->CloseTransport();
		}
		users.clear();
	}
	Fire(pending);
}


/* Transport API */

bool BFCPFloorControlServer::UserConnected(int userId, BFCPTransport *transport)
{
	Notifications pending;
	bool ok;
	{
		std::lock_guard<std::mutex> lock(mutex);

		if (this->ending)
			return false;

		BFCPUser *user = GetUserLocked(userId);
		if (! user)
			return ::Error("BFCPFloorControlServer::UserConnected() | connection from invalid userId '%d' ignored in conference '%d'\n", userId, this->conferenceId);

		ok = AttachTransportLocked(user, transport, pending);
	}
	Fire(pending);
	return ok;
}


bool BFCPFloorControlServer::AttachTransportLocked(BFCPUser* user, BFCPTransport* transport, Notifications& pending)
{
	if (user->GetTransport() == transport)
		return true;

	if (user->IsConnected()) {
		::Log("BFCPFloorControlServer::UserConnected() | user '%d' was already connected in conference '%d', resetting user\n", user->GetUserId(), this->conferenceId);
		user->ResetQueriedFloorIds();
		RevokeUserFloorRequestsLocked(user, pending);
		user->CloseTransport();
	}
	user->SetTransport(transport);

	::Log("BFCPFloorControlServer::UserConnected() | user '%d' connected to conference '%d'\n", user->GetUserId(), this->conferenceId);
	return true;
}


void BFCPFloorControlServer::UserDisconnected(int userId, BFCPTransport *transport)
{
	Notifications pending;
	{
		std::lock_guard<std::mutex> lock(mutex);

		BFCPUser *user = GetUserLocked(userId);
		// Removed meanwhile, or the disconnection is that of a superseded transport.
		if (! user || user->GetTransport() != transport)
			return;

		DetachTransportLocked(user, pending);
	}
	Fire(pending);
}


void BFCPFloorControlServer::TransportClosed(BFCPTransport *transport)
{
	Notifications pending;
	{
		std::lock_guard<std::mutex> lock(mutex);

		// A transport carries at most one user, and it is the server that said
		// which: a closing connection has no identity of its own to offer.
		BFCPUser* user = NULL;
		for (Users::iterator it = users.begin(); it != users.end(); ++it)
			if (it->second->GetTransport() == transport) {
				user = it->second.get();
				break;
			}

		// Never attached, or already superseded by a newer connection.
		if (! user)
			return;

		DetachTransportLocked(user, pending);
	}
	Fire(pending);
}


void BFCPFloorControlServer::DetachTransportLocked(BFCPUser* user, Notifications& pending)
{
	::Log("BFCPFloorControlServer::DetachTransport() | user '%d' disconnected from conference '%d'\n", user->GetUserId(), this->conferenceId);

	user->UnsetTransport();
	user->ResetQueriedFloorIds();

	if (! this->ending)
		RevokeUserFloorRequestsLocked(user, pending);
}


void BFCPFloorControlServer::MessageReceived(BFCPMessage *msg, BFCPTransport *from)
{
	Notifications pending;
	{
		std::lock_guard<std::mutex> lock(mutex);

		if (this->ending)
			return;

		::Debug("BFCPFloorControlServer::MessageReceived() | %s received:\n", BFCPMessage::PrimitiveName(msg->GetPrimitive()));
		msg->Dump();

		if (msg->GetConferenceId() != this->conferenceId) {
			ReplyErrorLocked(msg, from, BFCPAttrErrorCode::ConferenceDoesNotExist, "conference does not exist");
			return;
		}

		BFCPUser* user = GetUserLocked(msg->GetUserId());
		if (! user) {
			::Error("BFCPFloorControlServer::MessageReceived() | userId '%d' does not exist, replying Error 'UserDoesNotExist'\n", msg->GetUserId());
			ReplyErrorLocked(msg, from, BFCPAttrErrorCode::UserDoesNotExist, "user does not exist");
			return;
		}

		// A transport belongs to the first user that speaks on it. Another
		// transport may take a user over, but only with a Hello.
		if (user->GetTransport() != from) {
			if (user->IsConnected() && msg->GetPrimitive() != BFCPMessage::Hello) {
				::Error("BFCPFloorControlServer::MessageReceived() | userId '%d' spoke from a transport that is not its own\n", msg->GetUserId());
				ReplyErrorLocked(msg, from, BFCPAttrErrorCode::UnauthorizedOperation, "user is connected elsewhere");
				return;
			}
			AttachTransportLocked(user, from, pending);
		}

		switch (msg->GetPrimitive())
		{
			case BFCPMessage::FloorRequest:
				ProcessFloorRequestLocked((BFCPMsgFloorRequest *)msg, user, pending);
				break;
			case BFCPMessage::FloorRelease:
				ProcessFloorReleaseLocked((BFCPMsgFloorRelease *)msg, user, pending);
				break;
			case BFCPMessage::FloorQuery:
				ProcessFloorQueryLocked((BFCPMsgFloorQuery *)msg, user);
				break;
			case BFCPMessage::Hello:
				ProcessHelloLocked((BFCPMsgHello *)msg, user, pending);
				break;
			case BFCPMessage::Goodbye:
				ProcessGoodbyeLocked(msg, user, pending);
				break;
			case BFCPMessage::HelloAck:
			case BFCPMessage::FloorRequestStatusAck:
			case BFCPMessage::FloorStatusAck:
			case BFCPMessage::GoodbyeAck:
			case BFCPMessage::Error:
				// Ends a server transaction (the transport tracks it) or reports one.
				break;
			default:
				::Error("BFCPFloorControlServer::MessageReceived() | replying Error 'UnknownPrimitive' to %s\n", BFCPMessage::PrimitiveName(msg->GetPrimitive()));
				ReplyErrorLocked(msg, from, BFCPAttrErrorCode::UnknownPrimitive, "unknown or unsupported primitive");
				break;
		}
	}
	Fire(pending);
}


/* Locked helpers */

BFCPUser* BFCPFloorControlServer::GetUserLocked(int userId)
{
	Users::iterator it = users.find(userId);
	return it != users.end() ? it->second.get() : NULL;
}


BFCPFloorRequest* BFCPFloorControlServer::GetFloorRequestLocked(int floorRequestId)
{
	FloorRequests::iterator it = floorRequests.find(floorRequestId);
	return it != floorRequests.end() ? it->second.get() : NULL;
}


std::vector<BFCPFloorRequest*> BFCPFloorControlServer::GetFloorRequestsForFloorLocked(int floorId)
{
	std::vector<BFCPFloorRequest*> result;
	for (FloorRequests::iterator it = floorRequests.begin(); it != floorRequests.end(); ++it)
		if (it->second->HasFloorId(floorId))
			result.push_back(it->second.get());
	return result;
}


std::vector<BFCPFloorRequest*> BFCPFloorControlServer::GetFloorRequestsForFloorsLocked(const std::set<int>& floorIds)
{
	std::vector<BFCPFloorRequest*> result;
	for (FloorRequests::iterator it = floorRequests.begin(); it != floorRequests.end(); ++it)
		for (std::set<int>::const_iterator f = floorIds.begin(); f != floorIds.end(); ++f)
			if (it->second->HasFloorId(*f)) {
				result.push_back(it->second.get());
				break;
			}
	return result;
}


bool BFCPFloorControlServer::HasFloorLocked(int floorId) const
{
	return floors.find(floorId) != floors.end();
}


int BFCPFloorControlServer::NextTransactionIdLocked()
{
	transactionCounter = (transactionCounter % 0xFFFF) + 1;
	return transactionCounter;
}


// RFC 4582 §8.3: over a reliable transport a server-initiated message carries
// transaction id 0. RFC 8855: over UDP it carries a fresh one, to be acked.
int BFCPFloorControlServer::NotificationTransactionIdLocked(const BFCPUser* user)
{
	if (user && ! user->IsReliable())
		return NextTransactionIdLocked();
	return 0;
}


void BFCPFloorControlServer::RevokeUserFloorRequestsLocked(BFCPUser* user, Notifications& pending)
{
	int userId = user->GetUserId();
	std::vector<int> ids;
	for (FloorRequests::iterator it = floorRequests.begin(); it != floorRequests.end(); ++it)
		if (it->second->GetBeneficiaryId() == userId || it->second->GetUserId() == userId)
			ids.push_back(it->first);

	for (size_t i=0; i<ids.size(); i++) {
		BFCPFloorRequest* floorRequest = GetFloorRequestLocked(ids[i]);
		if (! floorRequest)
			continue;
		if (floorRequest->IsGranted())
			RevokeFloorRequestLocked(ids[i], "FloorRequests from this user have been revoked or denied", pending);
		else
			DenyFloorRequestLocked(ids[i], "FloorRequests from this user have been revoked or denied", pending);
	}
}


std::unique_ptr<BFCPMsgFloorStatus> BFCPFloorControlServer::BuildFloorStatusLocked(int floorId, int transactionId, int userId)
{
	std::unique_ptr<BFCPMsgFloorStatus> floorStatus(new BFCPMsgFloorStatus(transactionId, this->conferenceId, userId));
	floorStatus->SetFloorId(floorId);

	std::vector<BFCPFloorRequest*> requests = GetFloorRequestsForFloorLocked(floorId);
	for (size_t i=0; i<requests.size(); i++)
		floorStatus->AddFloorRequestInformation(requests[i]->CreateFloorRequestInformation());

	return floorStatus;
}


// FloorStatus notifications to the users who queried a floor of this request.
void BFCPFloorControlServer::NotifyForFloorRequestLocked(const BFCPFloorRequest* floorRequest)
{
	std::set<int> floorIds = floorRequest->GetFloorIds();

	for (Users::iterator it = users.begin(); it != users.end(); ++it) {
		BFCPUser* user = it->second.get();
		for (std::set<int>::const_iterator f = floorIds.begin(); f != floorIds.end(); ++f) {
			if (! user->HasQueriedFloorId(*f))
				continue;
			std::unique_ptr<BFCPMsgFloorStatus> floorStatus = BuildFloorStatusLocked(*f, NotificationTransactionIdLocked(user), user->GetUserId());
			user->SendMessage(*floorStatus);
		}
	}
}


void BFCPFloorControlServer::SendFloorRequestStatusLocked(const BFCPFloorRequest* floorRequest, int transactionId, int toUserId, const std::string& statusInfo, bool isResponse)
{
	std::unique_ptr<BFCPMsgFloorRequestStatus> floorRequestStatus(floorRequest->CreateFloorRequestStatus(transactionId));
	floorRequestStatus->SetUserId(toUserId);
	floorRequestStatus->SetResponder(isResponse);
	if (! statusInfo.empty())
		floorRequestStatus->SetDescription(statusInfo);
	SendMessageLocked(*floorRequestStatus);
}


void BFCPFloorControlServer::SendMessageLocked(const BFCPMessage& msg)
{
	BFCPUser* user = GetUserLocked(msg.GetUserId());
	if (! user) {
		::Error("BFCPFloorControlServer::SendMessage() | user '%d' does not exist\n", msg.GetUserId());
		return;
	}
	user->SendMessage(msg);
}


void BFCPFloorControlServer::ReplyErrorLocked(const BFCPMessage *msg, BFCPTransport *to, BFCPAttrErrorCode::ErrorCode errorCode, const std::string& errorInfo)
{
	::Log("BFCPFloorControlServer::ReplyError() | replying Error '%s' to %s\n", BFCPAttrErrorCode::CodeName(errorCode), BFCPMessage::PrimitiveName(msg->GetPrimitive()));
	BFCPMsgError error(msg, errorCode, errorInfo);
	error.SetResponder(true);
	if (to)
		to->Send(error);
}


/* Incoming primitives */

void BFCPFloorControlServer::ProcessFloorRequestLocked(BFCPMsgFloorRequest *req, BFCPUser* user, Notifications& pending)
{
	for (int i=0; i < req->CountFloorIds(); i++) {
		if (! HasFloorLocked(req->GetFloorId(i))) {
			::Error("BFCPFloorControlServer::ProcessFloorRequest() | requesting non existing floor '%d'\n", req->GetFloorId(i));
			ReplyErrorLocked(req, user->GetTransport(), BFCPAttrErrorCode::InvalidFloorId, "requested floor does not exist");
			return;
		}
	}

	if (req->HasBeneficiaryId() && ! GetUserLocked(req->GetBeneficiaryId())) {
		::Error("BFCPFloorControlServer::ProcessFloorRequest() | beneficiary '%d' does not exist\n", req->GetBeneficiaryId());
		ReplyErrorLocked(req, user->GetTransport(), BFCPAttrErrorCode::UserDoesNotExist, "beneficiary does not exist");
		return;
	}

	int floorRequestId = this->floorRequestCounter++;

	BFCPFloorRequest* floorRequest = new BFCPFloorRequest(floorRequestId, req->GetUserId(), this->conferenceId);
	if (req->HasBeneficiaryId())
		floorRequest->SetBeneficiaryId(req->GetBeneficiaryId());
	for (int i=0; i < req->CountFloorIds(); i++)
		floorRequest->AddFloorId(req->GetFloorId(i));
	this->floorRequests[floorRequestId].reset(floorRequest);

	::Log("BFCPFloorControlServer::ProcessFloorRequest() | FloorRequest '%d' added to conference '%d'\n", floorRequestId, this->conferenceId);

	// The response: Pending, with the transaction id of the request.
	SendFloorRequestStatusLocked(floorRequest, req->GetTransactionId(), req->GetUserId(), "", true);

	if (user->IsChair()) {
		::Log("BFCPFloorControlServer::ProcessFloorRequest() | the sender is a chair, request authorized\n");
		GrantFloorRequestLocked(floorRequestId, pending);
		return;
	}

	int userId = floorRequest->GetUserId();
	int beneficiaryId = floorRequest->GetBeneficiaryId();
	std::set<int> floorIds = floorRequest->GetFloorIds();
	pending.push_back([this, floorRequestId, userId, beneficiaryId, floorIds]() {
		listener->onFloorRequest(floorRequestId, userId, beneficiaryId, floorIds);
	});
}


void BFCPFloorControlServer::ProcessFloorReleaseLocked(BFCPMsgFloorRelease *req, BFCPUser* user, Notifications& pending)
{
	int floorRequestId = req->GetFloorRequestId();
	BFCPFloorRequest* floorRequest = GetFloorRequestLocked(floorRequestId);

	if (! floorRequest) {
		::Error("BFCPFloorControlServer::ProcessFloorRelease() | FloorRequest '%d' does not exist\n", floorRequestId);
		ReplyErrorLocked(req, user->GetTransport(), BFCPAttrErrorCode::FloorRequestIdDoesNotExist, "FloorRequest does not exist");
		return;
	}

	int sender = user->GetUserId();
	int requester = floorRequest->GetUserId();
	int beneficiary = floorRequest->GetBeneficiaryId();

	// Allowed: the requester, the beneficiary, or a chair.
	if (sender != requester && sender != beneficiary && ! user->IsChair()) {
		::Error("BFCPFloorControlServer::ProcessFloorRelease() | FloorRelease not allowed for user '%d'\n", sender);
		ReplyErrorLocked(req, user->GetTransport(), BFCPAttrErrorCode::UnauthorizedOperation, "you cannot release that FloorRequest");
		return;
	}

	bool wasGranted = floorRequest->IsGranted();

	if (wasGranted)
		floorRequest->SetStatus(sender == beneficiary ? BFCPAttrRequestStatus::Released : BFCPAttrRequestStatus::Revoked);
	else if (floorRequest->CanBeCancelled())
		floorRequest->SetStatus(BFCPAttrRequestStatus::Cancelled);
	else {
		::Error("BFCPFloorControlServer::ProcessFloorRelease() | cannot release a FloorRequest in status '%s'\n", floorRequest->GetStatusName());
		ReplyErrorLocked(req, user->GetTransport(), BFCPAttrErrorCode::UnauthorizedOperation, "cannot release or cancel the FloorRequest due to its current status");
		return;
	}

	// The response to the sender, and a notification to the requester if distinct.
	SendFloorRequestStatusLocked(floorRequest, req->GetTransactionId(), sender, "", true);
	if (sender != requester)
		SendFloorRequestStatusLocked(floorRequest, NotificationTransactionIdLocked(GetUserLocked(requester)), requester, "", false);

	NotifyForFloorRequestLocked(floorRequest);

	std::set<int> floorIds = floorRequest->GetFloorIds();
	this->floorRequests.erase(floorRequestId);

	if (wasGranted)
		pending.push_back([this, floorRequestId, beneficiary, floorIds]() {
			listener->onFloorReleased(floorRequestId, beneficiary, floorIds);
		});
}


void BFCPFloorControlServer::ProcessFloorQueryLocked(BFCPMsgFloorQuery *req, BFCPUser* user)
{
	for (int i=0; i < req->CountFloorIds(); i++) {
		if (! HasFloorLocked(req->GetFloorId(i))) {
			::Error("BFCPFloorControlServer::ProcessFloorQuery() | requesting non existing floor '%d'\n", req->GetFloorId(i));
			ReplyErrorLocked(req, user->GetTransport(), BFCPAttrErrorCode::InvalidFloorId, "requested floor does not exist");
			return;
		}
	}

	// The query replaces the subscription.
	user->ResetQueriedFloorIds();
	for (int i=0; i < req->CountFloorIds(); i++)
		user->AddQueriedFloorId(req->GetFloorId(i));

	int count = user->CountQueriedFloorIds();
	if (count == 0) {
		BFCPMsgFloorStatus floorStatus(req->GetTransactionId(), this->conferenceId, req->GetUserId());
		floorStatus.SetResponder(true);
		user->SendMessage(floorStatus);
		return;
	}

	// The first FloorStatus answers the query, the others are notifications.
	for (int i=0; i<count; i++) {
		const bool isResponse = (i == 0);
		int transactionId = isResponse ? req->GetTransactionId() : NotificationTransactionIdLocked(user);
		std::unique_ptr<BFCPMsgFloorStatus> floorStatus = BuildFloorStatusLocked(user->GetQueriedFloorId(i), transactionId, req->GetUserId());
		floorStatus->SetResponder(isResponse);
		user->SendMessage(*floorStatus);
	}
}


void BFCPFloorControlServer::ProcessHelloLocked(BFCPMsgHello *req, BFCPUser* user, Notifications& pending)
{
	BFCPMsgHelloAck ack(req->GetTransactionId(), this->conferenceId, req->GetUserId());
	ack.AddSupportedPrimitive(BFCPMessage::FloorRequest);
	ack.AddSupportedPrimitive(BFCPMessage::FloorRelease);
	ack.AddSupportedPrimitive(BFCPMessage::FloorRequestStatus);
	ack.AddSupportedPrimitive(BFCPMessage::FloorQuery);
	ack.AddSupportedPrimitive(BFCPMessage::FloorStatus);
	ack.AddSupportedPrimitive(BFCPMessage::Hello);
	ack.AddSupportedPrimitive(BFCPMessage::HelloAck);
	ack.AddSupportedPrimitive(BFCPMessage::Error);
	ack.AddSupportedPrimitive(BFCPMessage::FloorRequestStatusAck);
	ack.AddSupportedPrimitive(BFCPMessage::FloorStatusAck);
	ack.AddSupportedPrimitive(BFCPMessage::Goodbye);
	ack.AddSupportedPrimitive(BFCPMessage::GoodbyeAck);
	ack.AddSupportedAttribute(BFCPAttribute::BeneficiaryId);
	ack.AddSupportedAttribute(BFCPAttribute::FloorId);
	ack.AddSupportedAttribute(BFCPAttribute::FloorRequestId);
	ack.AddSupportedAttribute(BFCPAttribute::Priority);
	ack.AddSupportedAttribute(BFCPAttribute::RequestStatus);
	ack.AddSupportedAttribute(BFCPAttribute::ErrorCode);
	ack.AddSupportedAttribute(BFCPAttribute::ErrorInfo);
	ack.AddSupportedAttribute(BFCPAttribute::ParticipantProvidedInfo);
	ack.AddSupportedAttribute(BFCPAttribute::StatusInfo);
	ack.AddSupportedAttribute(BFCPAttribute::SupportedAttributes);
	ack.AddSupportedAttribute(BFCPAttribute::SupportedPrimitives);
	ack.AddSupportedAttribute(BFCPAttribute::BeneficiaryInformation);
	ack.AddSupportedAttribute(BFCPAttribute::FloorRequestInformation);
	ack.AddSupportedAttribute(BFCPAttribute::RequestedByInformation);
	ack.AddSupportedAttribute(BFCPAttribute::FloorRequestStatus);
	ack.AddSupportedAttribute(BFCPAttribute::OverallRequestStatus);
	ack.SetResponder(true);
	user->SendMessage(ack);

	// Over UDP the server greets back, as the endpoints in service expect.
	if (! user->IsReliable())
		user->SendMessage(BFCPMsgHello(NextTransactionIdLocked(), this->conferenceId, req->GetUserId()));

	int userId = user->GetUserId();
	pending.push_back([this, userId]() {
		listener->onUserConnected(userId);
	});
}


void BFCPFloorControlServer::ProcessGoodbyeLocked(BFCPMessage *req, BFCPUser* user, Notifications& pending)
{
	BFCPMessage goodbyeAck(BFCPMessage::GoodbyeAck, req->GetTransactionId(), this->conferenceId, req->GetUserId());
	goodbyeAck.SetResponder(true);
	user->SendMessage(goodbyeAck);
	RemoveUserLocked(user->GetUserId(), false, pending);
}
