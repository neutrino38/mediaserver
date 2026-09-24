#ifndef BFCPATTRREQUESTSTATUS_H
#define	BFCPATTRREQUESTSTATUS_H


#include "bfcp/BFCPAttribute.h"


// 5.2.5.  REQUEST-STATUS
//
//    The following is the format of the REQUEST-STATUS attribute.
//
//       0                   1                   2                   3
//       0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//      |0 0 0 0 1 0 1|M|0 0 0 0 0 1 0 0|Request Status |Queue Position |
//      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+


class BFCPAttrRequestStatus : public BFCPAttribute
{
public:
	enum Status {
		Pending = 1,
		Accepted,
		Granted,
		Denied,
		Cancelled,
		Released,
		Revoked
	};

	static const char* StatusName(enum Status status);

public:
	BFCPAttrRequestStatus();
	BFCPAttrRequestStatus(enum Status status, int queuePosition);
	void Dump();
	size_t Serialize(BYTE* out, size_t max, bool mandatory) const;
	static BFCPAttrRequestStatus* Parse(const BYTE* contents, size_t len);

	void SetStatus(enum Status status);
	void SetQueuePosition(int queuePosition);
	enum Status GetStatus() const;
	int GetQueuePosition() const;

private:
	enum Status status;
	int queuePosition;
};


#endif
