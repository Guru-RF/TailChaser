#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS
# TailChaser — set up a Raspberry Pi (Raspberry Pi OS / Debian Bookworm or
# Trixie) as one of the four TailChaser programs, then build + install it.
#
#   sudo ./bootstrap.sh <role> [mode]
#
#   role   simplex | duplex | receiver | transmitter
#
# Hardware: Pi + WM8960 Audio HAT + SA818 on /dev/serial0 (as SmartSPOT).
#
# On a new box:
#   sudo ./bootstrap.sh <role>                 # full: deps + build/install + HAT +
#                                              #   UART, then enable the service
#   sudo ./bootstrap.sh <role> deps-only       # deps + build/install only (no
#                                              #   firmware, no service enable)
#   sudo ./bootstrap.sh <role> prime-firstboot # stage /opt/tailchaser + arm a
#                                              #   one-shot that runs `full` on the
#                                              #   next boot (self-installing image)
#
# On a box that already runs one of TailChaser's predecessors:
#   sudo ./bootstrap.sh simplex  upgrade       # from svxsimplexd (SVXSimplexRepeater)
#   sudo ./bootstrap.sh receiver upgrade       # from svxrxd (SVXRepeaterRX)
#       Build + install, take the configuration (and the reflector key and
#       certificate) over as they are, stop the old daemon, start the new one.
#       If the new one does not come up, the old one is put back.
#
# On a box that runs SvxLink:
#   sudo ./bootstrap.sh simplex  replace-svxlink    # SimplexLogic + ReflectorLogic
#   sudo ./bootstrap.sh receiver replace-remotetrx  # remotetrx
#       The same, with the settings read from SvxLink's files. No firmware
#       changes, no reboot.
#
#   sudo ./bootstrap.sh <role> rollback        # back to what ran before
#
# On a box that already runs this role, every mode but rollback is an update:
# rebuild, install, check the configuration, restart.
#
# Environment:
#   SVXLINK_CONF=FILE / REMOTETRX_CONF=FILE   SvxLink's configuration
#   SA818_SCRIPT=FILE   the script with the `sa818 ... radio` lines
#   MIXER_SCRIPT=FILE   the script with the `amixer set` lines
#   LOGIN_WAIT=SECONDS  how long to wait for the reflector login (default 60)
#   IDLE_WAIT=SECONDS   how long to wait for a node in use to fall silent before
#                       it is restarted (default 300; 0 = do not wait)
#   KEEP=1              keep the new daemon even when it has not logged in in time
set -u

ROLE="${1:-}"
MODE="${2:-full}"
SERVICE_USER="tailchaser"
OPT_DIR="/opt/tailchaser"
CONF_DIR="/etc/tailchaser"
STATE_DIR="/var/lib/tailchaser"

run()  { printf '\033[38;5;104m --> %s\033[39m\n' "$*"; eval "$@"; }
say()  { printf '\033[38;5;220m%s\033[38;5;255m\n' "$*"; }
warn() { printf '\033[38;5;208m[warn] %s\033[39m\n' "$*"; }
die()  { printf '\033[38;5;196m[err] %s\033[39m\n' "$*" >&2; exit 1; }

USAGE="sudo ./bootstrap.sh <simplex|duplex|receiver|transmitter> [full|deps-only|prime-firstboot|upgrade|replace-svxlink|replace-remotetrx|rollback]"
[ "$(id -u)" -eq 0 ] || die "run as root: $USAGE"
case "$ROLE" in simplex|duplex|receiver|transmitter) ;; *) die "which program? $USAGE";; esac
case "$MODE" in full|deps-only|prime-firstboot|upgrade|replace-svxlink|replace-remotetrx|rollback) ;; *) die "unknown mode '$MODE' — $USAGE";; esac

PROG="tailchaser-$ROLE"
SERVICE="$PROG.service"
CONF="$CONF_DIR/$ROLE.conf"
DESTBIN="/usr/local/sbin/$PROG"
# what ran this box before, for the way back: "enabled <unit>" / "active <unit>" lines
REPLACED="$STATE_DIR/.replaced-by-$ROLE"

