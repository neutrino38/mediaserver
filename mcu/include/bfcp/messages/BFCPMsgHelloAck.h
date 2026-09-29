#ifndef BFCPMSGHELLOACK_H
#define	BFCPMSGHELLOACK_H


#include "bfcp/BFCPMessage.h"
#include "bfcp/attributes.h"


// 5.3.12.  HelloAck
//
//    HelloAck =   (COMMON-HEADER)
//                 (SUPPORTED-PRIMITIVES)
//                 (SUPPORTED-ATTRIBUTES)
//                *[EXTENSION-ATTRIBUTE]


class BFCPMsgHelloAck : public BFCPMessage
{
public:
	BFCPMsgHelloAck(int transactionId, int conferenceId, int userId);
	~BFCPMsgHelloAck();
	bool IsValid();
	void Dump();

protected:
	size_t SerializeAttributes(BYTE* out, size_t max) const;
	bool ParseAttributes(const BYTE* data, size_t size);

public:

	void AddSupportedPrimitive(enum BFCPMessage::Primitive primitive);
	void AddSupportedAttribute(enum BFCPAttribute::Name attribute);
	const BFCPAttrSupportedPrimitives& GetSupportedPrimitives() const;
	const BFCPAttrSupportedAttributes& GetSupportedAttributes() const;

private:
	// Mandatory attributes.
	BFCPAttrSupportedPrimitives supportedPrimitives;
	BFCPAttrSupportedAttributes supportedAttributes;
};


#endif  /* BFCPMSGHELLOACK_H */
