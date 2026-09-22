#include "bfcp/attributes/BFCPAttrErrorCode.h"
#include "log.h"


const char* BFCPAttrErrorCode::CodeName(enum BFCPAttrErrorCode::ErrorCode code)
{
	switch (code)
	{
		case ConferenceDoesNotExist:		return "ConferenceDoesNotExist";
		case UserDoesNotExist:			return "UserDoesNotExist";
		case UnknownPrimitive:			return "UnknownPrimitive";
		case UnknownMandatoryAttribute:		return "UnknownMandatoryAttribute";
		case UnauthorizedOperation:		return "UnauthorizedOperation";
		case InvalidFloorId:			return "InvalidFloorId";
		case FloorRequestIdDoesNotExist:	return "FloorRequestIdDoesNotExist";
		case FloorRequestMaxNumberReached:	return "FloorRequestMaxNumberReached";
		case UseTls:				return "UseTls";
		case UnableToParse:			return "UnableToParse";
		case UseDtls:				return "UseDtls";
		case UnsupportedVersion:		return "UnsupportedVersion";
		case IncorrectMessageLength:		return "IncorrectMessageLength";
		case GenericError:			return "GenericError";
	}
	return "Unknown";
}


BFCPAttrErrorCode::BFCPAttrErrorCode(enum BFCPAttrErrorCode::ErrorCode value) : value(value)
{
}


void BFCPAttrErrorCode::Dump()
{
	::Debug("[BFCPAttrErrorCode]\n");
	::Debug("- value: %s\n", CodeName(this->value));
	::Debug("[/BFCPAttrErrorCode]\n");
}


enum BFCPAttrErrorCode::ErrorCode BFCPAttrErrorCode::GetValue() const
{
	return this->value;
}
