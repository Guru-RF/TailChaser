#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS
# TailChaser — write a receiver.conf (tailchaser-receiver) from an existing SvxLink remotetrx setup.
#
#   migrate-remotetrx.sh [remotetrx.conf] [sa818-script] > receiver.conf
#
# Reads the uplink port and key, squelch line, sound device and DTMF muting
# from remotetrx.conf, and the channel (frequency, bandwidth, CTCSS, squelch,
# volume, filters) from the `sa818 ...` command lines of the script that
# programs the chip (the remotetrx unit's ExecStartPre; /usr/sbin/hotspot on a
# Guru-RF hotspot). Anything it cannot find is left at the default and
# listed on stderr.
#
# The output contains the uplink auth key: write it to a file only the service
# can read.
set -u
CONF="${1:-/etc/svxlink/remotetrx.conf}"
SCRIPT="${2:-}"
[ -r "$CONF" ] || { echo "cannot read $CONF" >&2; exit 1; }

if [ -z "$SCRIPT" ]; then
    # the script the remotetrx unit runs before starting, if it programs an SA818
    for s in $(systemctl cat remotetrx 2>/dev/null | sed -n 's/^ExecStartPre=-\{0,1\}\([^ ]*\).*/\1/p') \
             /usr/sbin/hotspot; do
        if [ -r "$s" ] && grep -q 'sa818' "$s" 2>/dev/null; then SCRIPT="$s"; break; fi
    done
fi

# first "KEY=value" in the remotetrx config, quotes and spaces stripped
cfg() { sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*//p" "$CONF" | head -n 1 | sed 's/[[:space:]]*$//; s/^"\(.*\)"$/\1/'; }
# value following "--opt" on an sa818 command line of the chip script
opt() { [ -n "$SCRIPT" ] && grep -v '^[[:space:]]*#' "$SCRIPT" | grep 'sa818' | sed -n "s/.*--$1[= ]\{1,\}\([^ ]*\).*/\1/p" | head -n 1; }
missing() { echo "migrate: $1 not found — left at the default" >&2; }
onoff() { case "$1" in enable|on|1|true|yes) echo 0;; disable|off|0|false|no) echo 1;; *) echo 0;; esac; }

PORT=$(cfg LISTEN_PORT);  [ -n "$PORT" ] || { PORT=5210; missing "LISTEN_PORT"; }
KEY=$(cfg AUTH_KEY);      [ -n "$KEY" ]  || missing "AUTH_KEY"

LINE=$(cfg SQL_GPIOD_LINE); [ -n "$LINE" ] || LINE=$(cfg GPIO_SQL_PIN)
ACTIVE_LOW=false
case "$LINE" in '!'*) ACTIVE_LOW=true; LINE=${LINE#!};; esac
LINE=$(printf '%s' "$LINE" | sed 's/^gpio//; s/^GPIO//')
case "$LINE" in ''|*[!0-9]*) LINE=12; ACTIVE_LOW=true; missing "the squelch GPIO line";; esac

DEV=$(cfg AUDIO_DEV); DEV=${DEV#alsa:}; [ -n "$DEV" ] || { DEV=plughw:0; missing "AUDIO_DEV"; }
CH=$(cfg AUDIO_CHANNEL); case "$CH" in 1) CH=right;; *) CH=left;; esac
PREAMP=$(cfg PREAMP); [ -n "$PREAMP" ] || PREAMP=0
MUTING=$(cfg DTMF_MUTING); case "$MUTING" in 0) MUTING=false;; *) MUTING=true;; esac

FREQ=$(opt frequency); FREQ=${FREQ%%,*}
BW=$(opt bw)
CTCSS=$(opt ctcss); CTCSS=${CTCSS##*,}          # "tx,rx" -> rx
SQL=$(opt squelch)
VOL=$(opt level)
EMPH=$(opt emphasis); HP=$(opt highpass); LP=$(opt lowpass)
[ -n "$SCRIPT" ] || echo "migrate: no sa818 chip script found — set [radio] by hand" >&2
[ -n "$FREQ" ] || missing "the frequency"
case "$BW" in 1) BW=wide;; *) BW=narrow;; esac
case "$CTCSS" in ''|None|none|0|0.0) CTCSS=none;; esac

cat <<EOF
# tailchaser-receiver configuration — migrated from $CONF${SCRIPT:+ and $SCRIPT}
# All settings and their meaning: /etc/tailchaser/receiver.conf.example

[uplink]
listen_port   = $PORT
auth_key      = $KEY
proto_minor   = auto

[radio]
${FREQ:+freq_rx       = $FREQ
}bandwidth     = $BW
ctcss_rx      = $CTCSS
squelch       = ${SQL:-1}
volume        = ${VOL:-1}
filter_emphasis = $(onoff "${EMPH:-enable}")
filter_highpass = $(onoff "${HP:-enable}")
filter_lowpass  = $(onoff "${LP:-enable}")
cos_line      = $LINE
cos_active_low= $ACTIVE_LOW

[gate]
mode          = on

[dtmf]
muting        = $MUTING

[audio]
alsa_device   = $DEV
audio_channel = $CH
capture_pct   = keep       # the sound card mixer stays as remotetrx had it
preamp_db     = $PREAMP

[status]
http_port     = 8080
EOF
