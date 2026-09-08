#include <string.h>

#include "datachannel.h"
#include "tools.h"

bool DCEP::ParseOpen(const BYTE* data,DWORD size,Open& out)
{
	if (!data || size < OpenHeaderLength)
		return false;

	if (data[0] != MessageOpen)
		return false;

	const DWORD labelLength    = get2(data,8);
	const DWORD protocolLength = get2(data,10);

	//Les deux longueurs viennent du pair. Additionnées en DWORD — deux WORD ne
	//peuvent pas y déborder — et comparées à ce qui est RÉELLEMENT arrivé, sinon
	//on lirait au-delà du tampon.
	if (OpenHeaderLength + labelLength + protocolLength > size)
		return false;

	out.channelType = data[1];
	out.priority    = (WORD) get2(data,2);
	out.reliability = get4(data,4);
	out.label.assign((const char*)data + OpenHeaderLength,labelLength);
	out.protocol.assign((const char*)data + OpenHeaderLength + labelLength,protocolLength);

	return true;
}

DWORD DCEP::SerializeOpen(const Open& open,BYTE* data,DWORD size)
{
	const DWORD length = OpenHeaderLength + open.label.size() + open.protocol.size();

	if (!data || size < length)
		return 0;

	data[0] = MessageOpen;
	data[1] = open.channelType;
	set2(data,2,open.priority);
	set4(data,4,open.reliability);
	set2(data,8,open.label.size());
	set2(data,10,open.protocol.size());

	memcpy(data + OpenHeaderLength,open.label.data(),open.label.size());
	memcpy(data + OpenHeaderLength + open.label.size(),open.protocol.data(),open.protocol.size());

	return length;
}

bool DCEP::IsAck(const BYTE* data,DWORD size)
{
	return data && size >= 1 && data[0] == MessageAck;
}

DWORD DCEP::SerializeAck(BYTE* data,DWORD size)
{
	if (!data || size < 1)
		return 0;

	data[0] = MessageAck;
	return 1;
}
