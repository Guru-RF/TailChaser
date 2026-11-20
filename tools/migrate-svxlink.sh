#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS
# TailChaser — write an simplex.conf from an existing SvxLink
# simplex node (a Simplex logic linked to a Reflector logic, SA818 programmed
# by a script, as on the Guru-RF hotspot images).
#
#   migrate-svxlink.sh [-c svxlink.conf] [-s sa818-script] [-m mixer-script]
#                      [-p pki-destination] > simplex.conf
#
#   -c  SvxLink's configuration   (default: CFGFILE of /etc/default/svxlink,
#                                  else /etc/svxlink/svxlink.conf)
#   -s  the script with the `sa818 ... radio ...` lines that program the chip
#       (default: found among the svxlink unit's ExecStartPre scripts,
#        else /usr/sbin/hotspot)
#   -m  the script with the `amixer set 'Headphone' N%` lines
#       (default: found the same way, else /usr/sbin/hotspot_volume)
#   -p  also copy the node's reflector key, certificate, signing request and
#       CA bundle into this directory (needs root). Without it the node asks
#       the reflector for a new certificate, which its sysop has to sign.
#
# Reads; changes nothing of SvxLink's. What it cannot find is left at the
# tailchaser-simplex default and said on stderr. The output can hold the reflector
# password (AUTH_KEY): write it to a file only the service can read.
set -u

note() { echo "migrate: $*" >&2; }
die()  { echo "migrate: $*" >&2; exit 1; }
usage() { sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

CONF=""; SCRIPT=""; MIXER=""; PKI_DEST=""
while getopts "c:s:m:p:h" o; do
    case "$o" in
        c) CONF=$OPTARG ;;
        s) SCRIPT=$OPTARG ;;
        m) MIXER=$OPTARG ;;
        p) PKI_DEST=$OPTARG ;;
        h) usage 0 ;;
        *) usage 2 ;;
    esac
done

if [ -z "$CONF" ]; then
    [ -r /etc/default/svxlink ] &&
        CONF=$(sed -n 's/^[[:space:]]*CFGFILE=//p' /etc/default/svxlink | tail -n 1 | tr -d '"')
    [ -n "$CONF" ] || CONF=/etc/svxlink/svxlink.conf
fi
[ -r "$CONF" ] || die "cannot read $CONF — is this an SvxLink node? (-c FILE)"
CONF_DIR=$(dirname "$CONF")

# ---------------------------------------------------------------------------
# SvxLink's configuration: the main file plus the files in CFG_DIR

ALL=$(cat "$CONF")
CFG_D=$(printf '%s\n' "$ALL" | sed -n 's/^[[:space:]]*CFG_DIR[[:space:]]*=[[:space:]]*//p' | head -n 1 | tr -d '"\r')
case "$CFG_D" in ''|/*) ;; *) CFG_D="$CONF_DIR/$CFG_D" ;; esac
if [ -n "$CFG_D" ] && [ -d "$CFG_D" ]; then
    for f in "$CFG_D"/*.conf; do
        [ -r "$f" ] && ALL="$ALL
$(cat "$f")"
    done
fi

# value of KEY in [SECTION]: first match, surrounding quotes and spaces removed
cfg() {
    printf '%s\n' "$ALL" | awk -v sec="$1" -v key="$2" '
        { sub(/\r$/, "") }
        /^[ \t]*\[/ { s = $0; sub(/^[ \t]*\[/, "", s); sub(/\].*$/, "", s); next }
        s == sec && /^[ \t]*[A-Za-z0-9_]+[ \t]*=/ {
            k = $0; sub(/[ \t]*=.*$/, "", k); sub(/^[ \t]*/, "", k)
            if (k == key) {
                v = $0; sub(/^[^=]*=[ \t]*/, "", v); sub(/[ \t]*$/, "", v)
                if (v ~ /^".*"$/) v = substr(v, 2, length(v) - 2)
                print v; exit
            }
        }'
}
int() { case "$1" in ''|-|*[!0-9-]*|?*-*) echo "$2" ;; *) echo "$1" ;; esac; }
num() { case "$1" in ''|*[!0-9.-]*|?*-*) echo "$2" ;; *) echo "$1" ;; esac; }
quoted() { printf '"%s"' "$(printf '%s' "$1" | tr -d '"')"; }
upper() { printf '%s' "$1" | tr '[:lower:]' '[:upper:]'; }

# ---------------------------------------------------------------------------
# which logics: a Simplex one and a Reflector one, as listed in LOGICS

SIMPLEX=""; REFL=""; OTHER=""
for l in $(cfg GLOBAL LOGICS | tr ',' ' '); do
    case "$(cfg "$l" TYPE)" in
        Simplex)   [ -n "$SIMPLEX" ] || SIMPLEX=$l ;;
        Reflector) [ -n "$REFL" ]    || REFL=$l ;;
        *)         OTHER="$OTHER $l" ;;
    esac
