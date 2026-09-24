#ifndef BFCPTRANSPORT_H
#define BFCPTRANSPORT_H


class BFCPMessage;


/**
 * The wire side of one BFCP user: a TCP connection or a UDP socket. Owned by
 * the transport layer, never by the floor control server, which only keeps a
 * pointer while the transport says it is attached.
 */
class BFCPTransport
{
public:
	virtual ~BFCPTransport() {}
	virtual bool Send(const BFCPMessage& msg) = 0;
	virtual void Close() = 0;
	// false for UDP: notifications then need a transaction id and an ack.
	virtual bool IsReliable() const = 0;
};


#endif /* BFCPTRANSPORT_H */
