# Minimal Samba server that speaks only SMB2 .. SMB 3.1.1, for testing curl's
# libsmbclient-backed smb:// handler.
#
#   user / password : smbuser / smbpass
#   share           : //<host>/share   (contains test.txt)
FROM alpine:3.20

RUN apk add --no-cache samba-server samba-common-tools

COPY smb.conf /etc/samba/smb.conf

RUN mkdir -p /srv/share /var/lib/samba/private /run/samba && \
    printf 'hello from SMB 3.1.1 via libsmbclient\n' > /srv/share/test.txt && \
    dd if=/dev/urandom of=/srv/share/blob.bin bs=1024 count=512 2>/dev/null && \
    adduser -D -H -s /sbin/nologin smbuser && \
    printf 'smbpass\nsmbpass\n' | smbpasswd -a -s smbuser && \
    chown -R smbuser:smbuser /srv/share

EXPOSE 445

# Run in the foreground and log to stdout so `podman logs` shows activity.
CMD ["smbd", "--foreground", "--no-process-group", "--debug-stdout"]