# What can be taken over, per role and mode. LEGACY_UNITS are stopped in this
# order and started again in the reverse one.
LEGACY_UNITS=""; OLD_CONF=""; OLD_STATE=""; MASK=""
case "$ROLE:$MODE" in
    simplex:upgrade)            LEGACY_UNITS="svxsimplexd.service"; OLD_CONF=/etc/svxsimplexd/svxsimplexd.conf; OLD_STATE=/var/lib/svxsimplexd ;;
    receiver:upgrade)           LEGACY_UNITS="svxrxd.service";      OLD_CONF=/etc/svxrxd/svxrxd.conf ;;
    simplex:replace-svxlink)    LEGACY_UNITS="svxlink-watchdog.service svxlink.service"; MASK=svxlink.service ;;
    receiver:replace-remotetrx) LEGACY_UNITS="remotetrx.service" ;;
    *:upgrade|*:replace-svxlink|*:replace-remotetrx)
        die "'$MODE' does not exist for $PROG — set it up with 'full' or 'deps-only' and write $CONF by hand" ;;
esac
TAKEOVER=no; [ -z "$LEGACY_UNITS" ] || TAKEOVER=yes
# the unit that has to be there for a take-over to make sense
MAIN_LEGACY="${LEGACY_UNITS##* }"

# (a masked unit exists too: `systemctl cat` does not say so on every systemd)
unit_exists() {
    systemctl cat "$1" >/dev/null 2>&1 ||
        [ -n "$(systemctl list-unit-files --no-legend "$1" 2>/dev/null)" ]
}

# ---------------------------------------------------------------------------
# Back to what ran before: this program off, the old units as they were.
# Nothing is uninstalled; the settings and the certificate copy stay.
restore_legacy() {
    local u main="" want_running=yes
    [ -f "$REPLACED" ] || return 1
    # The record lies in the daemon's directory: nothing but unit names is
    # taken from it, by root.
    if grep -qvE '^(unit|enabled|active|masked) [A-Za-z0-9@._-]+\.service$' "$REPLACED"; then
        die "$REPLACED holds something that is not a unit name — not touched; restore by hand"
    fi
    run "systemctl disable --now $SERVICE"         # unkeys: ExecStopPost
    for u in $(sed -n 's/^unit //p' "$REPLACED"); do main="$u"; done
    for u in $(sed -n 's/^masked //p' "$REPLACED"); do run "systemctl unmask $u"; done
    grep -qx "active $main" "$REPLACED" || want_running=no
    for u in $(sed -n 's/^unit //p' "$REPLACED" | tac); do
        unit_exists "$u" || continue
        ! grep -qx "enabled $u" "$REPLACED" || run "systemctl enable $u"
        ! grep -qx "active $u" "$REPLACED"  || run "systemctl start $u"
    done
    rm -f "$REPLACED"
    [ "$want_running" = no ] || systemctl is-active --quiet "$main"
}

if [ "$MODE" = "rollback" ]; then
    [ -f "$REPLACED" ] || die "nothing is recorded that $PROG replaced on this box"
    say "Rolling back: $PROG off, $(sed -n 's/^unit //p' "$REPLACED" | tr '\n' ' ')back on"
    restore_legacy || die "the old service did not start — see: journalctl -n 50"
    say "Done. $PROG is installed but disabled."
    exit 0
fi

REPO_DIR="$(cd "$(dirname "$0")" && pwd)"

# Firmware config locations (current images use /boot/firmware).
CONFIG_TXT=/boot/firmware/config.txt; [ -f "$CONFIG_TXT" ] || CONFIG_TXT=/boot/config.txt
CMDLINE=/boot/firmware/cmdline.txt;   [ -f "$CMDLINE"   ] || CMDLINE=/boot/cmdline.txt

# ---------------------------------------------------------------------------
# prime-firstboot: stage the repo at /opt/tailchaser and arm the one-shot that
# runs `bootstrap.sh <role> full` on the next boot. Run this once when
# preparing a golden SD image; do NOT do the heavy work now.
if [ "$MODE" = "prime-firstboot" ]; then
    say "Priming first-boot self-install of $PROG"
    if [ "$REPO_DIR" != "$OPT_DIR" ]; then
        run "mkdir -p '$OPT_DIR'"
        run "cp -a '$REPO_DIR/.' '$OPT_DIR/'"
        run "rm -rf '$OPT_DIR/.git'"
    fi
    run "chmod +x '$OPT_DIR/bootstrap.sh' '$OPT_DIR/firstboot.sh'"
    run "install -Dm644 '$OPT_DIR/systemd/tailchaser-firstboot@.service' \
            /etc/systemd/system/tailchaser-firstboot@.service"
    run "systemctl daemon-reload"
    run "systemctl enable tailchaser-firstboot@$ROLE.service"
    say "Primed: $OPT_DIR staged, first-boot one-shot enabled."
    say "Image this card (or reboot) — first boot self-installs, then reboots."
    exit 0