done
[ -n "$SIMPLEX" ] || die "no Simplex logic in LOGICS ($(cfg GLOBAL LOGICS)) — this is not a simplex node"
[ -n "$REFL" ]    || die "no Reflector logic in LOGICS ($(cfg GLOBAL LOGICS)) — the node is not on an SvxReflector"
[ -z "$OTHER" ]   || note "logics not taken over:$OTHER"

RX=$(cfg "$SIMPLEX" RX); TX=$(cfg "$SIMPLEX" TX)
[ -n "$RX" ] && [ -n "$TX" ] || die "[$SIMPLEX] has no RX / TX"
[ "$(cfg "$RX" TYPE)" = "Local" ] || note "[$RX] TYPE=$(cfg "$RX" TYPE) is not a local receiver — check [audio] and [radio]"

# the DTMF prefix that reaches the reflector logic: the link command of the
# simplex logic, then "1" (SvxLink: 9 1 <talkgroup> #)
PFX=""; LINKED=""
for l in $(cfg GLOBAL LINKS | tr ',' ' '); do
    CL=$(cfg "$l" CONNECT_LOGICS)
    case ",$CL" in *",$REFL:"*|*",$REFL,"*|*",$REFL") ;; *) continue ;; esac
    for item in $(printf '%s' "$CL" | tr ',' ' '); do
        case "$item" in
            "$SIMPLEX":*) LINKED=$l; rest=${item#*:}; PFX=${rest%%:*} ;;
            "$SIMPLEX")   LINKED=$l ;;
        esac
    done
done
[ -n "$LINKED" ] || note "no link connects [$SIMPLEX] to [$REFL] in SvxLink — tailchaser-simplex always connects them"
case "$PFX" in ''|*[!0-9A-D*]*) PFX="" ;; esac

# ---------------------------------------------------------------------------
# the scripts SvxLink's unit runs before it starts: chip and mixer

unit_scripts() {
    systemctl cat svxlink.service 2>/dev/null |
        sed -n 's/^ExecStartPre=[-@:+!]*\([^[:space:]]*\).*/\1/p'
}
find_script() {         # $1 = what one of its command lines looks like
    for s in $(unit_scripts) /usr/sbin/hotspot /usr/sbin/hotspot_volume; do
        [ -r "$s" ] && [ "$(head -c 2 "$s" 2>/dev/null)" = "#!" ] || continue
        if grep -v '^[[:space:]]*#' "$s" | grep -Eq "$1"; then echo "$s"; return; fi
    done
}
[ -n "$SCRIPT" ] || SCRIPT=$(find_script '(^|[[:space:]/])sa818[[:space:]].*[[:space:]]radio[[:space:]]')
[ -n "$MIXER" ]  || MIXER=$(find_script '(^|[[:space:]/])amixer[[:space:]].*(Headphone|Capture)')
[ -z "$SCRIPT" ] || [ -r "$SCRIPT" ] || die "cannot read $SCRIPT"
[ -z "$MIXER" ]  || [ -r "$MIXER" ]  || die "cannot read $MIXER"

