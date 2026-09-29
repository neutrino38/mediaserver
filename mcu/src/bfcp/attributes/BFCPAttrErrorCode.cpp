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


size_t BFCPAttrErrorCode::Serialize(BYTE* out, size_t max, bool mandatory) const
{
	// No Error Specific Details: the contents are the single code octet, so
	// Length is 3 and one padding octet follows.
	const BYTE contents = (BYTE)this->value;
	return Write(out, max, BFCPAttribute::ErrorCode, mandatory, &contents, 1);
}


BFCPAttrErrorCode* BFCPAttrErrorCode::Parse(const BYTE* contents, size_t len)
{
	if (len < 1)
		return NULL;
	// Error Specific Details, when present, are skipped: none of the codes we
	// answer carries any.
	return new BFCPAttrErrorCode((enum ErrorCode)contents[0]);
}
