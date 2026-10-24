// SPDX-License-Identifier: GPL-3.0-or-later
//
// Case-insensitive "natural" ordering of display names: runs of digits
// compare by value, so "Episode 2" sorts before "Episode 10". Bytes outside
// ASCII compare as-is, which keeps UTF-8 names stable if not dictionary
// ordered.
#ifndef BROWSE_NATURAL_SORT_H
#define BROWSE_NATURAL_SORT_H

#include <cctype>
#include <string>

namespace browse {

// <0, 0 or >0, like strcmp.
inline int NaturalCompare(const std::string& a, const std::string& b) {
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    const unsigned char ca = (unsigned char)a[i];
    const unsigned char cb = (unsigned char)b[j];

    if (std::isdigit(ca) && std::isdigit(cb)) {
      // Skip leading zeros, then the longer run of digits is the larger
      // number; equal lengths compare digit by digit.
      size_t si = i, sj = j;
      while (si < a.size() && a[si] == '0') si++;
      while (sj < b.size() && b[sj] == '0') sj++;
      size_t ei = si, ej = sj;
      while (ei < a.size() && std::isdigit((unsigned char)a[ei])) ei++;
      while (ej < b.size() && std::isdigit((unsigned char)b[ej])) ej++;
      if (ei - si != ej - sj)
        return (ei - si) < (ej - sj) ? -1 : 1;
      for (size_t k = 0; k < ei - si; k++) {
        if (a[si + k] != b[sj + k])
          return a[si + k] < b[sj + k] ? -1 : 1;
      }
      i = ei;
      j = ej;
      continue;
    }

    const int la = std::tolower(ca);
    const int lb = std::tolower(cb);
    if (la != lb)
      return la < lb ? -1 : 1;
    i++;
    j++;
  }
  if (i < a.size()) return 1;
  if (j < b.size()) return -1;
  return 0;
}

inline bool NaturalLess(const std::string& a, const std::string& b) {
  return NaturalCompare(a, b) < 0;
}

} // namespace browse

#endif
