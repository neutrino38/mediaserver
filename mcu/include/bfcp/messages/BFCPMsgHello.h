#ifndef BFCPMSGHELLO_H
#define	BFCPMSGHELLO_H


#include "bfcp/BFCPMessage.h"


// 5.3.11.  Hello
//
//    Hello =   (COMMON-HEADER)
//             *[EXTENSION-ATTRIBUTE]


class BFCPMsgHello : public BFCPMessage
{
public:
	BFCPMsgHello(int transactionId, int conferenceId, int userId);
	~BFCPMsgHello();
	void Dump();
};


#endif  /* BFCPMSGHELLO_H */
