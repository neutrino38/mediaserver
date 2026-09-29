#include "bfcp/BFCPAttribute.h"
#include "log.h"
#include <string.h>


const char* BFCPAttribute::TypeName(int type)
{
	switch (type)
	{
		case BeneficiaryId:		return "BENEFICIARY-ID";
		case FloorId:			return "FLOOR-ID";
		case FloorRequestId:		return "FLOOR-REQUEST-ID";
		case Priority:			return "PRIORITY";
		case RequestStatus:		return "REQUEST-STATUS";
		case ErrorCode:			return "ERROR-CODE";
		case ErrorInfo:			return "ERROR-INFO";
		case ParticipantProvidedInfo:	return "PARTICIPANT-PROVIDED-INFO";
		case StatusInfo:		return "STATUS-INFO";
		case SupportedAttributes:	return "SUPPORTED-ATTRIBUTES";
		case SupportedPrimitives:	return "SUPPORTED-PRIMITIVES";
		case UserDisplayName:		return "USER-DISPLAY-NAME";
		case UserUri:			return "USER-URI";
		case BeneficiaryInformation:	return "BENEFICIARY-INFORMATION";
		case FloorRequestInformation:	return "FLOOR-REQUEST-INFORMATION";
		case RequestedByInformation:	return "REQUESTED-BY-INFORMATION";
		case FloorRequestStatus:	return "FLOOR-REQUEST-STATUS";
		case OverallRequestStatus:	return "OVERALL-REQUEST-STATUS";
	}
	return "unknown";
}


size_t BFCPAttribute::Padded(size_t length)
{
	return (length + 3) & ~(size_t)3;
}


size_t BFCPAttribute::Write(BYTE* out, size_t max, enum BFCPAttribute::Name type, bool mandatory, const BYTE* contents, size_t contentsLen)
{
	const size_t length = HeaderLen + contentsLen;

	// The Length field is 8 bits: a longer attribute cannot be expressed, and
	// truncating it would put a lie on the wire.
	if (length > MaxLength) {
		::Error("BFCPAttribute::Write() | %s is %zu octets, over the 255 the Length field holds\n", TypeName(type), length);
		return Failed;
	}

	const size_t total = Padded(length);
	if (total > max)
		return Failed;

	out[0] = (BYTE)((type << 1) | (mandatory ? 1 : 0));
	out[1] = (BYTE)length;
	if (contentsLen)
		memcpy(out + HeaderLen, contents, contentsLen);
	// The padding is not counted by Length, but it IS on the wire.
	memset(out + length, 0, total - length);

	return total;
}


bool BFCPAttribute::ReadWord(const BYTE* contents, size_t len, WORD& value)
{
	if (len < 2)
		return false;
	value = (WORD)get2(contents, 0);
	return true;
}


BFCPAttrCursor::BFCPAttrCursor(const BYTE* data, size_t size) :
	data(data),
	size(size),
	pos(0),
	type(0),
	mandatory(false),
	contents(NULL),
	contentsLen(0),
	malformed(false)
{
}


bool BFCPAttrCursor::Next()
{
	// A clean end is pos == size exactly. Any leftover too short to hold an
	// attribute header is caught below, as malformed.
	if (malformed || pos == size)
		return false;

	if (pos + BFCPAttribute::HeaderLen > size) {
		::Error("BFCPAttrCursor::Next() | %zu octets left, too few for an attribute header\n", size - pos);
		malformed = true;
		return false;
	}

	const size_t length = data[pos + 1];

	// A length that does not even cover its own header would leave the cursor
	// in place: the walk would never end.
	if (length < BFCPAttribute::HeaderLen) {
		::Error("BFCPAttrCursor::Next() | attribute length %zu is below its own header\n", length);
		malformed = true;
		return false;
	}

	const size_t total = BFCPAttribute::Padded(length);
	if (pos + total > size) {
		::Error("BFCPAttrCursor::Next() | attribute claims %zu octets, %zu left\n", total, size - pos);
		malformed = true;
		return false;
	}

	type		= data[pos] >> 1;
	mandatory	= (data[pos] & 0x01) != 0;
	contents	= data + pos + BFCPAttribute::HeaderLen;
	contentsLen	= length - BFCPAttribute::HeaderLen;
	pos	       += total;

	return true;
}
