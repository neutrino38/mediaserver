#include "bfcp/messages/BFCPMsgError.h"
#include "log.h"


BFCPMsgError::BFCPMsgError(int transactionId, int conferenceId, int userId) :
		BFCPMessage(BFCPMessage::Error, transactionId, conferenceId, userId),
		errorCode(NULL),
		errorInfo(NULL)
{
}


BFCPMsgError::BFCPMsgError(const BFCPMessage *msg, enum BFCPAttrErrorCode::ErrorCode errorCode) :
		BFCPMessage(BFCPMessage::Error, msg->GetTransactionId(), msg->GetConferenceId(), msg->GetUserId()),
		errorCode(new BFCPAttrErrorCode(errorCode)),
		errorInfo(NULL)
{
}


BFCPMsgError::BFCPMsgError(const BFCPMessage *msg, enum BFCPAttrErrorCode::ErrorCode errorCode, const std::string& errorInfo) :
		BFCPMessage(BFCPMessage::Error, msg->GetTransactionId(), msg->GetConferenceId(), msg->GetUserId()),
		errorCode(new BFCPAttrErrorCode(errorCode)),
		errorInfo(new BFCPAttrErrorInfo(errorInfo))
{
}


BFCPMsgError::~BFCPMsgError()
{
	if (this->errorCode)
		delete this->errorCode;
	if (this->errorInfo)
		delete this->errorInfo;
}


bool BFCPMsgError::IsValid()
{
	if (! this->errorCode) {
		::Error("BFCPMsgError::IsValid() | MUST have an ErrorCode attribute\n");
		return false;
	}
	return true;
}


void BFCPMsgError::Dump()
{
	::Debug("[BFCPMsgError]\n");
	::Debug("- [primitive: Error, conferenceId: %d, userId: %d, transactionId: %d]\n", this->conferenceId, this->userId, this->transactionId);
	if (this->errorCode)
		::Debug("- errorCode: %s\n", BFCPAttrErrorCode::CodeName(this->errorCode->GetValue()));
	if (this->errorInfo)
		::Debug("- errorInfo: %s\n", this->errorInfo->GetValue().c_str());
	::Debug("[/BFCPMsgError]\n");
}


void BFCPMsgError::SetErrorCode(enum BFCPAttrErrorCode::ErrorCode errorCode)
{
	if (this->errorCode)
		delete this->errorCode;
	this->errorCode = new BFCPAttrErrorCode(errorCode);
}


void BFCPMsgError::SetErrorInfo(const std::string& errorInfo)
{
	if (this->errorInfo)
		delete this->errorInfo;
	this->errorInfo = new BFCPAttrErrorInfo(errorInfo);
}


bool BFCPMsgError::HasErrorCode() const
{
	return this->errorCode != NULL;
}


bool BFCPMsgError::HasErrorInfo() const
{
	return this->errorInfo != NULL;
}


enum BFCPAttrErrorCode::ErrorCode BFCPMsgError::GetErrorCode() const
{
	if (! this->errorCode)
		throw BFCPMessage::AttributeNotFound("'errorCode' attribute not found");
	return this->errorCode->GetValue();
}


const std::string& BFCPMsgError::GetErrorInfo() const
{
	if (! this->errorInfo)
		throw BFCPMessage::AttributeNotFound("'errorInfo' attribute not found");
	return this->errorInfo->GetValue();
}
