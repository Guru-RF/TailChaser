# The machine tests/cutover.sh rehearses on: Debian Bookworm (what the hotspot
# image is built on; BASE=debian:trixie-slim for the next one) with systemd as
# PID 1, and everything tailchaser-simplex needs to build — as on a node that once
# built SvxLink.
ARG BASE=debian:bookworm
FROM ${BASE}
ENV DEBIAN_FRONTEND=noninteractive container=docker
RUN apt-get update -qq \
 && apt-get install -y -qq --no-install-recommends \
        systemd systemd-sysv dbus procps curl ca-certificates \
        build-essential pkg-config libasound2-dev libopus-dev libgpiod-dev libssl-dev \
        python3 openssl >/dev/null \
 && rm -rf /var/lib/apt/lists/* \
 && systemctl mask getty.target console-getty.service systemd-logind.service \
        systemd-udevd.service systemd-udev-trigger.service >/dev/null 2>&1
STOPSIGNAL SIGRTMIN+3
# Docker hands an unprivileged container its cgroup tree read-only; systemd
# needs to write to it.
CMD ["/bin/sh", "-c", "mount -o remount,rw /sys/fs/cgroup 2>/dev/null; exec /sbin/init"]
