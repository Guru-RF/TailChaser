#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS
# Test of tools/migrate-svxlink.sh: an SvxLink simplex node's files in, an
# simplex.conf out — and tailchaser-simplex must accept it.
#
#   sh tests/migrate.sh ./tailchaser-simplex        (from the repository root)
set -u
EXE=${1:-./tailchaser-simplex}
FIX=tests/fixtures/hotspot-node
MIG="sh tools/migrate-svxlink.sh"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
FAILED=0
SVXLINK_WATCHDOG=$FIX/svxlink-watchdog; export SVXLINK_WATCHDOG

ok()   { echo "  ok   $*"; }
fail() { echo "  FAIL $*"; FAILED=$((FAILED + 1)); }
check() { if eval "$1"; then ok "$2"; else fail "$2"; fi; }
# "key = value" as tailchaser-simplex reads it: has <file> <section> <key> <value>
has() {
    v=$(awk -v sec="[$2]" -v key="$3" '
        /^\[/ { s = $1 }
        s == sec && $1 == key && $2 == "=" { sub(/^[^=]*= */, ""); print; exit }' "$1")
    [ "$v" = "$4" ] || { echo "       [$2] $3 = '$v', expected '$4'"; return 1; }
}
accepted() { "$EXE" -t -c "$1" >"$TMP/t.log" 2>&1 || { sed 's/^/       /' "$TMP/t.log"; return 1; }; }

echo "migrating an SvxLink node"

# ---- the node the fixture describes: compared line by line ----
$MIG -c $FIX/svxlink.conf -s $FIX/hotspot -m $FIX/hotspot_volume >"$TMP/a.conf" 2>"$TMP/a.err"
check '[ $? -eq 0 ] && diff -u "$FIX/expected.conf" "$TMP/a.conf"' \
      "a hotspot-image node: every setting is taken over as expected"
check 'accepted "$TMP/a.conf"' "...and tailchaser-simplex accepts the result"
check 'grep -q "gate: MONITOR" "$TMP/t.log"' \
      "the node keeps relaying every squelch opening, as SvxLink did: the gate only watches"
check 'grep -q "ModuleParrot" "$TMP/a.err" && grep -q "TIMEOUT=6000 -> \[tx\] timeout_s = 360" "$TMP/a.err"' \
      "what is not taken over, or changed, is said: the parrot module, the 6000 s time-out"

# ---- another node: split frequency, tones, password, old GPIO names ----
sed -e 's/^DNS_DOMAIN=.*/HOSTS=refl1.example.org:5310,refl2.example.org/' \
    -e 's/^CERT_EMAIL=.*/AUTH_KEY="s3cret key #1"/' \
    -e 's/^CONNECT_LOGICS=.*/CONNECT_LOGICS=ReflectorLogic:8:,SimplexLogic:8:/' \
    -e 's/^TIMEOUT=6000/TIMEOUT=300/' \
    -e 's/^PTT_GPIOD_LINE=.*/PTT_PIN=!gpio17/' -e 's/^PTT_TYPE=.*/PTT_TYPE=GPIO/' \
    -e 's/^SQL_GPIOD_LINE=.*/GPIO_SQL_PIN=gpio22/' \
    -e 's/^LONG_IDENT_INTERVAL=60/LONG_IDENT_INTERVAL=0/' -e 's/^SHORT_IDENT_INTERVAL=0/SHORT_IDENT_INTERVAL=10/' \
    -e 's/^CW_WPM=.*/CW_CPM=125/' -e 's/^PREAMP=0/PREAMP=-3.5/' \
    -e 's/^NODE_INFO_FILE=.*//' -e 's/^MONITOR_TGS=.*/MONITOR_TGS=8+,9,1745++/' \
    $FIX/svxlink.conf | awk '/^\[Tx1\]/ { tx = 1 } tx && /^AUDIO_CHANNEL=/ { $0 = "AUDIO_CHANNEL=1" } { print }' >"$TMP/b-svxlink.conf"
cat >"$TMP/b-hotspot" <<'B'
#!/bin/bash
#sa818 --port /dev/ttyUSB9 radio --frequency 999.000
sa818 --port /dev/ttyS0 radio --frequency 433.500 --offset -1.6 --ctcss 94.8,127.3 --squelch 3
B
$MIG -c "$TMP/b-svxlink.conf" -s "$TMP/b-hotspot" -m /dev/null >"$TMP/b.conf" 2>"$TMP/b.err"
check '[ $? -eq 0 ] && has "$TMP/b.conf" radio freq_rx 433.500 && has "$TMP/b.conf" radio freq_tx 431.9000' \
      "sa818 --frequency with --offset: receive and transmit frequency apart"
check 'has "$TMP/b.conf" radio ctcss_tx 94.8 && has "$TMP/b.conf" radio ctcss_rx 127.3' \
      "sa818 --ctcss TX,RX: a tone for each direction"
check 'has "$TMP/b.conf" radio serial_port /dev/ttyS0 && has "$TMP/b.conf" radio bandwidth wide &&
       has "$TMP/b.conf" radio squelch 3 && has "$TMP/b.conf" radio volume 4' \
      "options the script leaves out get the sa818 tool's defaults (wide, volume 4); commented-out lines are ignored"
check 'has "$TMP/b.conf" reflector host "refl1.example.org:5310,refl2.example.org" &&
       has "$TMP/b.conf" reflector auth_key "\"s3cret key #1\"" && has "$TMP/b.conf" reflector monitor_tgs "8+,9,1745++"' \
      "reflector by host list, with a password"
check 'has "$TMP/b.conf" radio ptt_line 17 && has "$TMP/b.conf" radio ptt_active_low true &&
       has "$TMP/b.conf" radio cos_line 22 && has "$TMP/b.conf" radio cos_active_low false' \
      "GPIO lines in the old sysfs notation (gpio22, !gpio17)"
check 'has "$TMP/b.conf" tx timeout_s 300 && has "$TMP/b.conf" ident interval_min 10 && has "$TMP/b.conf" ident wpm 25 &&
       has "$TMP/b.conf" dtmf tg_prefix 81 && has "$TMP/b.conf" audio tx_channel right &&
       has "$TMP/b.conf" audio preamp_db -3.5 && has "$TMP/b.conf" audio playback_pct keep' \
      "time-out, identification, DTMF prefix, audio channels; no mixer script = the mixer is kept"
check 'accepted "$TMP/b.conf"' "...and tailchaser-simplex accepts the result"
check '! grep -q "s3cret" "$TMP/b.err" "$TMP/t.log"' "the password is not echoed to the terminal or the log"

# ---- the watchdog's limit counts when it is the lower one ----
sed 's/^LOW_TIME_REQUIRED=.*/LOW_TIME_REQUIRED=240/' $FIX/svxlink-watchdog >"$TMP/wd"
SVXLINK_WATCHDOG="$TMP/wd" $MIG -c $FIX/svxlink.conf -s $FIX/hotspot -m $FIX/hotspot_volume >"$TMP/c.conf" 2>/dev/null
check 'has "$TMP/c.conf" tx timeout_s 240' "the PTT watchdog's limit becomes the transmit time-out"

# ---- key and certificate ----
mkdir -p "$TMP/pki"
for f in on0tst-s.key on0tst-s.crt on0tst-s.csr ca-bundle.crt; do echo "pem $f" >"$TMP/pki/$f"; done
sed -e 's/^CALLSIGN="ON0TST-S"/CALLSIGN="on0tst-s"/' -e "s|^CERT_EMAIL=.*|CERT_PKI_DIR=$TMP/pki|" \
    $FIX/svxlink.conf >"$TMP/d-svxlink.conf"
$MIG -c "$TMP/d-svxlink.conf" -s $FIX/hotspot -m $FIX/hotspot_volume -p "$TMP/dest" >"$TMP/d.conf" 2>"$TMP/d.err"
check '[ "$(cat "$TMP/dest/ON0TST-S.key")" = "pem on0tst-s.key" ] && [ -s "$TMP/dest/ON0TST-S.crt" ] &&
       [ -s "$TMP/dest/ON0TST-S.csr" ] && [ -s "$TMP/dest/ca-bundle.crt" ]' \
      "-p: key, certificate, request and CA bundle are copied, under the upper-case callsign"
check '[ "$(stat -c %a "$TMP/dest")" = 700 ] && [ "$(stat -c %a "$TMP/dest/ON0TST-S.key")" = 600 ] &&
       [ -s "$TMP/pki/on0tst-s.key" ]' \
      "...readable by their owner only; SvxLink's own files stay where they are"
echo "newer key" >"$TMP/dest/ON0TST-S.key"
$MIG -c "$TMP/d-svxlink.conf" -s $FIX/hotspot -m $FIX/hotspot_volume -p "$TMP/dest" >/dev/null 2>"$TMP/d.err"
check '[ "$(cat "$TMP/dest/ON0TST-S.key")" = "newer key" ] && grep -q "are kept" "$TMP/d.err"' \
      "a key that is already there is never overwritten"

# ---- not a simplex node ----
sed 's/^LOGICS=.*/LOGICS=RepeaterLogic,ReflectorLogic/' $FIX/svxlink.conf >"$TMP/e-svxlink.conf"
$MIG -c "$TMP/e-svxlink.conf" -s $FIX/hotspot >"$TMP/e.conf" 2>"$TMP/e.err"
check '[ $? -ne 0 ] && [ ! -s "$TMP/e.conf" ] && grep -q "not a simplex node" "$TMP/e.err"' \
      "a repeater's configuration is refused, with nothing written"

# ---- the shipped example: a box that was not set up must not transmit ----
EX=config/simplex.conf.example
"$EXE" -t -c $EX >"$TMP/x.log" 2>&1
check '[ $? -eq 78 ] && grep -q "freq is required" "$TMP/x.log" && grep -q "callsign is required" "$TMP/x.log"' \
      "the example configuration is refused as it is (exit 78: the service does not restart on it)"
sed -e 's/^#domain /domain /' -e 's/^#callsign /callsign /' -e 's/^#default_tg /default_tg /' \
    -e 's/^#freq /freq /' "$EX" >"$TMP/x.conf"
check 'accepted "$TMP/x.conf" && grep -q "ON0XXX in Morse every 60 min" "$TMP/t.log" &&
       ! grep -q "unknown setting" "$TMP/t.log"' \
      "...and accepted once its REQUIRED lines are filled in; every setting in it is known"
sed -e 's/^#rssi_lo .*/rssi_lo = 79/' -e 's/^#rssi_hi .*/rssi_hi = 79/' "$TMP/x.conf" >"$TMP/y.conf"
"$EXE" -t -c "$TMP/y.conf" >"$TMP/y.log" 2>&1
check '[ $? -eq 78 ] && grep -q "rssi_hi the greater" "$TMP/y.log"' \
      "an S-meter scale without a span is refused (rssi_hi must be above rssi_lo)"

echo
if [ "$FAILED" -ne 0 ]; then echo "$FAILED check(s) FAILED"; exit 1; fi
echo "all checks passed"
