#include "speech/allowlist.h"

#include <cstdlib>

bool AllowList::Add(const std::string& entry, std::string& error)
{
	const size_t slash = entry.find('/');
	const std::string text = entry.substr(0, slash);

	const IPAddress network = IPAddress::Parse(text);
	if (!network.IsSet())
	{
		error = "'" + entry + "' is not an IP address or prefix";
		return false;
	}

	const int bits = network.IsV4() ? 32 : 128;
	int length = bits;

	if (slash != std::string::npos)
	{
		const std::string digits = entry.substr(slash + 1);
		char* end = nullptr;
		const long value = digits.empty() ? -1 : strtol(digits.c_str(), &end, 10);
		if (value < 0 || value > bits || *end != '\0')
		{
			error = "'" + entry + "' has an invalid prefix length";
			return false;
		}
		length = (int)value;
	}

	prefixes.push_back({network, length});
	return true;
}

bool AllowList::Allows(const IPAddress& address) const
{
	for (const Prefix& prefix : prefixes)
		if (address.InPrefix(prefix.network, prefix.length))
			return true;
	return false;
}

bool AllowList::HasIPv6() const
{
	for (const Prefix& prefix : prefixes)
		if (prefix.network.IsV6())
			return true;
	return false;
}
