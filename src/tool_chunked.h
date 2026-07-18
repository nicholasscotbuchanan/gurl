#ifndef HEADER_CURL_TOOL_CHUNKED_H
#define HEADER_CURL_TOOL_CHUNKED_H
/***************************************************************************
 *                                  _   _ ____  _
 *  Project                     ___| | | |  _ \| |
 *                             / __| | | | |_) | |
 *                            | (__| |_| |  _ <| |___
 *                             \___|\___/|_| \_\_____|
 *
 * Copyright (C) Daniel Stenberg, <daniel@haxx.se>, et al.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution. The terms
 * are also available at https://curl.se/docs/copyright.html.
 *
 * You may opt to use, copy, modify, merge, publish, distribute and/or sell
 * copies of the Software, and permit persons to whom the Software is
 * furnished to do so, under the terms of the COPYING file.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 * SPDX-License-Identifier: curl
 *
 ***************************************************************************/
#include "tool_setup.h"

struct per_transfer;

/*
 * Learn a download's size up front with one cheap metadata request (HEAD/SIZE/
 * GETATTR) on per->curl, before any body byte is transferred. Returns TRUE and
 * sets '*total' when the file is larger than 1 GiB and the protocol honors byte
 * ranges, meaning the caller should fetch it as chunks; FALSE means perform an
 * ordinary single transfer. Leaves per->curl reset to a normal GET.
 */
bool tool_chunk_probe(struct per_transfer *per, curl_off_t *total);

/*
 * Fetch a range-capable download of 'total' bytes as fixed 1 GiB byte-range
 * chunks written to their offsets in one output file, keeping a fixed number
 * of streams in flight. Returns the final result of the download.
 */
CURLcode tool_chunk_download(struct per_transfer *per, CURLSH *share,
                             curl_off_t total);

#endif /* HEADER_CURL_TOOL_CHUNKED_H */
