#ifndef BFCPMESSAGE_H
#define	BFCPMESSAGE_H


#include "config.h"
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

	// 5.1.  COMMON-HEADER
	//
	//    0                   1                   2                   3
	//    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
	//   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	//   |Ver|R|F| Res |  Primitive    |        Payload Length         |
	//   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	//   |                         Conference ID                         |
	//   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	//   |         Transaction ID        |            User ID            |
	//   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	//   |                 Fragment Offset (if F is set)                 |
	//   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	//   |                 Fragment Length (if F is set)                 |
	//   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	//
	// Payload Length counts 4-octet units and excludes this header.

	static constexpr size_t HeaderLen		= 12;
	// RFC 4582 over a reliable transport; RFC 8855 over an unreliable one.
	static constexpr int VersionReliable	= 1;
	static constexpr int VersionUnreliable	= 2;
	// A Payload Length of 16 bits in 4-octet units, plus the header.
	static constexpr size_t MaxLen		= HeaderLen + 0xFFFF * 4;
	// Returned by Serialize when the message does not fit.
	static constexpr size_t Failed		= (size_t)-1;

	static const char* PrimitiveName(enum Primitive primitive);
	// Builds the typed message the octets describe. NULL when they do not
	// describe one: the caller owns what it gets.
	static BFCPMessage* Parse(const BYTE* data, size_t size);

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

	// Writes header and attributes. Failed when they do not fit.
	size_t Serialize(BYTE* out, size_t max) const;

	void SetVersion(int version);
	int GetVersion() const;
	void SetResponder(bool responder);
	bool IsResponder() const;

protected:
	virtual bool IsValid();
	// Failed on error. A message with no attribute writes 0 and succeeds.
	virtual size_t SerializeAttributes(BYTE* out, size_t max) const;
	virtual bool ParseAttributes(const BYTE* data, size_t size);

protected:
	enum BFCPMessage::Primitive primitive;
	int transactionId;
	int conferenceId;
	int userId;
	int version;
	bool responder;
};


#endif /* BFCPMESSAGE_H */
