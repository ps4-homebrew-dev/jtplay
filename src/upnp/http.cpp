// SPDX-License-Identifier: GPL-3.0-or-later
#include "http.h"

#include "net/http_client.h"

namespace upnp {

namespace {

constexpr long kTimeoutMs = 8000;
// Device descriptions and DIDL listings are small; anything this large is a
// server misbehaving, not something worth holding in memory.
constexpr size_t kMaxBytes = 4u << 20;

} // namespace

bool Url::Parse(const std::string& url, Url& out)
{
	static const std::string scheme = "http://";
	if (url.compare(0, scheme.size(), scheme) != 0)
		return false;

	const size_t host_begin = scheme.size();
	size_t host_end = url.find_first_of("/?", host_begin);
	if (host_end == std::string::npos)
		host_end = url.size();

	std::string hostport = url.substr(host_begin, host_end - host_begin);
	if (hostport.empty())
		return false;

	out.port = 80;
	// IPv6 literals ([::1]:8080) are rare in SSDP LOCATION headers but legal.
	if (hostport[0] == '[')
	{
		const size_t close = hostport.find(']');
		if (close == std::string::npos)
			return false;
		out.host = hostport.substr(1, close - 1);
		if (close + 1 < hostport.size() && hostport[close + 1] == ':')
			out.port = (uint16_t)std::stoi(hostport.substr(close + 2));
	}
	else
	{
		const size_t colon = hostport.rfind(':');
		if (colon != std::string::npos)
		{
			out.host = hostport.substr(0, colon);
			const std::string p = hostport.substr(colon + 1);
			if (p.empty() || p.find_first_not_of("0123456789") != std::string::npos)
				return false;
			out.port = (uint16_t)std::stoi(p);
		}
		else
		{
			out.host = hostport;
		}
	}

	out.path = (host_end < url.size()) ? url.substr(host_end) : "/";
	if (out.path[0] == '?')
		out.path = "/" + out.path;
	return !out.host.empty();
}

std::string ResolveUrl(const std::string& base, const std::string& ref)
{
	if (ref.empty())
		return base;
	if (ref.compare(0, 7, "http://") == 0 || ref.compare(0, 8, "https://") == 0)
		return ref;

	Url b;
	if (!Url::Parse(base, b))
		return ref;

	std::string origin = "http://" + b.host + ":" + std::to_string(b.port);
	if (ref[0] == '/')
		return origin + ref;

	// Relative path: resolve against the base path's directory.
	std::string dir = b.path;
	const size_t q = dir.find('?');
	if (q != std::string::npos)
		dir.resize(q);
	const size_t slash = dir.rfind('/');
	dir = (slash == std::string::npos) ? "/" : dir.substr(0, slash + 1);
	return origin + dir + ref;
}

bool HttpRequest(const char* method, const std::string& url,
                 const Headers& headers, const std::string& body,
                 HttpResponse& response, std::string& error)
{
	// net::HttpClient is not thread safe, so each caller thread keeps its own;
	// that also holds the connection open across a paged Browse.
	static thread_local net::HttpClient client;

	net::HttpClient::Request req;
	req.method = method;
	req.url = url;
	req.headers = headers;
	// Servers key their DLNA quirks off the UPnP user agent, so send the
	// conventional one rather than net::HttpClient's default.
	req.headers.emplace_back("User-Agent", "Linux/1.0 UPnP/1.0 jtplay/1.0");
	req.body = body;
	req.has_body = !body.empty();
	req.timeout_ms = kTimeoutMs;
	req.max_bytes = kMaxBytes;

	net::HttpClient::Response res;
	if (!client.Perform(req, res, error))
		return false;

	response.status = (int)res.status;
	response.body = std::move(res.body);
	return true;
}

} // namespace upnp
