# A minimal ssh server with rsync for the remote tests (tools/ssh-test/run.sh). The account "sftponly" can
# use sftp only (like some NAS accounts): it runs no commands, so rsync cannot be used with it. The Samba
# configuration (only testparm, no server) has a share for *Detect* in the share dialog.
FROM alpine:3.22
RUN apk add --no-cache openssh-server rsync samba-common-tools \
    && ssh-keygen -A \
    && adduser -D -s /bin/sh tester \
    && echo 'tester:tester-password' | chpasswd \
    && sed -i -e 's/^#\?PasswordAuthentication .*/PasswordAuthentication yes/' \
              -e 's/^#\?KbdInteractiveAuthentication .*/KbdInteractiveAuthentication no/' /etc/ssh/sshd_config \
    && adduser -D -s /bin/sh sftponly \
    && echo 'sftponly:sftponly-password' | chpasswd \
    && printf 'Match User sftponly\n    ForceCommand internal-sftp\n' >> /etc/ssh/sshd_config \
    && mkdir -p /etc/samba \
    && printf '[global]\n[testshare]\npath = /home/tester/samba\ncreate mask = 0640\nforce create mode = 0004\ndirectory mask = 0750\nforce user = tester\n' > /etc/samba/smb.conf
EXPOSE 22
CMD ["/usr/sbin/sshd", "-D", "-e"]