# value following "--name" on an sa818 command line of the chip script
opt() {
    [ -n "$SCRIPT" ] || return 0
    grep -v '^[[:space:]]*#' "$SCRIPT" | grep -E '(^|[[:space:]/])sa818[[:space:]]' |
        sed -n "s/.*--$1[= ]\{1,\}\([^ ]*\).*/\1/p" | head -n 1
}
# N of the last "amixer set 'NAME' N%" in the mixer script
mixpct() {
    [ -n "$MIXER" ] || return 0
    grep -v '^[[:space:]]*#' "$MIXER" |
        sed -n "s/.*amixer.*[[:space:]]['\"]\{0,1\}$1['\"]\{0,1\}[[:space:]]\{1,\}\([0-9]\{1,3\}\)%.*/\1/p" | tail -n 1
}
filter() { case "$1" in disable|Disable|off|1) echo 1 ;; *) echo 0 ;; esac; }
tone() { case "$1" in ''|None|none|0|0.0|00|0000) echo none ;; *) echo "$1" ;; esac; }

# ---------------------------------------------------------------------------
# [radio]

PORT=$(opt port)
FREQ=$(opt frequency); FREQ=${FREQ%%,*}
OFFSET=$(num "$(opt offset)" 0)
BW=$(opt bw)
CTCSS=$(opt ctcss)
SQL=$(int "$(opt squelch)" "")
VOL=$(int "$(opt level)" "")
[ -n "$SCRIPT" ] || note "no script with 'sa818 ... radio' lines found — set [radio] by hand (-s FILE)"
if [ -n "$SCRIPT" ]; then
    # what the sa818 tool does when an option is not given
    [ -n "$BW" ]  || BW=1
    [ -n "$SQL" ] || SQL=4
    [ -n "$VOL" ] || VOL=4
    [ -z "$(opt dcs)" ]  || note "the chip is programmed with a DCS code ($(opt dcs)) — tailchaser-simplex only does CTCSS: set ctcss_rx / ctcss_tx by hand"
    [ -z "$(opt tail)" ] || note "sa818 --tail is not taken over"
fi
case "$BW" in 1) BW=wide ;; *) BW=narrow ;; esac
case "$CTCSS" in                    # the sa818 tool: "tx,rx", or one tone for both
    *,*) CTCSS_TX=$(tone "${CTCSS%%,*}"); CTCSS_RX=$(tone "${CTCSS##*,}") ;;
    *)   CTCSS_TX=$(tone "$CTCSS");       CTCSS_RX=$CTCSS_TX ;;
esac
FREQ=$(num "$FREQ" "")
FREQ_TX=""
if [ -n "$FREQ" ] && [ "$OFFSET" != 0 ] && [ "$OFFSET" != 0.0 ]; then
    FREQ_TX=$(awk "BEGIN { printf \"%.4f\", $FREQ + $OFFSET }")
    [ "$FREQ_TX" != "$(awk "BEGIN { printf \"%.4f\", $FREQ }")" ] || FREQ_TX=""
fi
[ -n "$FREQ" ] || note "the frequency was not found — set [radio] freq by hand, the node does not start without it"

gpio_line() {           # "!12", "gpio12", "!gpio12" -> "12 true"
    v=$1; low=false
    case "$v" in '!'*) low=true; v=${v#!} ;; esac
    v=$(printf '%s' "$v" | sed 's/^[Gg][Pp][Ii][Oo]//')
    case "$v" in ''|*[!0-9]*) return 1 ;; esac
    echo "$v $low"
}
COS=$(cfg "$RX" SQL_GPIOD_LINE); [ -n "$COS" ] || COS=$(cfg "$RX" GPIO_SQL_PIN)
COS=$(gpio_line "$COS") || { COS="12 true"; note "the squelch GPIO line was not found ([$RX] SQL_DET=$(cfg "$RX" SQL_DET)) — left at GPIO12, active low"; }
PTT=$(cfg "$TX" PTT_GPIOD_LINE); [ -n "$PTT" ] || PTT=$(cfg "$TX" PTT_PIN)
PTT=$(gpio_line "$PTT") || { PTT="16 true"; note "the PTT GPIO line was not found ([$TX] PTT_TYPE=$(cfg "$TX" PTT_TYPE)) — left at GPIO16, active low"; }

