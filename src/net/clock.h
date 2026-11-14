// SPDX-License-Identifier: GPL-3.0-or-later
//
// The time https certificates are judged by. A console kept offline (as a
// jailbroken PS4 often is) can have its clock days off, and then every
// valid certificate looks expired or not valid yet. The servers know the
// time: HttpClient learns how far off this clock is from their Date
// headers (see http_client.cpp), and checks certificates by that.
#ifndef NET_CLOCK_H
#define NET_CLOCK_H

#include <atomic>
#include <cstdint>
#include <ctime>

namespace netclock {

// Seconds the servers' clock is ahead of the console's; 0 until known.
inline std::atomic<int64_t> skew{0};

// Off by less than this, the console's clock is left alone.
constexpr int64_t kToleranceSec = 60;

inline int64_t Now() { return (int64_t)std::time(nullptr) + skew.load(); }

} // namespace netclock

#endif
