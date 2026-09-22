#ifndef BFCPATTRPARTICIPANTPROVIDEDINFO_H
#define	BFCPATTRPARTICIPANTPROVIDEDINFO_H


#include "bfcp/BFCPAttribute.h"
#include <string>


// 5.2.8.  PARTICIPANT-PROVIDED-INFO
//
//       0                   1                   2                   3
//       0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//      |0 0 0 1 0 0 0|M|    Length     |             Text              |
//      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// The text is UTF-8 and is NOT NUL-terminated: its length is what Length
// says, padding excluded.


class BFCPAttrParticipantProvidedInfo : public BFCPAttribute
{
public:
	BFCPAttrParticipantProvidedInfo(const std::string& value);
	void Dump();
	size_t Serialize(BYTE* out, size_t max, bool mandatory) const;
	static BFCPAttrParticipantProvidedInfo* Parse(const BYTE* contents, size_t len);

	const std::string& GetValue() const;

private:
	std::string value;
};


#endif