# ---------------------------------------------------------------------------
# [audio]

DEV=$(cfg "$RX" AUDIO_DEV); DEV=${DEV#alsa:}
[ -n "$DEV" ] || { DEV=plughw:0; note "[$RX] AUDIO_DEV not found — left at plughw:0"; }
TXDEV=$(cfg "$TX" AUDIO_DEV); TXDEV=${TXDEV#alsa:}
[ -z "$TXDEV" ] || [ "$TXDEV" = "$DEV" ] ||
    note "[$TX] transmits on another sound device ($TXDEV) than [$RX] receives on ($DEV) — tailchaser-simplex uses one device"
case "$(cfg "$RX" AUDIO_CHANNEL)" in 1) CH=right ;; *) CH=left ;; esac
case "$(cfg "$TX" AUDIO_CHANNEL)" in 1) TXCH=right ;; *) TXCH=left ;; esac
PREAMP=$(num "$(cfg "$RX" PREAMP)" 0)
TXGAIN=$(num "$(cfg "$TX" MASTER_GAIN)" 0)
PLAY=$(mixpct Headphone); CAP=$(mixpct Capture)
if [ -z "$PLAY" ] || [ -z "$CAP" ]; then
    note "no 'amixer set Headphone/Capture N%' script found — the sound card mixer is left as it is"
    PLAY=${PLAY:-keep}; CAP=${CAP:-keep}
fi

# ---------------------------------------------------------------------------
# [tx], [ident]

DELAY=$(int "$(cfg "$TX" TX_DELAY)" 0)
HANG=$(int "$(cfg "$TX" PTT_HANGTIME)" 0)
# SvxLink's own limit, and the limit of the GPIO watchdog that restarts SvxLink
# when the PTT line stays keyed (hotspot images): the lower one counts.
WATCHDOG=${SVXLINK_WATCHDOG:-/usr/sbin/svxlink-watchdog}
LIMIT=""
[ -r "$WATCHDOG" ] && LIMIT=$(sed -n 's/^LOW_TIME_REQUIRED=\([0-9][0-9]*\).*/\1/p' "$WATCHDOG" | head -n 1)
LIMIT=${LIMIT:-360}
TOUT=$(int "$(cfg "$TX" TIMEOUT)" 0)
if [ "$TOUT" -le 0 ] || [ "$TOUT" -gt "$LIMIT" ]; then
    note "[$TX] TIMEOUT=$(cfg "$TX" TIMEOUT) -> [tx] timeout_s = $LIMIT"
    TOUT=$LIMIT
fi

ID_CALL=$(cfg "$SIMPLEX" CALLSIGN)
LONG=$(int "$(cfg "$SIMPLEX" LONG_IDENT_INTERVAL)" 0)
SHORT=$(int "$(cfg "$SIMPLEX" SHORT_IDENT_INTERVAL)" 0)
if [ "$LONG" -gt 0 ]; then ID_MIN=$LONG
elif [ "$SHORT" -gt 0 ]; then ID_MIN=$SHORT
else ID_MIN=0; note "SvxLink never identified (LONG_/SHORT_IDENT_INTERVAL = 0) — [ident] interval_min = 0"
fi
[ "$LONG" -le 0 ] || [ "$SHORT" -le 0 ] || [ "$SHORT" -ge "$LONG" ] ||
    note "SHORT_IDENT_INTERVAL=$SHORT (identification after activity) is not taken over: the callsign is sent every $LONG min, on the clock"
if [ "$ID_MIN" -gt 0 ] && [ "$(cfg "$SIMPLEX" LONG_CW_ID_ENABLE)" = 0 ] && [ "$(cfg "$SIMPLEX" SHORT_CW_ID_ENABLE)" != 1 ]; then
    note "SvxLink identified by voice; tailchaser-simplex identifies in Morse"
fi
WPM=$(int "$(cfg "$SIMPLEX" CW_WPM)" "")
if [ -z "$WPM" ]; then
    CPM=$(int "$(cfg "$SIMPLEX" CW_CPM)" 100); WPM=$((CPM / 5))
