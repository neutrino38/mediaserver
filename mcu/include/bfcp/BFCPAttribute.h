#ifndef BFCPATTRIBUTE_H
#define	BFCPATTRIBUTE_H


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

public:
	virtual ~BFCPAttribute() {}
	virtual void Dump() = 0;
};


#endif  /* BFCPATTRIBUTE_H */