fi

ALREADY=no
systemctl is-active --quiet "$SERVICE" && ALREADY=yes
if [ "$TAKEOVER" = yes ] && [ "$ALREADY" = no ]; then
    unit_exists "$MAIN_LEGACY" || die "there is no $MAIN_LEGACY on this box — use 'full' for a new one"
    [ -z "$OLD_CONF" ] || [ -r "$OLD_CONF" ] || die "$OLD_CONF not found — nothing to take over"
fi

# ---------------------------------------------------------------------------
say "[1/6] Build dependencies"
have_deps() {
    command -v cc >/dev/null && command -v make >/dev/null && command -v pkg-config >/dev/null &&
        pkg-config --exists libgpiod alsa opus openssl
}
if have_deps; then
    say "already installed"           # a node that built SvxLink has them all: no apt run needed
else
    run "apt-get update -qq"
    run "apt-get install -y build-essential pkg-config git \
            libasound2-dev libopus-dev libgpiod-dev libssl-dev"
    have_deps || die "build dependencies are missing"
fi

# ---------------------------------------------------------------------------
say "[2/6] Service user '$SERVICE_USER'"
id "$SERVICE_USER" >/dev/null 2>&1 || \
    run "useradd --system --no-create-home --shell /usr/sbin/nologin $SERVICE_USER"
getent group gpio >/dev/null || run "groupadd --system gpio"
run "usermod -aG audio,gpio,dialout $SERVICE_USER"

# ---------------------------------------------------------------------------
say "[3/6] Building + installing $PROG"
run "make -C '$REPO_DIR' clean"
run "make -C '$REPO_DIR' $PROG" || die "build failed"
run "make -C '$REPO_DIR' install ROLE=$ROLE"
run "install -d -m 700 -o '$SERVICE_USER' -g '$SERVICE_USER' '$STATE_DIR'"

# ---------------------------------------------------------------------------
# Is the program up, and does it stay up? Then: is it connected — logged in on
# the reflector, or (a receiver) simply still running?
# 0 = yes; 1 = it does not run; 2 = it runs but has not logged in.
came_up() {
    local since="$1" wait="${LOGIN_WAIT:-60}" i
    sleep 5
    systemctl is-active --quiet "$SERVICE" || return 1
    [ "$ROLE" != receiver ] || return 0
    say "$PROG is running — waiting for the reflector login (up to $wait s)"
    for i in $(seq 1 "$wait"); do
        if journalctl -u "$SERVICE" --since "$since" --no-pager 2>/dev/null | grep -q 'reflector: logged in as'; then
            systemctl is-active --quiet "$SERVICE" || return 1
            return 0
        fi
        systemctl is-active --quiet "$SERVICE" || return 1
        sleep 1
    done
    return 2
}

# ---------------------------------------------------------------------------
# A node that is on the air is not cut off in the middle of an over, and not
# in the last minute before the hour either (it identifies on the hour). Asks
# the status page of whatever runs the box now; waits up to IDLE_WAIT seconds.
# Without a status page (SvxLink) there is nothing to ask.
wait_idle() {
    local conf="$1" max="${IDLE_WAIT:-300}" port i=0
    [ "$max" != 0 ] || return 0
    command -v python3 >/dev/null || return 0
    port=$(sed -n '/^\[status\]/,/^\[/s/^[[:space:]]*http_port[[:space:]]*=[[:space:]]*\([0-9]*\).*/\1/p' "$conf" 2>/dev/null | head -n 1)
    [ -n "$port" ] || port=8080
    [ "$port" != 0 ] || return 0
    while [ "$i" -lt "$max" ]; do
        python3 - "$port" <<'PY'
import json, sys, time, urllib.request
try:
    d = json.load(urllib.request.urlopen("http://127.0.0.1:%s/status.json" % sys.argv[1], timeout=3))
except Exception:
    sys.exit(0)                 # nothing to ask: go ahead
busy = (d.get("gate", {}).get("cos") or d.get("gate", {}).get("open") or d.get("tx", {}).get("keyed")
        or d.get("link", {}).get("talker") or d.get("receiver", {}).get("squelch"))
sys.exit(1 if busy or time.localtime().tm_min == 59 else 0)
PY
        [ $? -ne 0 ] || return 0
        [ "$i" -ne 0 ] || say "the node is in use (or the hour is about to strike) — waiting for a quiet moment"
        sleep 2; i=$((i + 2))
    done
    die "the node was not idle for $max s — nothing was stopped (IDLE_WAIT=seconds to wait longer, IDLE_WAIT=0 not to wait)"
}

