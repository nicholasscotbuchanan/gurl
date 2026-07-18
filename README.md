<!--
Copyright (C) Daniel Stenberg, <daniel@haxx.se>, et al.

SPDX-License-Identifier: curl
-->

# [![curl logo](https://github.com/nicholasscotbuchanan/gurl/blob/master/gurl.png)](https://github.com/nicholasscotbuchanan/gurl/edit/master/README.md)

gurl is the multi-threded curl. curl is a command-line tool for transferring data 
from or to a server using URLs. It supports these protocols: DICT, FILE, FTP, 
FTPS, GOPHER, GOPHERS, HTTP, HTTPS, IMAP, IMAPS, LDAP, LDAPS, MQTT, MQTTS, POP3, 
POP3S, RTSP, SCP, SFTP, SMBv1, SMBS, SMTP, SMTPS, TELNET, TFTP, WS and WSS.

But Gurl ALSO supports
- NFSv3, and SMB 3.11 is on the way! 
- Multithreaded downloads

Learn how to use curl by reading [the
man page](https://curl.se/docs/manpage.html) or [everything
curl](https://everything.curl.dev/).

Find out how to install curl by reading [the INSTALL
document](https://curl.se/docs/install.html).

libcurl is the library curl is using to do its job. It is readily available to
be used by your software. Read [the libcurl
man page](https://curl.se/libcurl/c/libcurl.html) to learn how.

## NFS

NFSv3 support comes from [libnfs](https://github.com/sahlberg/libnfs), enabled
with `--with-libnfs` (autotools) or `-DCURL_USE_LIBNFS=ON` (CMake). libnfs runs
its own RPC transport, so no NFS mount is needed on the client.

URLs are `nfs://server/export/path`:

    # download a file from an export
    curl nfs://storage.example.com/exports/data/report.csv -o report.csv

    # upload
    curl -T ./report.csv nfs://storage.example.com/exports/data/report.csv

    # resume an interrupted download, or fetch a byte range
    curl -C - nfs://storage.example.com/exports/data/big.iso -o big.iso
    curl -r 0-1048575 nfs://storage.example.com/exports/data/big.iso -o head.bin

    # non-default RPC ports, read from the URL by libnfs
    curl "nfs://storage.example.com/exports/data/big.iso?nfsport=2049&mountport=635" -o big.iso

Large downloads are split into parallel chunks automatically; pass
`--legacy-io` to force a single connection.

## SMB

SMB2 and SMB3 — up to SMB 3.1.1, with message signing and AES-GCM encryption —
come from Samba's [libsmbclient](https://www.samba.org/), enabled with
`--with-libsmbclient` (autotools) or `-DCURL_USE_LIBSMBCLIENT=ON` (CMake). The
dialect range is pinned to SMB2..SMB 3.1.1, so it never negotiates down to
SMBv1. Without libsmbclient, curl uses its built-in SMBv1 handler instead.

Note that linking libsmbclient makes the resulting binary GPL-3.0, so it is off
by default.

URLs are `smb://server/share/path`:

    # download, authenticating as a local account on the server
    curl -u 'user:password' smb://fileserver/share/docs/report.pdf -o report.pdf

    # domain accounts are given as DOMAIN/user (DOMAIN\user also works)
    curl -u 'CORP/alice:password' smb://fileserver/share/docs/report.pdf -O

    # upload
    curl -u 'user:password' -T ./report.pdf smb://fileserver/share/docs/report.pdf

    # resume, or fetch a byte range
    curl -u 'user:password' -C - smb://fileserver/share/big.iso -o big.iso
    curl -u 'user:password' -r 0-1048575 smb://fileserver/share/big.iso -o head.bin

    # a non-default port
    curl -u 'user:password' smb://fileserver:4450/share/docs/report.pdf -O

`tests/smbserver/` builds a container running a real Samba server restricted to
SMB2..SMB 3.1.1 to test against.

## Open Source

curl is Open Source and is distributed under an MIT-like
[license](https://curl.se/docs/copyright.html).

## Contact

Contact us on a suitable [mailing list](https://curl.se/mail/) or
use GitHub [issues](https://github.com/curl/curl/issues)/
[pull requests](https://github.com/curl/curl/pulls)/
[discussions](https://github.com/curl/curl/discussions).

All contributors to the project are listed in [the THANKS
document](https://curl.se/docs/thanks.html).

## Commercial support

For commercial support, maybe private and dedicated help with your problems or
applications using (lib)curl visit [the support page](https://curl.se/support.html).

## Website

We don't have one. Sorry. Visit the [curl website](https://curl.se/) for the latest curl news and downloads.

## Source code

Download the latest gurl source from the Git server:

    git clone https://github.com/nicholasscotbuchanan/gurl.git

Download the latest curl source from the Git server:

    git clone https://github.com/curl/curl.git

## Security problems

Report suspected security problems
[privately](https://curl.se/dev/vuln-disclosure.html) and not in public.

## Backers

Thank you to all our backers :pray: [Become a backer](https://opencollective.com/curl#section-contribute).

## Sponsors

Support this project by becoming a [sponsor](https://curl.se/sponsors.html).
