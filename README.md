<!--
Copyright (C) Daniel Stenberg, <daniel@haxx.se>, et al.

SPDX-License-Identifier: curl
-->

# [![curl logo](https://github.com/nicholasscotbuchanan/gurl/blob/master/gurl.png)](https://github.com/nicholasscotbuchanan/gurl/edit/master/README.md)

gurl is the multi-threded curl — a **fork of [curl](https://curl.se/)**, not
curl itself. It is built on curl's source and owes everything to it; we just add
a few things on top. Huge thanks to Daniel Stenberg and the curl contributors
for the tool all of this stands on. gurl is not affiliated with or endorsed by
the curl project, and its bugs are ours, not theirs.

curl is a command-line tool for transferring data from or to a server using
URLs. It supports these protocols: DICT, FILE, FTP, FTPS, GOPHER, GOPHERS, HTTP,
HTTPS, IMAP, IMAPS, LDAP, LDAPS, MQTT, MQTTS, POP3, POP3S, RTSP, SCP, SFTP, SMB,
SMTP, SMTPS, TELNET, TFTP, WS and WSS.

But gurl ALSO supports
- NFSv3
- SMB 2 and SMB 3, all the way up to SMB 3.1.1 — with signing and encryption. It's here!
- Multithreaded downloads

### NFS

NFSv3 lives at `nfs://server/export/path` (backed by libnfs, `--with-libnfs`):

    curl nfs://storage/exports/data/report.csv -o report.csv
    curl -T ./report.csv nfs://storage/exports/data/report.csv    # upload
    curl -C - nfs://storage/exports/data/big.iso -o big.iso        # resume

### SMB

The `smb://` scheme now speaks SMB2 and SMB3 (up to SMB 3.1.1, with signing and
encryption) via [libsmb2](https://github.com/sahlberg/libsmb2), `--with-libsmb2`.
It never drops back to SMBv1.

    # authenticate with -u (domain accounts as DOMAIN/user)
    curl -u 'user:password' smb://fileserver/share/docs/report.pdf -O
    curl -u 'user:password' -T ./report.pdf smb://fileserver/share/docs/report.pdf
    curl -u 'user:password' -C - smb://fileserver/share/big.iso -o big.iso   # resume
    curl -u 'user:password' smb://fileserver:4450/share/report.pdf -O        # custom port

A ready-to-run Samba test server lives in [`tests/smbserver/`](tests/smbserver/).

## Prebuilt binaries

Static, self-contained builds for Linux (x86_64/aarch64), macOS (Intel and
Apple Silicon), Windows and FreeBSD are attached to each
[release](https://github.com/nicholasscotbuchanan/gurl/releases). They bundle
OpenSSL, libnfs and libsmb2, so `nfs://` and `smb://` work out of the box — the
binary is still named `curl`.

After downloading, make it executable and (optionally) verify it against the
published checksums:

    chmod +x curl-macos-arm64
    shasum -a 256 -c SHA256SUMS.txt      # sha256sum -c on Linux

**macOS note.** The Apple Silicon (arm64) build is **ad-hoc** code-signed — it
has to be, or Apple Silicon will not launch it — but it is **not** Apple
Developer-ID signed or notarized. The Intel (x86_64) build is unsigned and runs
under Rosetta. Either way, if you download through a browser macOS quarantines
the file; clear that before the first run:

    xattr -d com.apple.quarantine curl-macos-arm64

(or right-click the file in Finder and choose Open once).

Learn how to use curl by reading [the
man page](https://curl.se/docs/manpage.html) or [everything
curl](https://everything.curl.dev/).

Find out how to install curl by reading [the INSTALL
document](https://curl.se/docs/install.html).

libcurl is the library curl is using to do its job. It is readily available to
be used by your software. Read [the libcurl
man page](https://curl.se/libcurl/c/libcurl.html) to learn how.

## Open Source

curl is Open Source and is distributed under an MIT-like
[license](https://curl.se/docs/copyright.html). gurl keeps that same license for
everything inherited from curl; the code added here is offered under it too.

## Contact

For **gurl** — anything about this fork, including the NFS, SMB and
multithreaded-download features — use the gurl
[issues](https://github.com/nicholasscotbuchanan/gurl/issues) and
[pull requests](https://github.com/nicholasscotbuchanan/gurl/pulls). Please do
**not** send gurl's bugs to the curl project; they did not write this and are
not responsible for it.

For **curl** itself, use curl's own [mailing lists](https://curl.se/mail/) and
[issues](https://github.com/curl/curl/issues). Every curl contributor is listed
in [the THANKS document](https://curl.se/docs/thanks.html) — gurl is grateful to
all of them.

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
