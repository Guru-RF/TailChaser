#!/usr/bin/env bash
# Runs INSIDE the test container that tests/cutover.sh starts: a Debian with
# systemd as PID 1, dressed up as a hotspot-image node that runs SvxLink.
#
#   run.sh setup          make it that node: SvxLink's files where the image
#                         has them, its units (stand-ins that hold no hardware),
#                         a reflector that knows the node's certificate
#   run.sh cutover        bootstrap.sh simplex replace-svxlink — once on a node whose
#                         hardware cannot be used (SvxLink must come back), once
#                         for real — and how the service then behaves
#   run.sh after-reboot   what runs after a restart of the machine; a rebuild;
#                         bootstrap.sh simplex rollback
set -u

SRC=/src                            # the repository, read-only
REPO=/root/TailChaser       # where "git clone" put it on the node
FIX=$SRC/tests/fixtures/hotspot-node
CONF=/etc/tailchaser/simplex.conf
REFL=/var/lib/standin-reflector
U=tailchaser-simplex.service
LOGS=/var/tmp/cutover
FAILED=0

ok()    { printf '  ok   %s\n' "$1"; }
fail()  { printf '  FAIL %s\n' "$1"; FAILED=$((FAILED + 1)); }
check() { if eval "$1"; then ok "$2"; else fail "$2"; fi; }
show()  { sed 's/\x1b\[[0-9;]*m//g; s/^/       | /' "$1" | tail -n "${2:-25}"; }
active()  { systemctl is-active  "$1" 2>/dev/null; }    # active | inactive | failed | activating
enabled() { systemctl is-enabled "$1" 2>/dev/null; }    # enabled | disabled | masked
prop()    { systemctl show -p "$1" --value "$U"; }
events()  { grep -c "^\[\"$1\"" "$REFL/events.log" 2>/dev/null; }
wait_for() {                # wait_for SECONDS CONDITION
    local i
    for i in $(seq 1 $(($1 * 5))); do eval "$2" && return 0; sleep 0.2; done
    return 1
}
logged_in() { curl -fs http://127.0.0.1:8080/status.json 2>/dev/null | grep -q '"connected": *true'; }

mkdir -p "$LOGS"

# ===========================================================================
setup() {
    # ---- SvxLink's files, where the hotspot image has them ----
    install -d /etc/svxlink/svxlink.d /var/lib/svxlink/pki
    sed 's/^DNS_DOMAIN=.*/HOSTS=127.0.0.1:5300/' "$FIX/svxlink.conf" > /etc/svxlink/svxlink.conf
    cp "$FIX"/svxlink.d/*.conf /etc/svxlink/svxlink.d/
    cp "$FIX/node_info.json" /etc/svxlink/
    install -m 755 "$FIX/hotspot" "$FIX/hotspot_volume" "$FIX/svxlink-watchdog" /usr/sbin/
    printf 'RUNASUSER=svxlink\nCFGFILE=/etc/svxlink/svxlink.conf\nLOGFILE=/var/log/svxlink\n' > /etc/default/svxlink
    # what those scripts call: there is no chip and no sound card to set here
    printf '#!/bin/sh\nexit 0\n' > /usr/local/bin/sa818
    printf '#!/bin/sh\nexit 0\n' > /usr/bin/amixer
    # the image's other start-up scripts: present, and not the ones to read
    printf '#!/bin/bash\n# waits for the sound card, then sets the levels\n/usr/sbin/hotspot_volume\n' > /usr/sbin/svxlink_checkalsa
    printf '#!/bin/bash\n# 2 m or 70 cm board?\nsed -i "s/434.925/145.925/" /usr/sbin/hotspot.default 2>/dev/null\nexit 0\n' > /usr/sbin/hotspot_bandswitch
    chmod 755 /usr/local/bin/sa818 /usr/bin/amixer /usr/sbin/svxlink_checkalsa /usr/sbin/hotspot_bandswitch

    # ---- its units. svxlink.service is the image's, minus SvxLink itself ----
    cat > /lib/systemd/system/svxlink.service <<'EOF'
[Unit]
Description=SvxLink repeater control software (stand-in: holds no hardware)
After=network.target remote-fs.target syslog.target time.target
After=wm8960-soundcard.service

[Service]
EnvironmentFile=/etc/default/svxlink
ExecStartPre=-/bin/rm -f /tmp/state
ExecStartPre=-/bin/touch ${LOGFILE}
ExecStartPre=/usr/sbin/svxlink_checkalsa
ExecStartPre=/usr/sbin/hotspot_bandswitch
ExecStartPre=/usr/sbin/hotspot
ExecStartPre=/usr/sbin/hotspot_volume
ExecStart=/bin/sh -c 'exec sleep infinity'
Restart=on-failure

[Install]
WantedBy=multi-user.target
EOF
    # a plain file in /etc, as on the image: it cannot be masked
    cat > /etc/systemd/system/svxlink-watchdog.service <<'EOF'
[Unit]
Description=SVXLink PTT watchdog (stand-in)
After=svxlink.service
Wants=svxlink.service

[Service]
ExecStart=/bin/sh -c 'exec sleep infinity'
Restart=always

[Install]
WantedBy=multi-user.target
EOF
    cat > /etc/systemd/system/hotspot-online.service <<'EOF'
[Unit]
Description=Announce the hotspot online (stand-in)
After=svxlink.service
Requires=svxlink.service

[Service]
Type=oneshot
ExecStart=/bin/true

[Install]
WantedBy=svxlink.service
EOF

    # ---- the reflector, and what it signed for this node back in SvxLink's days ----
    python3 "$SRC/tests/cutover/reflector.py" "$REFL" pki ON0TST-S /var/lib/svxlink/pki > "$REFL.serial" || exit 1
    cat > /etc/systemd/system/standin-reflector.service <<EOF
[Unit]
Description=Stand-in SvxReflector (tests/e2e_simplex.py)

[Service]
ExecStart=/usr/bin/python3 $SRC/tests/cutover/reflector.py $REFL serve 5300
Restart=always

[Install]
WantedBy=multi-user.target
EOF
    systemctl daemon-reload
    systemctl enable -q --now standin-reflector.service svxlink.service svxlink-watchdog.service hotspot-online.service

    # ---- the repository, as cloned onto the node ----
    mkdir -p "$REPO"
    (cd "$SRC" && tar cf - --exclude=.git --exclude='*.o' --exclude='*.d' --exclude=./tailchaser-simplex --exclude=./tailchaser-duplex --exclude=./tailchaser-receiver --exclude=./tailchaser-transmitter \
        --exclude=./vadtest --exclude=./gatetest --exclude=__pycache__ .) | tar xf - -C "$REPO"

    [ "$(active svxlink.service)" = active ] && [ "$(active svxlink-watchdog.service)" = active ] &&
        [ "$(active standin-reflector.service)" = active ] || { echo "setup: the stand-in node did not come up"; exit 1; }
    echo "the node before: svxlink $(active svxlink.service)/$(enabled svxlink.service)," \
         "svxlink-watchdog $(active svxlink-watchdog.service)/$(enabled svxlink-watchdog.service)," \
         "hotspot-online $(enabled hotspot-online.service)"
}

# ===========================================================================
cutover() {
    echo
    echo "replace-svxlink on a node whose hardware tailchaser-simplex cannot use"
    LOGIN_WAIT=20 "$REPO/bootstrap.sh" simplex replace-svxlink > "$LOGS/1.log" 2>&1
    rc=$?
    check '[ $rc -ne 0 ] && grep -q "rolled back: the old service is running again" "$LOGS/1.log"' \
          "it says so, and exits with an error (no sound card, no GPIO chip in this container)"
    [ $rc -ne 0 ] && grep -q "rolled back" "$LOGS/1.log" || show "$LOGS/1.log" 40
    check '[ "$(active svxlink.service)" = active ] && [ "$(enabled svxlink.service)" = enabled ]' \
          "SvxLink runs again: started, enabled, not masked"
    check '[ "$(active svxlink-watchdog.service)" = active ] && [ "$(enabled svxlink-watchdog.service)" = enabled ]' \
          "...and so does its watchdog"
    check '[ "$(active $U)" != active ] && [ "$(enabled $U)" = disabled ]' \
          "tailchaser-simplex is stopped and disabled"
    check '[ "$(stat -c "%a %U:%G" $CONF)" = "640 root:tailchaser" ]' \
          "the settings were taken over into $CONF, for root and the service only"
    grep -v '^#\|^domain \|^host ' "$FIX/expected.conf" > "$LOGS/want.conf"
    grep -v '^#\|^domain \|^host ' "$CONF" > "$LOGS/got.conf"
    check 'diff -u "$LOGS/want.conf" "$LOGS/got.conf" > "$LOGS/conf.diff" && grep -q "^host  *= 127.0.0.1:5300" $CONF' \
          "...found through SvxLink's own unit and /etc/default/svxlink: the same settings as in tests/migrate.sh"
    [ -s "$LOGS/conf.diff" ] && show "$LOGS/conf.diff"
    P=/var/lib/tailchaser/pki
    check '[ "$(stat -c "%a %U" $P/ON0TST-S.key)" = "600 tailchaser" ] && cmp -s $P/ON0TST-S.key /var/lib/svxlink/pki/ON0TST-S.key &&
           cmp -s $P/ON0TST-S.crt /var/lib/svxlink/pki/ON0TST-S.crt && [ "$(stat -c "%a %U" $P)" = "700 tailchaser" ]' \
          "the node's reflector key and certificate were copied, for the service only"
    check '[ "$(events csr)" = 0 ]' "the reflector was not bothered"

    echo
    echo "replace-svxlink for real (bench audio: the one thing changed in the migrated settings)"
    sed -i 's|^alsa_device .*|alsa_device     = file:pause:1000|' "$CONF"
    "$REPO/bootstrap.sh" simplex replace-svxlink > "$LOGS/2.log" 2>&1
    rc=$?
    check '[ $rc -eq 0 ] && grep -q "tailchaser-simplex runs this box now" "$LOGS/2.log"' "it succeeds"
    [ $rc -eq 0 ] || show "$LOGS/2.log" 60
    check 'grep -q "exists — keeping it" "$LOGS/2.log"' "an existing $CONF is kept, not written again"
    check '[ "$(active $U)" = active ] && [ "$(enabled $U)" = enabled ]' "tailchaser-simplex runs and is enabled"
    check '[ "$(active svxlink.service)" = inactive ] && [ "$(enabled svxlink.service)" = masked ]' \
          "SvxLink is stopped and masked"
    check '[ "$(active svxlink-watchdog.service)" = inactive ] && [ "$(enabled svxlink-watchdog.service)" = disabled ]' \
          "its watchdog is stopped and disabled"
    check 'journalctl -u $U --no-pager | grep -q "reflector: logged in as ON0TST-S"' \
          "the journal shows the login on the reflector"
    check 'grep -q "\"cert-login\", \"ON0TST-S\"" $REFL/events.log && [ "$(events csr)" = 0 ]' \
          "...with the certificate SvxLink had: the reflector saw ON0TST-S, and no new signing request"
    PID=$(prop MainPID)
    check '[ "$(ps -o user= -p $PID)" = tailchaser ]' "the daemon runs as the service user, not as root"
    check 'logged_in' "the status page answers on port 8080: connected"
    systemctl restart svxlink.service > "$LOGS/portal.log" 2>&1
    rc=$?
    check '[ $rc -ne 0 ] && [ "$(active svxlink.service)" = inactive ] && [ "$(active $U)" = active ]' \
          "\"systemctl restart svxlink\" — what the hotspot menus do — is refused; tailchaser-simplex keeps running"
    systemctl start svxlink-watchdog.service > /dev/null 2>&1
    check '[ "$(active svxlink.service)" = inactive ]' \
          "starting SvxLink's watchdog (it Wants=svxlink) does not bring SvxLink back either"
    systemctl stop svxlink-watchdog.service

    echo
    echo "the service"
    sleep 36
    check '[ "$(prop MainPID)" = "$PID" ] && [ "$(prop NRestarts)" = 0 ] && [ "$(active $U)" = active ]' \
          "after 40 s it is still the same process: systemd's watchdog (30 s) is being fed"

    T0=$(date +%s)
    kill -9 "$PID"
    wait_for 15 '[ "$(active $U)" = active ] && [ "$(prop MainPID)" != "$PID" ] && [ "$(prop MainPID)" != 0 ]'
    rc=$?
    check '[ $rc -eq 0 ]' "kill -9: a new daemon runs $(( $(date +%s) - T0 )) s later"
    check 'journalctl -u $U --since "@$T0" --no-pager | grep -q "no PTT line to release"' \
          "...and in between the service ran the PTT release (ExecStopPost: tailchaser-simplex -u)"
    check 'wait_for 20 logged_in' "...logged in again"

    PID=$(prop MainPID)
    T0=$(date +%s)
    kill -STOP "$PID"
    wait_for 60 '[ "$(prop MainPID)" != "$PID" ] && [ "$(prop MainPID)" != 0 ] && [ "$(active $U)" = active ]'
    rc=$?
    check '[ $rc -eq 0 ] && journalctl -u $U --since "@$T0" --no-pager | grep -qi "watchdog timeout"' \
          "a daemon that hangs (SIGSTOP) is killed by the watchdog and replaced, $(( $(date +%s) - T0 )) s later"
    check 'journalctl -u $U --since "@$T0" --no-pager | grep -q "no PTT line to release"' \
          "...again with the PTT release in between"

    wait_for 20 logged_in
    systemctl stop "$U"
    check '[ "$(prop Result)" = success ] && [ "$(prop ExecMainStatus)" = 0 ] && [ "$(active $U)" = inactive ]' \
          "systemctl stop: the daemon exits by itself, status 0"

    cp -p "$CONF" "$CONF.good"
    sed -i '/^freq /d' "$CONF"
    systemctl start "$U" > /dev/null 2>&1
    sleep 8
    check '[ "$(prop ExecMainStatus)" = 78 ] && [ "$(active $U)" = failed ] && [ "$(prop NRestarts)" = 0 ]' \
          "a configuration without a frequency: exit 78, and the service does not keep retrying"
    check 'journalctl -u $U --no-pager -n 20 | grep -q "freq is required"' "...the journal says why"
    cp -p "$CONF.good" "$CONF" && rm -f "$CONF.good"
    systemctl reset-failed "$U"
    systemctl start "$U"
    check 'wait_for 20 logged_in' "with the setting back, it starts and logs in"
    return 0
}

# ===========================================================================
after_reboot() {
    echo
    echo "after a restart of the machine"
    wait_for 60 '[ "$(systemctl is-system-running 2>/dev/null)" = running ] || [ "$(systemctl is-system-running 2>/dev/null)" = degraded ]'
    check 'wait_for 40 logged_in' "tailchaser-simplex came up by itself and is logged in"
    check '[ "$(active svxlink.service)" = inactive ] && [ "$(active svxlink-watchdog.service)" = inactive ]' \
          "SvxLink and its watchdog stayed down"
    check '[ -z "$(systemctl --failed --no-legend)" ]' "no unit failed on the way up"
    [ -z "$(systemctl --failed --no-legend)" ] || systemctl --failed --no-legend

    echo
    echo "replace-svxlink again, on a node that tailchaser-simplex already runs (an update)"
    PID=$(prop MainPID)
    "$REPO/bootstrap.sh" simplex replace-svxlink > "$LOGS/3.log" 2>&1
    rc=$?
    check '[ $rc -eq 0 ] && grep -q "restarting it on the new build" "$LOGS/3.log" && [ "$(prop MainPID)" != "$PID" ] && logged_in' \
          "it rebuilds, restarts the daemon and waits for the login; SvxLink is not touched"
    [ $rc -eq 0 ] || show "$LOGS/3.log" 40
    check '[ "$(enabled svxlink.service)" = masked ] && [ "$(active svxlink.service)" = inactive ]' "SvxLink still masked"

    echo
    echo "rollback"
    "$REPO/bootstrap.sh" simplex rollback > "$LOGS/4.log" 2>&1
    rc=$?
    check '[ $rc -eq 0 ] && grep -q "installed but disabled" "$LOGS/4.log"' "it succeeds"
    [ $rc -eq 0 ] || show "$LOGS/4.log" 40
    check '[ "$(active svxlink.service)" = active ] && [ "$(enabled svxlink.service)" = enabled ]' \
          "SvxLink: started, enabled, unmasked"
    check '[ "$(active svxlink-watchdog.service)" = active ] && [ "$(enabled svxlink-watchdog.service)" = enabled ]' \
          "its watchdog: started and enabled"
    check '[ "$(active $U)" = inactive ] && [ "$(enabled $U)" = disabled ]' "tailchaser-simplex: stopped and disabled"
    check '[ -x /usr/local/sbin/tailchaser-simplex ] && [ -f "$CONF" ] && [ -f /var/lib/tailchaser/pki/ON0TST-S.key ]' \
          "nothing was removed: the program, its settings and the certificate stay for the next attempt"
    check 'cmp -s /var/lib/svxlink/pki/ON0TST-S.key /var/lib/tailchaser/pki/ON0TST-S.key &&
           [ "$(stat -c %U /var/lib/svxlink/pki/ON0TST-S.key)" = root ]' \
          "SvxLink's own files were never changed"
    return 0
}

case "${1:-}" in
    setup)        setup ;;
    cutover)      cutover ;;
    after-reboot) after_reboot ;;
    *) echo "usage: run.sh setup|cutover|after-reboot" >&2; exit 2 ;;
esac
[ "$FAILED" -eq 0 ]
