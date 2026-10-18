#ifndef SPEECH_ALLOWLIST_H
#define SPEECH_ALLOWLIST_H

#include <string>
#include <vector>
#include "ipaddress.h"

/**
 * Addresses and CIDR prefixes allowed on the control port (SPEC §6.1).
 */
class AllowList
{
public:
	// "10.0.0.0/8", "192.0.2.7" (a host), "2001:db8::/32". False, with the
	// reason in `error`, when the entry is not one of these forms.
	bool Add(const std::string& entry, std::string& error);

	bool Allows(const IPAddress& address) const;
	bool Empty() const		{ return prefixes.empty(); }
	bool HasIPv6() const;

private:
	struct Prefix
	{
		IPAddress	network;
		int		length;
	};
	std::vector<Prefix> prefixes;
};

#endif
