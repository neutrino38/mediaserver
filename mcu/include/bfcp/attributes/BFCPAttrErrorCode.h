#ifndef BFCPATTRERRORCODE_H
#define	BFCPATTRERRORCODE_H


#include "bfcp/BFCPAttribute.h"


// 5.2.6.  ERROR-CODE
//
//       0                   1                   2                   3
//       0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//      |0 0 0 0 1 1 0|M|    Length     |  Error Code   |               |
//      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+


class BFCPAttrErrorCode : public BFCPAttribute
{
public:
	enum ErrorCode {
		ConferenceDoesNotExist = 1,
		UserDoesNotExist,
		UnknownPrimitive,
		UnknownMandatoryAttribute,
		UnauthorizedOperation,
		InvalidFloorId,
		FloorRequestIdDoesNotExist,
		FloorRequestMaxNumberReached,
		UseTls,
		UnableToParse,
		UseDtls,
		UnsupportedVersion,
		IncorrectMessageLength,
		GenericError
	};

	static const char* CodeName(enum ErrorCode code);

public:
	BFCPAttrErrorCode(enum ErrorCode errorCode);
	void Dump();
	enum ErrorCode GetValue() const;

private:
	enum ErrorCode value;
};


#endif
