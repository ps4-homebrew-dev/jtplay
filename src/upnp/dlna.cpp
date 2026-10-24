// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdlib>
#include <cstring>

#include <tinyxml2.h>

#include "dlna.h"
#include "http.h"

using tinyxml2::XMLDocument;
using tinyxml2::XMLElement;

namespace upnp {

namespace {

// UPnP documents come with XML namespaces; matching on the local part of
// the element name ("service" for "u:service") keeps this independent of
// whatever prefixes a given server chose.
bool NameIs(const XMLElement* e, const char* local)
{
	const char* name = e->Name();
	if (!name)
		return false;
	const char* colon = std::strchr(name, ':');
	return std::strcmp(colon ? colon + 1 : name, local) == 0;
}

const XMLElement* FirstChildLocal(const XMLElement* parent, const char* local)
{
	if (!parent)
		return nullptr;
	for (const XMLElement* e = parent->FirstChildElement(); e; e = e->NextSiblingElement())
		if (NameIs(e, local))
			return e;
	return nullptr;
}

std::string ChildText(const XMLElement* parent, const char* local)
{
	const XMLElement* e = FirstChildLocal(parent, local);
	const char* t = e ? e->GetText() : nullptr;
	return t ? t : "";
}

// Depth-first search for the <device> whose <deviceType> is a MediaServer.
// Some servers nest it inside an embedded <deviceList>.
const XMLElement* FindMediaServerDevice(const XMLElement* device)
{
	if (!device)
		return nullptr;
	const std::string type = ChildText(device, "deviceType");
	if (type.find(":device:MediaServer:") != std::string::npos)
		return device;

	if (const XMLElement* list = FirstChildLocal(device, "deviceList"))
		for (const XMLElement* sub = list->FirstChildElement(); sub; sub = sub->NextSiblingElement())
			if (NameIs(sub, "device"))
				if (const XMLElement* found = FindMediaServerDevice(sub))
					return found;
	return nullptr;
}

std::string XmlEscape(const std::string& s)
{
	std::string out;
	out.reserve(s.size());
	for (char c : s)
	{
		switch (c)
		{
		case '&': out += "&amp;"; break;
		case '<': out += "&lt;"; break;
		case '>': out += "&gt;"; break;
		case '"': out += "&quot;"; break;
		case '\'': out += "&apos;"; break;
		default: out += c; break;
		}
	}
	return out;
}

// Picks the resource to hand to the player. Everything DLNA streams over
// plain HTTP announces "http-get" protocolInfo; among those, the first one
// wins (servers list the original file first and transcodes after it).
void PickResource(const XMLElement* obj, DidlObject& out)
{
	const XMLElement* chosen = nullptr;
	const XMLElement* fallback = nullptr;
	for (const XMLElement* res = obj->FirstChildElement(); res; res = res->NextSiblingElement())
	{
		if (!NameIs(res, "res"))
			continue;
		const char* url = res->GetText();
		if (!url || !*url)
			continue;
		const char* pi = res->Attribute("protocolInfo");
		// Servers attach downscaled preview images to videos and music as
		// extra <res> entries (DLNA.ORG_PN=JPEG_TN and friends). Remember
		// the first one as the thumbnail and keep it out of the playable
		// resource selection.
		if (pi && std::strstr(pi, ":image/") && !out.IsImage())
		{
			if (out.thumb.empty())
				out.thumb = url;
			continue;
		}
		if (!fallback)
			fallback = res;
		// First http-get resource wins, but keep scanning: thumbnail
		// entries are usually listed after the playable one.
		if (!chosen && pi && std::strncmp(pi, "http-get", 8) == 0)
			chosen = res;
	}
	if (!chosen)
		chosen = fallback;
	if (!chosen)
		return;

	out.res_url = chosen->GetText();
}

DidlObject ParseObject(const XMLElement* e, bool container, const std::string& base_url)
{
	DidlObject o;
	o.container = container;
	if (const char* id = e->Attribute("id"))
		o.id = id;

	o.title = ChildText(e, "title");        // dc:title
	o.upnp_class = ChildText(e, "class");   // upnp:class
	o.artist = ChildText(e, "artist");      // upnp:artist
	if (o.artist.empty())
		o.artist = ChildText(e, "creator"); // dc:creator
	o.album = ChildText(e, "album");
	o.genre = ChildText(e, "genre");
	o.date = ChildText(e, "date");
	const std::string art = ChildText(e, "albumArtURI");
	if (!art.empty())
		o.album_art = ResolveUrl(base_url, art);

	if (!container)
	{
		PickResource(e, o);
		if (!o.res_url.empty())
			o.res_url = ResolveUrl(base_url, o.res_url);
		if (!o.thumb.empty())
			o.thumb = ResolveUrl(base_url, o.thumb);
	}
	return o;
}

// One Browse SOAP round trip; appends the page to 'out' and reports how
// many objects the page carried plus the server's TotalMatches.
bool BrowsePage(const MediaServer& server, const std::string& object_id,
                uint32_t start, uint32_t count, BrowseResult& out,
                uint32_t* returned, std::string& error)
{
	const std::string body =
		"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
		"<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\""
		" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
		"<s:Body>"
		"<u:Browse xmlns:u=\"urn:schemas-upnp-org:service:ContentDirectory:1\">"
		"<ObjectID>" + XmlEscape(object_id) + "</ObjectID>"
		"<BrowseFlag>BrowseDirectChildren</BrowseFlag>"
		"<Filter>*</Filter>"
		"<StartingIndex>" + std::to_string(start) + "</StartingIndex>"
		"<RequestedCount>" + std::to_string(count) + "</RequestedCount>"
		"<SortCriteria></SortCriteria>"
		"</u:Browse>"
		"</s:Body>"
		"</s:Envelope>";

	const Headers headers = {
		{"Content-Type", "text/xml; charset=\"utf-8\""},
		{"SOAPACTION",
		 "\"urn:schemas-upnp-org:service:ContentDirectory:1#Browse\""},
	};

	HttpResponse resp;
	if (!HttpRequest("POST", server.control_url, headers, body, resp, error))
		return false;
	if (resp.status != 200)
	{
		error = "the server rejected the browse request (HTTP " +
			std::to_string(resp.status) + ")";
		return false;
	}

	XMLDocument envelope;
	if (envelope.Parse(resp.body.c_str(), resp.body.size()) != tinyxml2::XML_SUCCESS)
	{
		error = "the server sent a malformed SOAP response";
		return false;
	}

	// Envelope -> Body -> BrowseResponse -> { Result, TotalMatches, ... }
	const XMLElement* env = envelope.RootElement();
	const XMLElement* soap_body = FirstChildLocal(env, "Body");
	const XMLElement* browse = FirstChildLocal(soap_body, "BrowseResponse");
	if (!browse)
	{
		error = "the server sent an unexpected SOAP response";
		return false;
	}
	const std::string didl_xml = ChildText(browse, "Result");
	out.total_matches = (uint32_t)std::strtoul(ChildText(browse, "TotalMatches").c_str(), nullptr, 10);
	const uint32_t number_returned =
		(uint32_t)std::strtoul(ChildText(browse, "NumberReturned").c_str(), nullptr, 10);

	// The Result payload is DIDL-Lite XML that arrived entity-escaped inside
	// the envelope; tinyxml2 already unescaped it when reading the text.
	XMLDocument didl;
	if (didl.Parse(didl_xml.c_str(), didl_xml.size()) != tinyxml2::XML_SUCCESS || !didl.RootElement())
	{
		error = "the server sent malformed DIDL-Lite";
		return false;
	}

	uint32_t parsed = 0;
	for (const XMLElement* e = didl.RootElement()->FirstChildElement(); e; e = e->NextSiblingElement())
	{
		if (NameIs(e, "container"))
			out.objects.push_back(ParseObject(e, true, server.control_url));
		else if (NameIs(e, "item"))
			out.objects.push_back(ParseObject(e, false, server.control_url));
		else
			continue;
		parsed++;
	}

	// Trust the larger of the two; some servers report NumberReturned=0
	// while still returning objects.
	*returned = std::max(parsed, number_returned);
	return true;
}

} // namespace

bool DescribeServer(const std::string& location, MediaServer& out, std::string& error)
{
	HttpResponse resp;
	if (!HttpGet(location, resp, error))
		return false;
	if (resp.status != 200)
	{
		error = "device description request failed (HTTP " + std::to_string(resp.status) + ")";
		return false;
	}

	XMLDocument doc;
	if (doc.Parse(resp.body.c_str(), resp.body.size()) != tinyxml2::XML_SUCCESS)
	{
		error = "malformed device description";
		return false;
	}

	const XMLElement* root = doc.RootElement();
	if (!root)
	{
		error = "empty device description";
		return false;
	}

	// Old (UPnP 1.0) descriptions may carry a URLBase that relative URLs
	// resolve against; without one, the description's own URL is the base.
	std::string base = ChildText(root, "URLBase");
	if (base.empty())
		base = location;

	const XMLElement* device = FindMediaServerDevice(FirstChildLocal(root, "device"));
	if (!device)
	{
		error = "not a media server";
		return false;
	}

	out = {};
	out.location = location;
	out.friendly_name = ChildText(device, "friendlyName");
	out.udn = ChildText(device, "UDN");
	const std::string manufacturer = ChildText(device, "manufacturer");
	const std::string model = ChildText(device, "modelName");
	out.model = manufacturer.empty() ? model
		: (model.empty() ? manufacturer : manufacturer + " " + model);
	if (out.friendly_name.empty())
		out.friendly_name = out.model.empty() ? "Media server" : out.model;

	const XMLElement* services = FirstChildLocal(device, "serviceList");
	if (services)
	{
		for (const XMLElement* svc = services->FirstChildElement(); svc; svc = svc->NextSiblingElement())
		{
			if (!NameIs(svc, "service"))
				continue;
			const std::string type = ChildText(svc, "serviceType");
			if (type.find(":service:ContentDirectory:") == std::string::npos)
				continue;
			const std::string control = ChildText(svc, "controlURL");
			if (!control.empty())
			{
				out.control_url = ResolveUrl(base, control);
				break;
			}
		}
	}

	if (out.control_url.empty())
	{
		error = "the server does not expose a content directory";
		return false;
	}
	return true;
}

bool Browse(const MediaServer& server, const std::string& object_id,
            BrowseResult& out, std::string& error, size_t max_objects)
{
	out = {};

	uint32_t start = 0;
	for (;;)
	{
		uint32_t returned = 0;
		if (!BrowsePage(server, object_id, start, 200, out, &returned, error))
			return start > 0; // keep what we already have on a late failure
		if (returned == 0)
			break;
		start += returned;
		if (out.total_matches > 0 && start >= out.total_matches)
			break;
		if (out.objects.size() >= max_objects)
			break;
	}
	return true;
}

} // namespace upnp