fi
PITCH=$(int "$(cfg "$SIMPLEX" CW_PITCH)" 800)
AMP=$(num "$(cfg "$SIMPLEX" CW_AMP)" -6)
[ -z "$(cfg "$SIMPLEX" MODULES)" ] ||
    note "SvxLink modules are not taken over: $(cfg "$SIMPLEX" MODULES)"

# ---------------------------------------------------------------------------
# [reflector]

CALL=$(cfg "$REFL" CALLSIGN)
[ -n "$CALL" ] || die "[$REFL] has no CALLSIGN"
DOMAIN=$(cfg "$REFL" DNS_DOMAIN)
HOSTS=$(cfg "$REFL" HOSTS); [ -n "$HOSTS" ] || HOSTS=$(cfg "$REFL" HOST)
HPORT=$(int "$(cfg "$REFL" HOST_PORT)" ""); [ -n "$HPORT" ] || HPORT=$(int "$(cfg "$REFL" PORT)" "")
[ -n "$DOMAIN$HOSTS" ] || die "[$REFL] has neither DNS_DOMAIN nor HOSTS"
KEY=$(cfg "$REFL" AUTH_KEY)
case "$KEY" in *"Change this key"*) KEY="" ;; esac
EMAIL=$(cfg "$REFL" CERT_EMAIL)
DEF_TG=$(int "$(cfg "$REFL" DEFAULT_TG)" 0)
MON=$(cfg "$REFL" MONITOR_TGS)
TGTO=$(int "$(cfg "$REFL" TG_SELECT_TIMEOUT)" 30)
UDPHB=$(int "$(cfg "$REFL" UDP_HEARTBEAT_INTERVAL)" 10)
[ "$UDPHB" -ge 1 ] && [ "$UDPHB" -le 60 ] || UDPHB=10

NODE_INFO=$(cfg "$REFL" NODE_INFO_FILE)
case "$NODE_INFO" in ''|/*) ;; *) NODE_INFO="$CONF_DIR/$NODE_INFO" ;; esac
# first "key": value of the node information file (which is not always strict JSON)
jval() {
    [ -n "$NODE_INFO" ] && [ -r "$NODE_INFO" ] || return 0
    sed -n "s/.*\"$1\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\".*/\1/p
            s/.*\"$1\"[[:space:]]*:[[:space:]]*\(-\{0,1\}[0-9][0-9.]*\).*/\1/p" "$NODE_INFO" | head -n 1
}
LOCATION=$(jval nodeLocation); SYSOP=$(jval sysop); QTH=$(jval name)
LAT=$(jval lat); LON=$(jval long); LOC=$(jval loc)

# ---------------------------------------------------------------------------
# the node's identity on the reflector: key, certificate, request, CA bundle

if [ -n "$PKI_DEST" ]; then
    PKI=$(cfg "$REFL" CERT_PKI_DIR); [ -n "$PKI" ] || PKI=/var/lib/svxlink/pki
    src() { v=$(cfg "$REFL" "$1"); [ -n "$v" ] && echo "$v" || echo "$PKI/$2"; }
    S_KEY=$(src CERT_KEYFILE "$CALL.key"); S_CRT=$(src CERT_CRTFILE "$CALL.crt")
    S_CSR=$(src CERT_CSRFILE "$CALL.csr"); S_CA=$(src CERT_CAFILE ca-bundle.crt)
    U=$(upper "$CALL")
    if [ -s "$PKI_DEST/$U.key" ]; then
        note "$PKI_DEST/$U.key exists — the key and certificate there are kept"
    elif [ -s "$S_KEY" ] && [ -s "$S_CRT" ]; then
        ( umask 077
          mkdir -p "$PKI_DEST" &&
          cp "$S_KEY" "$PKI_DEST/$U.key" &&
          cp "$S_CRT" "$PKI_DEST/$U.crt" &&
          { [ ! -s "$S_CSR" ] || cp "$S_CSR" "$PKI_DEST/$U.csr"; } &&
          { [ ! -s "$S_CA" ]  || cp "$S_CA" "$PKI_DEST/ca-bundle.crt"; }
        ) || die "could not copy the key and certificate to $PKI_DEST"
        note "the node's key and certificate were copied from $(dirname "$S_KEY") to $PKI_DEST"
    elif [ -n "$KEY" ]; then
        :       # password login: no certificate needed
    else
        note "no key + certificate for $CALL in $(dirname "$S_KEY") — the node will ask the reflector for a certificate, and its sysop has to sign the request"
    fi
