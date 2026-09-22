#ifndef BFCPMESSAGE_H
#define	BFCPMESSAGE_H


#include <stdexcept>


class BFCPMessage
{
public:
	class AttributeNotFound : public std::runtime_error
	{
	public:
		AttributeNotFound(const char*);
	};

public:
	// Primitive codes of RFC 4582 §5.1, plus the four of RFC 8855.
	enum Primitive {
		FloorRequest = 1,
		FloorRelease,
		FloorRequestQuery,
		FloorRequestStatus,
		UserQuery,
		UserStatus,
		FloorQuery,
		FloorStatus,
		ChairAction,
		ChairActionAck,
		Hello,
		HelloAck,
		Error,
		FloorRequestStatusAck,
		FloorStatusAck,
		Goodbye,
		GoodbyeAck
	};

	static const char* PrimitiveName(enum Primitive primitive);

public:
	BFCPMessage(enum BFCPMessage::Primitive primitive, int transactionId, int conferenceId, int userId);
	virtual ~BFCPMessage();
	// Useful for changing the userId of a message.
	void SetUserId(int userId);
	void SetTransactionId(int transactionId);
	enum BFCPMessage::Primitive GetPrimitive() const;
	int GetTransactionId() const;
	int GetConferenceId() const;
	int GetUserId() const;
	virtual void Dump();

protected:
	virtual bool IsValid();

protected:
	enum BFCPMessage::Primitive primitive;
	int transactionId;
	int conferenceId;
	int userId;
};


#endif /* BFCPMESSAGE_H */
