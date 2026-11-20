#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS
# TailChaser — rehearse the cut-over from SvxLink on a throw-away machine.
#
#   sh tests/cutover.sh                             on Debian Bookworm
#   BASE=debian:trixie-slim sh tests/cutover.sh     ... or another release
#
# bootstrap.sh simplex replace-svxlink / rollback stop and start services on a node
# that is on the air; this runs them first where nothing is. A container with
# Debian Bookworm and systemd as PID 1 is dressed up as a hotspot-image node:
# SvxLink's configuration, scripts and certificate where the image has them,
# svxlink.service / svxlink-watchdog.service / hotspot-online.service with the
# image's dependencies between them (stand-ins: they hold no hardware), and the
# stand-in reflector of tests/e2e_simplex.py, which has signed the node's certificate.
# Then, with the real bootstrap.sh and the real systemd unit:
#
#   * replace-svxlink where tailchaser-simplex cannot start  -> SvxLink is put back
#   * replace-svxlink for real                        -> tailchaser-simplex logs in
#     with SvxLink's certificate; SvxLink is stopped and masked
#   * the service: the systemd watchdog is fed; a killed or hung daemon is
#     replaced, with the PTT release in between; a broken configuration is not
#     retried
#   * a restart of the machine; an update; rollback
#
# What it cannot show is the hardware: there is no sound card, no GPIO chip and
# no SA818 in the container (and none of the host's is passed in — the
# container is not privileged). tailchaser-simplex runs there with bench audio.
#
# Needs Docker. About four minutes. Not part of `make check`.
set -eu
cd "$(dirname "$0")/.."

BASE=${BASE:-debian:bookworm}
IMAGE=tailchaser-cutover-test:$(echo "$BASE" | tr -c 'A-Za-z0-9_.\n-' '-')
NAME=tailchaser-cutover-$$
FAILED=0

echo "building the test machine ($BASE + systemd) ..."
docker build -q --build-arg BASE="$BASE" -t "$IMAGE" tests/cutover >/dev/null
# SYS_ADMIN: systemd mounts (its cgroups, the service's sandbox). SYS_NICE and
# SYS_RESOURCE: the unit asks for real-time priority and its limits.
docker run -d --name "$NAME" --hostname simplex-test \
    --cap-add SYS_ADMIN --cap-add SYS_NICE --cap-add SYS_RESOURCE \
    --security-opt seccomp=unconfined \
    --tmpfs /run --tmpfs /run/lock --tmpfs /tmp:exec \
    -v "$PWD":/src:ro "$IMAGE" >/dev/null
trap 'docker rm -f "$NAME" >/dev/null 2>&1 || true' EXIT INT TERM

booted() {
    for _ in $(seq 1 60); do
        case "$(docker exec "$NAME" systemctl is-system-running 2>/dev/null || true)" in
            running|degraded) return 0 ;;
        esac
        sleep 0.5
    done
    echo "systemd did not come up in the container"; exit 1
}

booted
docker exec "$NAME" bash /src/tests/cutover/run.sh setup
docker exec "$NAME" bash /src/tests/cutover/run.sh cutover || FAILED=1
docker restart -t 20 "$NAME" >/dev/null
booted
docker exec "$NAME" bash /src/tests/cutover/run.sh after-reboot || FAILED=1

echo
if [ "$FAILED" -ne 0 ]; then
    echo "--- the journal of tailchaser-simplex.service ---"
    docker exec "$NAME" journalctl -u tailchaser-simplex.service --no-pager -n 60 || true
    echo "some checks FAILED"
    exit 1
fi
echo "all checks passed"