fi

# ---------------------------------------------------------------------------

set -- $COS; COS_LINE=$1; COS_LOW=$2
set -- $PTT; PTT_LINE=$1; PTT_LOW=$2

# "key = value", only when there is a value
kv() { [ -z "$2" ] || printf '%-15s = %s\n' "$1" "$2"; }

echo "# tailchaser-simplex configuration — taken over from $CONF${SCRIPT:+, $SCRIPT}${MIXER:+, $MIXER}"
echo "# All settings and their meaning: /etc/tailchaser/simplex.conf.example"
echo
echo "[reflector]"
kv domain "$DOMAIN"
kv host "$HOSTS"
kv port "$HPORT"
kv callsign "$CALL"
[ -z "$KEY" ] || kv auth_key "$(quoted "$KEY")"
kv email "$EMAIL"
kv default_tg "$DEF_TG"
kv monitor_tgs "$MON"
kv tg_select_timeout_s "$TGTO"
kv udp_heartbeat_s "$UDPHB"
[ -z "$LOCATION" ] || kv location "$(quoted "$LOCATION")"
[ -z "$SYSOP" ]    || kv sysop "$(quoted "$SYSOP")"
[ -z "$QTH" ]      || kv qth_name "$(quoted "$QTH")"
kv latitude "$LAT"
kv longitude "$LON"
kv locator "$LOC"
echo
echo "[radio]"
kv serial_port "$PORT"
if [ -n "$FREQ_TX" ]; then
    kv freq_rx "$FREQ"
    kv freq_tx "$FREQ_TX"
else
    kv freq "$FREQ"
fi
kv bandwidth "$BW"
kv ctcss_rx "$CTCSS_RX"
kv ctcss_tx "$CTCSS_TX"
kv squelch "${SQL:-1}"
kv volume "${VOL:-1}"
kv filter_emphasis "$(filter "$(opt emphasis)")"
kv filter_highpass "$(filter "$(opt highpass)")"
kv filter_lowpass "$(filter "$(opt lowpass)")"
kv cos_line "$COS_LINE"
kv cos_active_low "$COS_LOW"
kv ptt_line "$PTT_LINE"
kv ptt_active_low "$PTT_LOW"
echo
echo "[tx]"
kv delay_ms "$DELAY"
kv hang_ms "$HANG"
kv timeout_s "$TOUT"
kv gain_db "$TXGAIN"
echo
echo "[ident]"
kv callsign "$ID_CALL"
kv interval_min "$ID_MIN"
kv wpm "$WPM"
kv pitch_hz "$PITCH"
kv level_db "$AMP"
echo
# A node that is taken over keeps relaying what it relayed: every squelch
# opening. The gate only watches, and says what it would have kept off the
# talkgroup; switching it on is the sysop's decision, with that in hand.
echo "[gate]"
echo "mode            = monitor    # everything is relayed, as SvxLink did. The log and the status"
echo "                             # page mark what \"on\" would keep off the talkgroup: noise bursts,"
echo "                             # dead carriers, kerchunks. Watch that for a few days, then set on."
echo
echo "[dtmf]"
[ -z "$PFX" ] || kv tg_prefix "${PFX}1"
echo
echo "[audio]"
kv alsa_device "$DEV"
kv audio_channel "$CH"
[ "$TXCH" = "$CH" ] || kv tx_channel "$TXCH"
kv playback_pct "$PLAY"
kv capture_pct "$CAP"
kv preamp_db "$PREAMP"
echo
echo "[status]"
kv http_port 8080
