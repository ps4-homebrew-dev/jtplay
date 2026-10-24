// SPDX-License-Identifier: GPL-3.0-or-later
//
// SSDP discovery: multicasts an M-SEARCH for MediaServer devices and
// collects the unicast responses. Blocking; run it on a worker thread.
#ifndef UPNP_SSDP_H
#define UPNP_SSDP_H

#include <string>
#include <vector>

namespace upnp {

struct SsdpResult {
  std::string location; // URL of the device description document
  std::string usn;      // unique service name, used for de-duplication
};

// Sends an M-SEARCH for urn:schemas-upnp-org:device:MediaServer:1 on every
// usable IPv4 interface and gathers responses for roughly 'wait_ms'.
// Results are de-duplicated by USN (falling back to LOCATION). An empty
// vector with an empty 'error' simply means nobody answered.
std::vector<SsdpResult> SsdpSearch(int wait_ms, std::string& error);

} // namespace upnp

#endif
