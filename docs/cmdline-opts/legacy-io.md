---
c: Copyright (C) Daniel Stenberg, <daniel@haxx.se>, et al.
SPDX-License-Identifier: curl
Long: legacy-io
Help: Download over a single connection (no parallel chunks)
Added: 8.22.0
Category: connection curl global
Multi: boolean
Scope: global
See-also:
  - parallel
  - parallel-max
Example:
  - --legacy-io $URL
---

# `--legacy-io`

Force curl to download over a single connection, disabling the automatic
parallel chunked download it otherwise performs.

By default, for a large enough download over a protocol that honors byte
ranges, curl probes the total size up front and then fetches the file as
multiple byte-range requests over parallel connections to improve throughput.
Use this option to opt out of that behavior and perform a single sequential
transfer instead, for example when a server behaves poorly with concurrent
range requests.
