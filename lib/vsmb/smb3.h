#ifndef HEADER_CURL_SMB3_H
#define HEADER_CURL_SMB3_H
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
#include "curl_setup.h"

#ifdef USE_LIBSMB2

#include "protocol.h"

/* SMB2/3 handler backed by libsmb2, providing modern SMB dialects
   dialects up to SMB 3.1.1 (signing and AES-GCM encryption). When built,
   this replaces the native SMBv1 handler for the smb:// scheme. */
extern const struct Curl_protocol Curl_protocol_smb3;

#endif /* USE_LIBSMB2 */

#endif /* HEADER_CURL_SMB3_H */
