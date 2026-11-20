#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS
# TailChaser — first-boot self-install.
#
#   firstboot.sh <simplex|duplex|receiver|transmitter>
#
# Run once on the first boot of a flashed image by
# tailchaser-firstboot@<role>.service (armed with
# `sudo ./bootstrap.sh <role> prime-firstboot` when baking the image). It runs
# the full bootstrap (deps + WM8960 driver + UART + build + install + enable
# the service), then reboots to apply the firmware changes.
#
# Idempotent + self-disarming: on success it writes a marker and disables the
# one-shot; on failure it leaves the one-shot armed so the next boot retries.
set -u

ROLE="${1:?role}"
SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
MARK="/var/lib/tailchaser/.firstboot-done"
log() { printf '[firstboot] %s\n' "$*"; }

[ -e "$MARK" ] && { log "already bootstrapped — nothing to do"; exit 0; }

log "running bootstrap.sh $ROLE full ..."
if ! "$SELF_DIR/bootstrap.sh" "$ROLE" full; then
    log "bootstrap FAILED — leaving the one-shot armed to retry on next boot"
    exit 1
fi

mkdir -p "$(dirname "$MARK")"
: > "$MARK"
systemctl disable "tailchaser-firstboot@$ROLE.service" 2>/dev/null || true

log "bootstrap complete — rebooting to apply WM8960 + UART firmware changes"
sleep 2
systemctl reboot
