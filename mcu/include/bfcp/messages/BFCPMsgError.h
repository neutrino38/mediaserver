#ifndef BFCPMSGERROR_H
#define	BFCPMSGERROR_H


#include "bfcp/BFCPMessage.h"
#include "bfcp/attributes.h"
#include <string>


// 5.3.13.  Error
//
//    Error =   (COMMON-HEADER)
//              (ERROR-CODE)
//              [ERROR-INFO]
//             *[EXTENSION-ATTRIBUTE]


class BFCPMsgError : public BFCPMessage
{
public:
	BFCPMsgError(int transactionId, int conferenceId, int userId);
	BFCPMsgError(const BFCPMessage *msg, enum BFCPAttrErrorCode::ErrorCode errorCode);
	BFCPMsgError(const BFCPMessage *msg, enum BFCPAttrErrorCode::ErrorCode errorCode, const std::string& errorInfo);
	~BFCPMsgError();
	bool IsValid();
	void Dump();

	void SetErrorCode(enum BFCPAttrErrorCode::ErrorCode);
	void SetErrorInfo(const std::string& errorInfo);
	bool HasErrorCode() const;
	bool HasErrorInfo() const;
	enum BFCPAttrErrorCode::ErrorCode GetErrorCode() const;
	const std::string& GetErrorInfo() const;

private:
	// Mandatory attributes.
	BFCPAttrErrorCode *errorCode;
	// Optional attributes.
	BFCPAttrErrorInfo *errorInfo;
};


#endif  /* BFCPMSGERROR_H */
