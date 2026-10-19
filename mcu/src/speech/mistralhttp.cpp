#include "speech/mistralhttp.h"

#include <mutex>
#include <curl/curl.h>

namespace
{

struct Transfer
{
	std::function<bool(const char*, size_t)>	onBody;
	bool						cancelled = false;
	CURL*						curl;
};

size_t OnBody(char* data, size_t size, size_t count, void* user)
{
	Transfer* transfer = (Transfer*)user;
	const size_t bytes = size * count;

	long status = 0;
	curl_easy_getinfo(transfer->curl, CURLINFO_RESPONSE_CODE, &status);
	if (status < 200 || status > 299)
		return bytes;

	if (!transfer->onBody(data, bytes))
		transfer->cancelled = true;
	return transfer->cancelled ? 0 : bytes;
}

}

MistralHttp::Result MistralHttp::Post(const std::string& baseUrl, const std::string& path, const std::string& apiKey,
				      const std::string& json, const std::string& accept,
				      std::function<bool(const char*, size_t)> onBody)
{
	static std::once_flag init;
	std::call_once(init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

	Result result;
	CURL* curl = curl_easy_init();
	if (!curl)
	{
		result.error = "curl_easy_init failed";
		return result;
	}

	Transfer transfer;
	transfer.onBody = onBody;
	transfer.curl = curl;

	std::string url = baseUrl;
	while (!url.empty() && url.back() == '/')
		url.pop_back();
	url += path;
	const std::string authorization = "Authorization: Bearer " + apiKey;
	const std::string acceptHeader = "Accept: " + accept;

	curl_slist* headers = nullptr;
	// No key: a speech-server control port, which filters by address.
	if (!apiKey.empty())
		headers = curl_slist_append(headers, authorization.c_str());
	headers = curl_slist_append(headers, "Content-Type: application/json");
	headers = curl_slist_append(headers, acceptHeader.c_str());

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json.c_str());
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)json.size());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, OnBody);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
	// A stream that stalls for 15 s is dead.
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 15L);

	const CURLcode code = curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.httpStatus);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	result.cancelled = transfer.cancelled;
	if (result.cancelled)
		return result;
	if (code != CURLE_OK)
		result.error = curl_easy_strerror(code);
	else if (result.httpStatus < 200 || result.httpStatus > 299)
		result.error = "HTTP " + std::to_string(result.httpStatus);
	return result;
}
