// SPDX-License-Identifier: GPL-3.0-or-later
//
// The DLNA client side: turning an SSDP LOCATION into a usable MediaServer
// (device description + ContentDirectory control URL), and browsing that
// server's content tree with SOAP Browse calls.
//
// Everything here is blocking network I/O; the app calls it from a worker
// thread only.
#ifndef UPNP_DLNA_H
#define UPNP_DLNA_H

#include <cstdint>
#include <string>
#include <vector>

namespace upnp {

// A discovered media server, ready to be browsed.
struct MediaServer {
  std::string friendly_name;
  std::string model;        // "<manufacturer> <modelName>", best effort
  std::string udn;          // uuid:... from the description
  std::string location;     // description document URL (shown as the host)
  std::string control_url;  // absolute ContentDirectory control URL
};

// One entry of a DIDL-Lite listing: either a container ("folder") or an
// item with at least one <res> to play. Only what browse::Entry can carry
// is kept; DIDL's remaining metadata is dropped while parsing.
struct DidlObject {
  std::string id;
  std::string title;
  std::string upnp_class;   // object.container..., object.item.videoItem...
  bool container = false;

  // Metadata, all optional.
  std::string artist;       // dc:creator or upnp:artist
  std::string album;
  std::string genre;
  std::string date;         // dc:date
  std::string album_art;    // upnp:albumArtURI, absolute
  std::string thumb;        // image <res> on a non-image item (JPEG_TN etc)

  std::string res_url;      // best playable resource (http-get preferred)

  // Best artwork URL for this object: cover art if announced, otherwise a
  // server-generated thumbnail (videos), otherwise nothing.
  const std::string& ArtUrl() const { return album_art.empty() ? thumb : album_art; }

  bool IsAudio() const { return upnp_class.rfind("object.item.audioItem", 0) == 0; }
  bool IsVideo() const { return upnp_class.rfind("object.item.videoItem", 0) == 0; }
  bool IsImage() const { return upnp_class.rfind("object.item.imageItem", 0) == 0; }
};

struct BrowseResult {
  std::vector<DidlObject> objects;
  uint32_t total_matches = 0;
};

// Fetches and parses the device description at 'location'. Returns false
// (with a reason in 'error') if the document is unreachable, is not a
// MediaServer, or exposes no ContentDirectory service.
bool DescribeServer(const std::string& location, MediaServer& out, std::string& error);

// ContentDirectory Browse with BrowseFlag=BrowseDirectChildren. The root of
// every server is object id "0". Handles paging internally and returns the
// complete listing (capped at 'max_objects' for pathological folders).
bool Browse(const MediaServer& server, const std::string& object_id,
            BrowseResult& out, std::string& error, size_t max_objects = 5000);

} // namespace upnp

#endif
