# SMB 3.1.1 test server

A minimal Samba container for testing curl's libsmbclient-backed `smb://`
handler (`lib/vsmb/smb3.c`). Configured to speak **only** SMB2 .. SMB 3.1.1 —
SMBv1 is disabled, so a successful transfer proves the modern code path.

## Layout

| file | purpose |
| --- | --- |
| `Dockerfile` | Alpine + Samba, seeds the share and the `smbuser` account |
| `smb.conf` | `server min protocol = SMB2`, `server max protocol = SMB3_11` |
| `smb3-server.sh` | start / stop / status / encrypt / logs |
| `test-smb3.sh` | curl-only assertions against the running server |

## Usage

```sh
./smb3-server.sh start          # build + run, binds 127.0.0.1:445
./test-smb3.sh                  # run the suite (uses ../../src/curl)
./smb3-server.sh stop
```

Share details:

- URL: `smb://127.0.0.1/share`
- Credentials: `smbuser` / `smbpass`
- Contents: `test.txt` (known string), `blob.bin` (512 KiB random)

```sh
curl -u smbuser:smbpass smb://127.0.0.1/share/test.txt
curl -u smbuser:smbpass -T ./local.txt smb://127.0.0.1/share/remote.txt
```

## Verifying the negotiated dialect

`smb3-server.sh status` prints the live session while a transfer is running:

```
PID  Username  Machine          Protocol Version  Encryption    Signing
56   smbuser   ipv4:...:58558   SMB3_11           AES-128-GCM   AES-128-GMAC
```

Sessions are short-lived on loopback, so poll during a transfer to catch one:

```sh
curl -s -u smbuser:smbpass smb://127.0.0.1/share/blob.bin -o /dev/null &
./smb3-server.sh status
```

## Forcing encryption

`./smb3-server.sh encrypt` flips the server to `smb encrypt = required`, which
only SMB3 clients can satisfy. Re-running `test-smb3.sh` afterwards exercises
the AES-GCM transform end to end.

## Notes

- Requires `podman` (macOS) or `docker`; the script picks whichever exists.
- Host port 445 must be free — macOS has no native service on it by default,
  but "Windows File Sharing" in System Settings will conflict if enabled.
- The handler ignores any port in the URL and always uses 445, so the
  container must be published on 445 rather than a high port.
