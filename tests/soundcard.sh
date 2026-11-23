#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS
# Test of tools/tailchaser-soundcard with stand-ins for the Pi's commands: the
# card is loaded when it is not there, the repeater board's relays are
# switched, the mixer script runs, and a card that never appears is an error.
#
#   sh tests/soundcard.sh        (from the repository root)
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
FAILS=0
check() { if eval "$1"; then echo "  ok   $2"; else echo "  FAIL $2"; FAILS=$((FAILS + 1)); fi; }
mkdir "$TMP/bin"
# stand-ins: each writes what it was asked; dtoverlay makes the card appear
cat > "$TMP/bin/dtoverlay" <<'X'
#!/bin/sh
echo "dtoverlay $*" >> "$LOG"
case "$1" in
    wm8960-soundcard) [ -n "$NOCARD" ] || echo " 0 [wm8960soundcard]: simple-card - wm8960-soundcard" > "$CARDS" ;;
    audiosense-pi)    [ -n "$NOCARD" ] || echo " 0 [audiosensepi   ]: audiosense-pi - audiosense-pi" > "$CARDS" ;;
esac
X
printf '#!/bin/sh\necho "dtparam $*" >> "$LOG"\n' > "$TMP/bin/dtparam"
printf '#!/bin/sh\necho "pinctrl $*" >> "$LOG"\n' > "$TMP/bin/pinctrl"
printf '#!/bin/sh\necho "mixer" >> "$LOG"\n' > "$TMP/mixer.sh"
chmod +x "$TMP/bin/"* "$TMP/mixer.sh"
go() {  # go <conf lines...>: a fresh run, no card to begin with unless CARD0 is set
    : > "$TMP/log"; printf '%s\n' "${CARD0:---- no soundcards ---}" > "$TMP/cards"
    printf '%s\n' "$@" > "$TMP/conf"
    PATH="$TMP/bin:$PATH" LOG="$TMP/log" CARDS="$TMP/cards" TAILCHASER_CARDS="$TMP/cards" \
        TAILCHASER_CARD_WAIT=1 bash tools/tailchaser-soundcard "$TMP/conf" > "$TMP/out" 2>&1
}

go "AUDIO=wm8960"
check '[ $? -eq 0 ] && grep -q "dtoverlay wm8960-soundcard" "$TMP/log" && grep -q "dtparam i2c_arm=on" "$TMP/log"' \
      "a WM8960 that is not there is loaded as an overlay"
go "AUDIO=tlv320"
check '[ $? -eq 0 ] && grep -q "dtoverlay audiosense-pi" "$TMP/log"' \
      "the TI module is loaded as audiosense-pi"
CARD0=" 0 [wm8960soundcard]: simple-card - wm8960-soundcard" go "AUDIO=wm8960"
check '[ $? -eq 0 ] && ! grep -q dtoverlay "$TMP/log"' \
      "a card that is there is left alone"
NOCARD=1 go "AUDIO=wm8960"
check '[ $? -eq 1 ] && grep -q "did not appear" "$TMP/out"' \
      "a card that does not appear is an error: TailChaser is not started on nothing"
go "AUDIO=wm8960" "BOARD=rptr" "MIXER_SCRIPT=$TMP/mixer.sh"
check '[ $? -eq 0 ] && grep -q "pinctrl set 7 op dh" "$TMP/log" && grep -q "pinctrl set 8 op dh" "$TMP/log"' \
      "the repeater board: both bypass relays off (GPIO 7 and 8 high)"
check '[ "$(tail -1 "$TMP/log")" = mixer ]' \
      "the mixer script runs, after the card is there"
go "BOARD=rptr"
check '[ $? -eq 0 ] && grep -q "pinctrl set 7" "$TMP/log" && ! grep -q dtoverlay "$TMP/log"' \
      "relays only: the sound card is the firmware's business"
go "AUDIO=cm108"
check '[ $? -eq 2 ]' "an audio module it does not know is refused"
bash -n bootstrap.sh
check '[ $? -eq 0 ]' "bootstrap.sh parses"

[ "$FAILS" -eq 0 ] && echo "soundcard: all passed" || { echo "soundcard: $FAILS failed"; exit 1; }
