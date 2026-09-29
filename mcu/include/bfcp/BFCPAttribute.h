#ifndef BFCPATTRIBUTE_H
#define	BFCPATTRIBUTE_H


#include "config.h"
#include <string>


// 5.2.  Attribute Format
//
//       0                   1                   2                   3
//       0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//      |       Type      |M|    Length     |                           |
//      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+                             |
//      |                       Attribute Contents                      |
//      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// Length counts the 2-octet header and the contents, NOT the padding that
// brings the attribute to the next 4-octet boundary. It is 8 bits, so an
// attribute never exceeds 255 octets, padding excluded.


class BFCPAttribute
{
public:
	// Type codes of RFC 4582 §5.2 (RFC 8855 keeps them).
	enum Name {
		BeneficiaryId = 1,
		FloorId,
		FloorRequestId,
		Priority,
		RequestStatus,
		ErrorCode,
		ErrorInfo,
		ParticipantProvidedInfo,
		StatusInfo,
		SupportedAttributes,
		SupportedPrimitives,
		UserDisplayName,
		UserUri,
		BeneficiaryInformation,
		FloorRequestInformation,
		RequestedByInformation,
		FloorRequestStatus,
		OverallRequestStatus,
	};

	static constexpr size_t HeaderLen	= 2;
	static constexpr size_t MaxLength	= 255;
	// Returned by Serialize when the attribute does not fit or cannot be built.
	static constexpr size_t Failed	= (size_t)-1;

	static const char* TypeName(int type);
	// Rounds up to the next 4-octet boundary.
	static size_t Padded(size_t length);
	// Writes a whole attribute (header, contents, padding). Failed on error.
	static size_t Write(BYTE* out, size_t max, enum Name type, bool mandatory, const BYTE* contents, size_t contentsLen);
	// Reads a 16-bit value out of an attribute's contents.
	static bool ReadWord(const BYTE* contents, size_t len, WORD& value);

public:
	virtual ~BFCPAttribute() {}
	virtual void Dump() = 0;
	// The M bit belongs to the position, not to the attribute: the same
	// attribute is mandatory in one message and optional in another, so the
	// parent that writes it says which.
	virtual size_t Serialize(BYTE* out, size_t max, bool mandatory) const = 0;
};


/**
 * Walks a sequence of attributes. Stops at the end, or on the first malformed
 * one — a length that does not fit, or one so short it would not advance.
 */
class BFCPAttrCursor
{
public:
	BFCPAttrCursor(const BYTE* data, size_t size);

	bool Next();
	bool Malformed() const		{ return malformed; }
	int Type() const		{ return type; }
	bool IsMandatory() const	{ return mandatory; }
	const BYTE* Contents() const	{ return contents; }
	size_t ContentsLen() const	{ return contentsLen; }

private:
	const BYTE*	data;
	size_t		size;
	size_t		pos;
	int		type;
	bool		mandatory;
	const BYTE*	contents;
	size_t		contentsLen;
	bool		malformed;
};


#endif  /* BFCPATTRIBUTE_H */