# ---------------------------------------------------------------------------
# This box already runs this program: the run was a rebuild. Restart on it.
if [ "$ALREADY" = yes ]; then
    say "$PROG already runs this box — restarting it on the new build"
    run "$DESTBIN -t -c '$CONF'" || die "the configuration is not usable — $PROG was NOT restarted"
    run "systemctl daemon-reload"
    wait_idle "$CONF"
    SINCE="$(date '+%Y-%m-%d %H:%M:%S')"
    run "systemctl restart $SERVICE"
    came_up "$SINCE"
    case $? in
        0) say "$PROG is running."; exit 0 ;;
        1) journalctl -u "$SERVICE" -n 25 --no-pager || true
           die "$PROG does not stay up — see the log above" ;;
        *) journalctl -u "$SERVICE" -n 25 --no-pager || true
           die "$PROG runs but has not logged in on the reflector — see the log above" ;;
    esac
fi

# ---------------------------------------------------------------------------
# Take over from what runs this box now, in place.
if [ "$TAKEOVER" = yes ]; then
    say "[4/6] Taking the settings over"
    run "install -d -m 755 '$CONF_DIR'"
    if [ -f "$CONF" ]; then
        say "$CONF exists — keeping it"
    else
        # The config can carry a password or the link key: root + the service only.
        case "$MODE" in
        upgrade)
            # The same settings under a new name; only the key directory moved.
            ( umask 027; sed "s#/var/lib/svxsimplexd/pki#$STATE_DIR/pki#" "$OLD_CONF" > "$CONF.new" ) \
                || { rm -f "$CONF.new"; die "could not read $OLD_CONF — nothing was touched"; }
            if [ -n "$OLD_STATE" ] && [ -d "$OLD_STATE/pki" ] && [ ! -e "$STATE_DIR/pki" ]; then
                run "cp -a '$OLD_STATE/pki' '$STATE_DIR/pki'"      # a copy: the way back stays open
            fi ;;
        replace-svxlink)
            ( umask 027
              sh "$REPO_DIR/tools/migrate-svxlink.sh" ${SVXLINK_CONF:+-c "$SVXLINK_CONF"} \
                  ${SA818_SCRIPT:+-s "$SA818_SCRIPT"} ${MIXER_SCRIPT:+-m "$MIXER_SCRIPT"} \
                  -p "$STATE_DIR/pki" > "$CONF.new" ) \
                || { rm -f "$CONF.new"; die "could not take the settings over — SvxLink was NOT touched"; } ;;
        replace-remotetrx)
            ( umask 027
              sh "$REPO_DIR/tools/migrate-remotetrx.sh" "${REMOTETRX_CONF:-/etc/svxlink/remotetrx.conf}" \
                  ${SA818_SCRIPT:+"$SA818_SCRIPT"} > "$CONF.new" ) \
                || { rm -f "$CONF.new"; die "could not take the settings over — remotetrx was NOT touched"; } ;;
        esac
        run "mv '$CONF.new' '$CONF'"
        run "chown root:'$SERVICE_USER' '$CONF' && chmod 640 '$CONF'"
        say "Settings taken over into $CONF"
    fi
    # the copied key and certificate belong to the service
    run "chown -hR '$SERVICE_USER':'$SERVICE_USER' '$STATE_DIR'"
    run "$DESTBIN -t -c '$CONF'" || die "the configuration that was taken over is not usable — nothing was stopped"
    run "systemctl daemon-reload"

    [ -z "$OLD_CONF" ] || wait_idle "$OLD_CONF"
    say "[5/6] Stopping $(echo $LEGACY_UNITS)"
    # Remember what was enabled and running, for the way back. (Not overwritten
    # by a second attempt, when the old setup is already half taken apart.)
    if [ ! -f "$REPLACED" ]; then
        for u in $LEGACY_UNITS; do
            unit_exists "$u" || continue
            echo "unit $u"
            systemctl is-enabled --quiet "$u" 2>/dev/null && echo "enabled $u"
            systemctl is-active  --quiet "$u" 2>/dev/null && echo "active $u"
        done > "$REPLACED"
        [ -z "$MASK" ] || echo "masked $MASK" >> "$REPLACED"
    fi
    # The old program holds the sound card, the GPIO lines and the SA818: it has
    # to go first. From here until the new one is up the node is off the air.
    # (A watchdog unit goes first — it would start its daemon again.)
    for u in $LEGACY_UNITS; do
        unit_exists "$u" && run "systemctl disable --now $u"
    done
    # Nothing may start SvxLink behind our back (the hotspot image's menus do
    # "systemctl restart svxlink"): two programs on one radio.
    [ -z "$MASK" ] || run "systemctl mask $MASK" || warn "could not mask $MASK"

    say "[6/6] Starting $PROG"
    SINCE="$(date '+%Y-%m-%d %H:%M:%S')"
    run "systemctl enable $SERVICE"
    run "systemctl start $SERVICE"
    came_up "$SINCE"
    RESULT=$?
    if [ "$RESULT" -eq 2 ] && [ "${KEEP:-0}" = 1 ]; then
        warn "$PROG runs but has not logged in on the reflector yet — kept, as asked (KEEP=1)"
        RESULT=0
    fi
    if [ "$RESULT" -eq 0 ]; then
        say "$PROG runs this box now."
        say "  watch:    journalctl -u $PROG -f     or  http://<this box>:8080/"
        say "  settings: $CONF   (the old program's files no longer apply)"
        say "  go back:  sudo $REPO_DIR/bootstrap.sh $ROLE rollback"
        exit 0
    fi
    if [ "$RESULT" -eq 1 ]; then
        warn "$PROG did not stay up:"
    else
        warn "$PROG runs but did not log in on the reflector within ${LOGIN_WAIT:-60} s:"
    fi
    journalctl -u "$SERVICE" --since "$SINCE" -n 40 --no-pager || true
    restore_legacy || die "rolled back, but the old service did not start — see: journalctl -n 50"
    die "rolled back: the old service is running again"
