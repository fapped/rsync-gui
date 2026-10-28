# A minimal ssh server with rsync for the remote tests (tools/ssh-test/run.sh).
FROM alpine:3.22
RUN apk add --no-cache openssh-server rsync \
    && ssh-keygen -A \
    && adduser -D -s /bin/sh tester \
    && echo 'tester:tester-password' | chpasswd \
    && sed -i -e 's/^#\?PasswordAuthentication .*/PasswordAuthentication yes/' \
              -e 's/^#\?KbdInteractiveAuthentication .*/KbdInteractiveAuthentication no/' /etc/ssh/sshd_config
EXPOSE 22
CMD ["/usr/sbin/sshd", "-D", "-e"]
