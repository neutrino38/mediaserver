#ifndef DATACHANNEL_H
#define DATACHANNEL_H

#include <string>
#include "config.h"

/*
 * DCEP — Data Channel Establishment Protocol (RFC 8832).
 *
 * Le protocole qui ouvre un canal DANS une association SCTP déjà montée : deux
 * messages, sur le PPID 50, et le canal est là. Rien d'autre ne circule dessus.
 *
 * Statique et sans état : un message se lit, un message s'écrit. L'état du canal
 * appartient à la couche qui l'utilise (T140DataChannel).
 *
 * Conception : docs/conception/T140-DC/SPEC.md §5.4.
 */
class DCEP
{
public:
	enum MessageType
	{
		MessageAck  = 0x02,
		MessageOpen = 0x03,
	};

	enum ChannelType
	{
		Reliable			= 0x00,
		ReliableUnordered		= 0x80,
		PartialReliableRexmit		= 0x01,
		PartialReliableRexmitUnordered	= 0x81,
		PartialReliableTimed		= 0x02,
		PartialReliableTimedUnordered	= 0x82,
	};

	//En-tête d'un DATA_CHANNEL_OPEN, avant label et protocol :
	//  0    type de message
	//  1    type de canal
	//  2-3  priorité
	//  4-7  paramètre de fiabilité
	//  8-9  longueur du label
	//  10-11 longueur du protocol
	static const DWORD OpenHeaderLength = 12;

	struct Open
	{
		Open() : channelType(Reliable), priority(0), reliability(0) {}

		//Le canal que demande RFC 8865 pour du T.140 : fiable et ordonné, donc
		//sans paramètre de fiabilité partielle.
		bool IsReliableOrdered() const { return channelType == Reliable; }

		BYTE		channelType;
		WORD		priority;
		DWORD		reliability;
		std::string	label;
		std::string	protocol;
	};

	//false si le message n'est pas un OPEN, ou s'il ne tient pas ce qu'il
	//annonce : les deux longueurs sont déclarées dans l'en-tête, et rien
	//n'oblige un pair à dire la vérité.
	static bool  ParseOpen(const BYTE* data,DWORD size,Open& out);
	//Nombre d'octets écrits, 0 si le tampon est trop petit.
	static DWORD SerializeOpen(const Open& open,BYTE* data,DWORD size);

	static bool  IsAck(const BYTE* data,DWORD size);
	static DWORD SerializeAck(BYTE* data,DWORD size);
};

#endif /* DATACHANNEL_H */