fi

# Seed a live config from the example if the operator hasn't created one yet.
# The example is deliberately incomplete — no callsign, no frequency: a box
# that was not set up does not start, and does not transmit.
if [ ! -f "$CONF" ]; then
    run "install -m640 -g '$SERVICE_USER' '$CONF_DIR/$ROLE.conf.example' '$CONF'"
fi

if [ "$MODE" = "deps-only" ]; then
    say "deps-only: skipping WM8960 / audio / UART firmware changes."
    say "Done. Edit $CONF, then: sudo systemctl enable --now $SERVICE"
    exit 0
fi

# ---------------------------------------------------------------------------
# Patch the Waveshare WM8960 source for ASoC API drift on newer kernels. It
# introspects the RUNNING kernel's headers, so older kernels are left untouched.
wm8960_asoc_compat_patch() {
    local dir="$1" snd
    # The ASoC DAI headers live in the kernel-headers tree; on split-package RPi
    # OS (e.g. Trixie) they're in the -common package, NOT the arch one — and
    # /lib/modules/<rel>/build symlinks to the arch package, which has no
    # include/sound. Locate them wherever they actually are (by the file that
    # defines SND_SOC_DAIFMT_MASTER_MASK), so the version guards below don't
    # silently skip and leave the source uncompilable on a newer kernel.
    snd=$(dirname "$(grep -rlsm1 SND_SOC_DAIFMT_MASTER_MASK \
            /usr/src/linux-headers-*/include/sound \
            "/lib/modules/$(uname -r)/build/include/sound" 2>/dev/null | head -1)")
    [ -d "$snd" ] || snd="/usr/src/linux-headers-$(uname -r)/include/sound"
    # DAIFMT clock-provider macros renamed (~6.0): CBM_CFM -> CBP_CFP, etc.
    if grep -rqs "SND_SOC_DAIFMT_CBP_CFP" "$snd"; then
        sed -i -e "s/SND_SOC_DAIFMT_CBM_CFM/SND_SOC_DAIFMT_CBP_CFP/g" \
               -e "s/SND_SOC_DAIFMT_CBS_CFS/SND_SOC_DAIFMT_CBC_CFC/g" \
               -e "s/SND_SOC_DAIFMT_CBM_CFS/SND_SOC_DAIFMT_CBP_CFC/g" \
               -e "s/SND_SOC_DAIFMT_CBS_CFM/SND_SOC_DAIFMT_CBC_CFP/g" \
               "$dir/wm8960.c" "$dir/wm8960-soundcard.c" 2>/dev/null || true
    fi
    # simple_util_*() first arg became struct simple_util_priv* on newer kernels.
    if grep -A2 -s "simple_util_set_dailink_name" "$snd/simple_card_utils.h" | grep -qs "simple_util_priv"; then
        sed -i -e "s/simple_util_set_dailink_name(dev,/simple_util_set_dailink_name(priv,/g" \
               -e "s/simple_util_parse_card_name(card,/simple_util_parse_card_name(priv,/g" \
               "$dir/wm8960-soundcard.c" 2>/dev/null || true
    fi
    # snd_soc_dai_link_set_capabilities() removed from the API -> drop the call.
    if ! grep -rqs "snd_soc_dai_link_set_capabilities" "$snd"; then
        sed -i "/snd_soc_dai_link_set_capabilities(dai_link);/d" "$dir/wm8960-soundcard.c" 2>/dev/null || true
    fi
}

