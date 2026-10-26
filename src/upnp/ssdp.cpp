// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <set>

#include <sys/types.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ssdp.h"

namespace upnp {

namespace {

constexpr const char* kSsdpAddr = "239.255.255.250";
constexpr uint16_t kSsdpPort = 1900;

// The search target. MediaServer:1 is answered by every DLNA server,
// including ones that also announce :2/:3/:4 — UPnP requires devices to
// respond to searches for lower versions.
constexpr const char* kSearchTarget = "urn:schemas-upnp-org:device:MediaServer:1";

// Case-insensitive "Header: value" lookup in an SSDP response.
std::string HeaderValue(const std::string& response, const std::string& name)
{
	size_t pos = 0;
	while (pos < response.size())
	{
		size_t eol = response.find("\r\n", pos);
		if (eol == std::string::npos)
			eol = response.size();
		const std::string line = response.substr(pos, eol - pos);
		const size_t colon = line.find(':');
		if (colon != std::string::npos && colon == name.size())
		{
			bool match = true;
			for (size_t i = 0; i < name.size(); i++)
			{
				if (std::tolower((unsigned char)line[i]) != std::tolower((unsigned char)name[i]))
				{
					match = false;
					break;
				}
			}
			if (match)
			{
				size_t v = colon + 1;
				while (v < line.size() && line[v] == ' ')
					v++;
				return line.substr(v);
			}
		}
		pos = eol + 2;
	}
	return {};
}

// One multicast socket per IPv4 interface: on multi-homed hosts the kernel
// picks a single default interface for multicast otherwise, and the server
// may live on one of the others.
std::vector<int> OpenSearchSockets()
{
	std::vector<int> fds;

	ifaddrs* ifs = nullptr;
	if (getifaddrs(&ifs) != 0)
		ifs = nullptr;

	std::set<uint32_t> seen;
	for (ifaddrs* ifa = ifs; ifa; ifa = ifa->ifa_next)
	{
		if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
			continue;
		if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK) ||
		    !(ifa->ifa_flags & IFF_MULTICAST))
			continue;

		const in_addr addr = ((sockaddr_in*)ifa->ifa_addr)->sin_addr;
		if (!seen.insert(addr.s_addr).second)
			continue;

		const int fd = socket(AF_INET, SOCK_DGRAM, 0);
		if (fd < 0)
			continue;
		if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &addr, sizeof(addr)) != 0 ||
		    bind(fd, ifa->ifa_addr, sizeof(sockaddr_in)) != 0)
		{
			close(fd);
			continue;
		}
		const unsigned char ttl = 2;
		setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
		fds.push_back(fd);
	}
	if (ifs)
		freeifaddrs(ifs);

	// No usable interface enumerated: fall back to a plain socket and the
	// kernel's default multicast route.
	if (fds.empty())
	{
		const int fd = socket(AF_INET, SOCK_DGRAM, 0);
		if (fd >= 0)
			fds.push_back(fd);
	}
	return fds;
}

} // namespace

std::vector<SsdpResult> SsdpSearch(int wait_ms, std::string& error)
{
	std::vector<SsdpResult> results;
	error.clear();

	std::vector<int> fds = OpenSearchSockets();
	if (fds.empty())
	{
		error = "cannot create a UDP socket";
		return results;
	}

	sockaddr_in dst = {};
	dst.sin_family = AF_INET;
	dst.sin_port = htons(kSsdpPort);
	inet_pton(AF_INET, kSsdpAddr, &dst.sin_addr);

	const int mx = std::max(1, std::min(5, wait_ms / 1000));
	const std::string msearch =
		"M-SEARCH * HTTP/1.1\r\n"
		"HOST: " + std::string(kSsdpAddr) + ":" + std::to_string(kSsdpPort) + "\r\n"
		"MAN: \"ssdp:discover\"\r\n"
		"MX: " + std::to_string(mx) + "\r\n"
		"ST: " + std::string(kSearchTarget) + "\r\n"
		"\r\n";

	// Send the search twice per socket; SSDP is UDP and the first packet is
	// routinely lost while switches sort out multicast group membership.
	for (int repeat = 0; repeat < 2; repeat++)
		for (int fd : fds)
			sendto(fd, msearch.data(), msearch.size(), 0, (sockaddr*)&dst, sizeof(dst));

	std::set<std::string> seen;
	std::vector<pollfd> pfds;
	for (int fd : fds)
		pfds.push_back({fd, POLLIN, 0});

	// Listen until the deadline, however early poll() comes back: every
	// answer wakes it up, and counting a whole slice per wake-up would
	// end the search long before slower servers have answered.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
	for (;;)
	{
		const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
			deadline - std::chrono::steady_clock::now()).count();
		if (left <= 0)
			break;
		const int pr = poll(pfds.data(), (nfds_t)pfds.size(), (int)left);
		if (pr <= 0)
			continue;

		for (pollfd& p : pfds)
		{
			if (!(p.revents & POLLIN))
				continue;
			char buf[8192];
			const ssize_t n = recv(p.fd, buf, sizeof(buf) - 1, 0);
			if (n <= 0)
				continue;
			buf[n] = 0;
			const std::string response(buf, (size_t)n);

			if (response.compare(0, 15, "HTTP/1.1 200 OK") != 0)
				continue;
			SsdpResult r;
			r.location = HeaderValue(response, "LOCATION");
			r.usn = HeaderValue(response, "USN");
			if (r.location.empty())
				continue;

			const std::string key = r.usn.empty() ? r.location : r.usn;
			if (seen.insert(key).second)
				results.push_back(std::move(r));
		}
	}

	for (int fd : fds)
		close(fd);
	return results;
}

} // namespace upnp