# ---------------------------------------------------------------------------
say "[4/6] Installing WM8960 Audio HAT driver"
# Kernel headers for the DKMS build (the Waveshare installer asks for the wrong
# package name on Trixie, so make sure the right one is present first).
run "apt-get install -y git linux-headers-$(uname -r) || apt-get install -y git linux-headers-rpi-v8 || true"
if lsmod | grep -q snd_soc_wm8960_soundcard; then
    say "WM8960 (Waveshare) driver already loaded — skipping"
else
    TMP="$(mktemp -d)"
    # A known state of somebody else's installer, not whatever is there today:
    # it runs as root. WM8960_REF=<commit> in the environment takes another.
    WM8960_REF="${WM8960_REF:-d4915ac97730e78e43f985dabcf3903e85eb8b0a}"
    if run "git clone https://github.com/waveshare/WM8960-Audio-HAT '$TMP/wm8960' && git -C '$TMP/wm8960' checkout -q --detach '$WM8960_REF'"; then
        wm8960_asoc_compat_patch "$TMP/wm8960"   # kernel-ASoC compat (no-op on old kernels)
        ( cd "$TMP/wm8960" && run "./install.sh" ) \
            || warn "WM8960 install.sh failed — see the DKMS make.log"
    else
        warn "Could not clone the WM8960 driver"
    fi
    rm -rf "$TMP"
fi

# ---------------------------------------------------------------------------
say "[5/6] Audio + UART"
# Make the WM8960 the capture/playback card: turn off onboard + HDMI audio.
run "perl -i -pe 's/^dtparam=audio=on/dtparam=audio=off/g' '$CONFIG_TXT'"
grep -q 'vc4-kms-v3d,noaudio' "$CONFIG_TXT" || \
    run "perl -i -pe 's/dtoverlay=vc4-kms-v3d\b/dtoverlay=vc4-kms-v3d,noaudio/g' '$CONFIG_TXT'"
# Free the UART for the SA818 (GPIO14/15 -> /dev/serial0). Do NOT disable-bt.
grep -q '^enable_uart=1' "$CONFIG_TXT" || run "echo 'enable_uart=1' >> '$CONFIG_TXT'"
run "perl -i -pe 's/\s*console=serial0,[0-9]+//g' '$CMDLINE'"
run "systemctl disable serial-getty@ttyS0.service 2>/dev/null || true"

# ---------------------------------------------------------------------------
say "[6/6] Enabling the $PROG service"
MODEL="$(tr -d '\0' < /proc/device-tree/model 2>/dev/null || true)"
say "Detected board: ${MODEL:-unknown}"
run "systemctl daemon-reload"
# enable (not --now): the WM8960 + UART changes need a reboot first.
run "systemctl enable $SERVICE"

cat <<EOF

$(say "Bootstrap complete — the $PROG service is ENABLED (starts on boot).")
Before it goes on the air:
  1. sudo nano $CONF
       fill in the lines marked REQUIRED (they are listed at the top of the file)
     check it:  sudo $PROG -t -c $CONF
     Until these are set the service does not start.
  2. sudo reboot                  # applies the WM8960 + UART firmware changes
  3. (not a receiver) first connection: the node sends a certificate request to
     the reflector; its sysop signs it, then the node logs in by itself.
  4. watch it:  journalctl -u $PROG -f     or  http://<this box>:8080/
EOF
