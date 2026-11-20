<p align="center"><img src="docs/logo.svg" alt="TailChaser — repeater software" width="560"></p>

# TailChaser

**Repeater software** for FM repeaters and simplex nodes on a reflector
talkgroup — on a Raspberry Pi with a sound card and a radio. Website:
**[TailChaser.app](https://tailchaser.app)**.

One source tree, four small C programs. Pick the one that matches the site:

| program | the site | it has |
|---|---|---|
| **TailChaser-Simplex** `tailchaser-simplex` | a simplex node: one radio, one frequency | receiver + transmitter + reflector link |
| **TailChaser-Duplex** `tailchaser-duplex` | a repeater in one box | receiver + transmitter + repeater logic + reflector link |
| **TailChaser-Receiver** `tailchaser-receiver` | the receive site of a split repeater | receiver; serves it to the transmit site |
| **TailChaser-Transmitter** `tailchaser-transmitter` | the transmit site of a split repeater | transmitter + repeater logic + reflector link; gets its receiver over the network |

**Compatible with [SvxReflector](https://github.com/sm0svx/svxlink) and
TailchaserReflector**: the reflector link is the SvxReflector protocol
(version 3 — TLS with certificates and encrypted audio — and version 2 with a
password), Opus audio, talkgroups, monitored talkgroups with priorities. A
TailChaser node sits on a talkgroup next to SvxLink nodes and neither notices
the difference.

`tailchaser-receiver` speaks SvxLink's RemoteTrx protocol on its other side, so
it serves a `tailchaser-transmitter` **or** an SvxLink repeater with a
`TYPE=Net` receiver.

TailChaser is the merge of SVXSimplexRepeater (`svxsimplexd`) and
SVXRepeaterRX (`svxrxd`); both can be upgraded in place
([Upgrading](#upgrading-and-replacing-svxlink)).

```
                      ┌──────────────── tailchaser-duplex ────────────────┐
                      │                                                   │
  radio ─AF─► sound ─►│ gate ─► normaliser ─┬─► Opus ─► reflector         │
    └──SQ──► GPIO ───►│                     └─► repeat ─┐                 │
  radio ◄─AF─ sound ◄─│ shaper ◄─ queue ◄───────────────┴◄─ Opus ◄─ refl. │
    ◄─PTT/PA─ GPIO ◄──│ TX delay · hang · roger beep · time-out · Morse ID│
                      └───────────────────────────────────────────────────┘

  split site:   tailchaser-receiver ══ RemoteTrx (TCP 5210) ══► tailchaser-transmitter ══► reflector
                gate + normaliser                               repeat + everything on the TX side
```

## How it differs from SvxLink

TailChaser is not a fork of SvxLink and contains none of its code. It speaks
SvxLink's protocols — SvxReflector and RemoteTrx — so the two mix freely on a
talkgroup and on a split site. What is different is the scope and the defaults.

| | SvxLink | TailChaser |
|---|---|---|
| What it is | a general voice-services framework: logics, modules (EchoLink, parrot, voice mail…), TCL event scripts | one job: an FM repeater or simplex node on a reflector talkgroup |
| Programs | `svxlink` + `remotetrx`, one big configuration | four programs with configurations that look alike; each has only what its site needs |
| What reaches the talkgroup | whatever opens the squelch | only a voice: a gate with a voice detector keeps noise, dead carriers and tone bursts off the reflector (`docs/DETECTION.md`) |
| Levels | set per site with gains; every node sounds different | every over is levelled to one speech level before it goes anywhere |
| Squelch | many detectors (CTCSS, SigLev, GPIO, …) with thresholds to tune | the radio's line, a CTCSS tone found in the audio, or both — with the audio held back so the first word and the noise tail are right |
| Signal meter | SigLevDetNoise / tone, calibrated with `siglevdetcal` | SA818 RSSI, or the receiver noise — calibrated from one recording |
| Identification | spoken, from sound files and TCL | Morse on the clock; spoken announcements from text, made once a day by an online voice |
| Roger beep | TCL | built in, and it tells you where the over came from: "blop" local, "bleep blop" network |
| Radio | any, wired by you | any — and an SA818 module is programmed by the daemon itself |
| Setting up the audio | an oscilloscope, a deviation meter, or patience | built-in test tones and a tool that compares you with other repeaters on any web SDR (below) |
| Watching it | log file | a status page in every program, JSON for the rest |
| Moving over | — | `bootstrap.sh … replace-svxlink` reads SvxLink's files, takes key and certificate along, and `rollback` goes back |
| Size | C++, TCL, sound packs | about 12 000 lines of C, no interpreter in the daemon |

What SvxLink has and TailChaser does not: modules, EchoLink, voice
announcements from sound packs, APRS, scripting, voting between receivers
(planned), DTMF command trees beyond talkgroup selection.

## Calibrating a repeater's audio without test equipment

Getting a repeater to sound like the others on the talkgroup normally takes a
deviation meter or a service monitor. TailChaser brings its own tools, and all
they need is a web SDR that hears you.

| Tool | What it answers |
|---|---|
| **Test transmission** — `sudo systemctl kill -s USR1 tailchaser-<role>` | Keys up without the network and sends 1000 Hz, then 700 + 1900 Hz, at a known level (`[tx] test_s`, `test_level_db`). Something exact to measure. |
| **`tools/sdrmon.py`** — listens on an OpenWebRX receiver | Tunes your repeater and reference repeaters in turns and prints, for each: speech level, peaks, bass/mid/treble balance, CTCSS tone. For the test transmission: level, harmonics (over-deviation shows as distortion), and whether 1900 Hz comes back as loud as 700 Hz (pre-emphasis). |
| **Level in the log** — every over | `net: … [level −15 dBFS, peak −1]` for talkgroup audio and `rx: normaliser — −24 dBFS in, +8 dB, −16 dBFS out` for a station on the input: one number to compare the two paths with. |
| **`ctcsstest`** — on a recording of the receiver | Whether the tone squelch would open, the tone's level and margin, and the two numbers the noise signal meter needs. |
| **`agctest`, `gatetest`, `vadtest`** — on recordings | What the normaliser and the gate would have done with real overs, before changing a live repeater. |
| **Status page / `status.json`** | Live: receive level, tone level and margin, noise level, normaliser gain, S-meter. |

A session goes like this:

```sh
# 1. how do we compare with the neighbours, on the same talkgroup audio?
python3 tools/sdrmon.py http://your.websdr.example/ --profile 70cm \
        --listen ON0AAA:131.8,ON0BBB,ON0CCC --seconds 240

# 2. something exact: tones from our transmitter, measured on the SDR
sudo systemctl kill -s USR1 tailchaser-transmitter

# 3. adjust [audio] playback_pct (deviation) or [tx] level_db, repeat 2
# 4. the input: read "rx: normaliser" and "net:" in the log, set
#    [audio] normalize_target_db so both come out alike
```

A web SDR's audio has passed its own filters and AGC, so its numbers are
relative: compare with a repeater you trust, on the same receiver, rather than
reading them as kHz of deviation. That is how ON0ORA and ON0DEN were set to
within about 1 dB of each other.

## What all four have in common

**The talkgroup only hears a site when somebody is actually talking.** The
radio's squelch says "there is a carrier", not "there is a voice". The gate
listens first: audio is kept in a look-back buffer, and the over is passed on —
from its first word — once a voice is heard on it. Dead carriers, kerchunks,
noise bursts and 1750 Hz tones stay local. After an over that passed, the
channel is *in conversation* for `hold_s` and the next over passes at once.
`[gate] mode = monitor` passes everything and logs what *would* have been
blocked; `off` is a plain squelch. How it works and how it was measured:
[docs/DETECTION.md](docs/DETECTION.md).

**Every site sounds equally loud.** Stations arrive at very different levels —
their deviation, their microphone, the receiver's sound card. Before received
audio goes anywhere (the talkgroup, the repeater's own transmitter, the transmit
site), each over is brought to the same speech level: `[audio] normalize`
(on by default), `normalize_target_db = -20`, `normalize_max_gain_db = 30`. The
gain follows the loud parts of speech, drops fast and rises slowly, so pauses
are not pumped up and an empty channel's hiss is left alone. Audio *from* the
talkgroup to the transmitter is set with the sound card level (`playback_pct`,
which sets the deviation) and `[tx] gain_db`, as before.

**A transmitter that cannot get stuck.** The PTT line is taken first and pinned
inactive before anything else is set up; a time-out limits every transmission;
the daemon exits (and systemd restarts it) when the sound card stops or the PTT
line does not respond; and whatever happens to the daemon, the service runs
`tailchaser-<role> -u` afterwards, which releases PTT and the amplifier line.

**One kind of configuration file.** `/etc/tailchaser/<role>.conf` — the same
sections with the same meaning in all four, each program having only the
sections for the parts it has:

| section | simplex | duplex | receiver | transmitter |
|---|:-:|:-:|:-:|:-:|
| `[reflector]` the talkgroup link | ● | ● | | ● |
| `[uplink]` `[siglev]` serving a transmit site | | | ● | |
| `[receiver]` where the receive site is | | | | ● |
| `[radio]` SA818 channel, GPIO lines | ● | ● | ● | ● |
| `[tx]` `[ident]` carrier timing, Morse ID | ● | ● | | ● |
| `[repeater]` roger beep | | ● | | ● |
| `[gate]` `[dtmf]` the squelch gate | ● | ● | ● | (DTMF commands) |
| `[audio]` sound card, normaliser | ● | ● | ● | ● |
| `[status]` `[gpio]` `[log]` | ● | ● | ● | ● |

Every setting is explained in `config/<role>.conf.example`. A file with its
REQUIRED lines still commented out is refused (exit 78, no restart loop): a box
that was not set up does not transmit. `tailchaser-<role> -t -c FILE` checks a
file.

**A status page** on port 8080 (`/` and `/status.json`): the links, an S-meter,
the last overs with what the gate made of each, what was transmitted.

## The repeater roles

`tailchaser-duplex` and `tailchaser-transmitter` are repeaters:

- what a station says on the input is transmitted again **and** sent to the
  talkgroup; what is said on the talkgroup is transmitted;
- **a station on the input wins** over the talkgroup: a talkgroup over that
  arrives while somebody is on the input is not transmitted (the log and the
  status page say so);
- the carrier comes up `[tx] delay_ms` (300) before the audio — nothing is
  lost, the audio waits — and stays up `hang_ms` (3000) after the last audio;
- **which repeater is coming from the network** is sent in fast Morse before
  it is heard: ON0OST → `OST` (`[repeater] net_call`). Other stations get
  none — the roger beep already says "network". The speech waits in a buffer
  meanwhile, so nothing is lost and nothing is added after the over;
- a **roger beep** follows every over: "blop" after a station on the input,
  "bleep blop" after one from the talkgroup — so you hear where it came from
  (`[repeater] roger_tones`, `roger_net_tones`);
- the **time-out** (`[tx] timeout_s`) counts for as long as the transmitter is
  busy without a pause; after it the carrier drops until whoever was talking
  has stopped;
- the callsign goes out in **Morse on the clock** (`[ident]`), over whatever is
  being transmitted;
- DTMF `91<talkgroup>#` selects a talkgroup, as on an SvxLink node.

A power amplifier is switched with `[radio] pa_line`: taken just before PTT,
released just after. `refresh_after_tx = true` gives the SA818 its filter
settings again after every transmission, for a module that loses them next to
a PA. `[tx] preemphasis = true` does the 6 dB/octave pre-emphasis in software
for a module whose own is bypassed; `limit_db` puts a ceiling on what goes to
the modulator.

`[radio] type = none` is for a radio that needs no programming — only its
audio and its squelch / PTT lines are used.

**Not there yet**

- **Voting.** One receiver per repeater for now. `tailchaser-receiver` already
  reports the signal strength with every over (`[siglev]`), and the plan is for
  `-duplex` and `-transmitter` to take several receivers and pick the one with
  the highest RSSI.
- **A carrier squelch of its own.** A receiver without squelch line needs a
  CTCSS tone on its input (`[ctcss]`, below); a noise squelch in software, for
  an input without tone, is not there.
- Voice announcements, SvxLink modules (parrot, EchoLink, ...): none. The
  identification is Morse.

## Installing

On Raspberry Pi OS / Debian Bookworm or Trixie, with a WM8960 Audio HAT and the
SA818 on `/dev/serial0`:

```sh
git clone https://github.com/Guru-RF/TailChaser && cd TailChaser
sudo ./bootstrap.sh simplex          # or duplex | receiver | transmitter
sudo nano /etc/tailchaser/simplex.conf   # the lines marked REQUIRED
sudo reboot
```

`bootstrap.sh <role> deps-only` builds and installs without touching the
firmware configuration or enabling anything; `prime-firstboot` arms a
self-installing image. Other sound cards work too — the mixer is then left
alone (`playback_pct` / `capture_pct` only apply to a WM8960; set the levels
with `alsamixer`).

By hand: `make && sudo make install ROLE=simplex` (needs `libasound2-dev
libopus-dev libgpiod-dev libssl-dev`; libgpiod 1.x and 2.x are both supported).

### A split repeater

On the receive site, `/etc/tailchaser/receiver.conf`:

```ini
[uplink]
listen_port = 5210
auth_key    = a-long-shared-secret
[radio]
freq_rx     = 431.2000
ctcss_rx    = 131.8
```

On the transmit site, `/etc/tailchaser/transmitter.conf`:

```ini
[reflector]
domain      = be.svx.link
callsign    = ON0XXX
default_tg  = 8
[receiver]
host        = on0xxx-rx.example.org
auth_key    = a-long-shared-secret
[radio]
freq_tx     = 438.8000
ctcss_tx    = 131.8
[ident]
callsign    = ON0XXX
```

The transmitter connects to the receiver and reconnects by itself. The link
carries uncompressed audio by default (`codec = S16`, 256 kbit/s — fine on a
LAN); `codec = OPUS` for a slow link.

### A receiver that is not an SA818

`-receiver`, `-duplex` and `-simplex` take any receiver whose audio is on the
sound card. What opens the squelch is one of:

| The receiver | `[radio]` | `[ctcss]` |
|---|---|---|
| SA818 (it decodes the tone itself) | `type = sa818`, `ctcss_rx = 88.5`, `cos_line = 12` | `detect = false` |
| A set with a squelch line, no tone decoder | `type = none`, `cos_line = <gpio>` | `detect = true`, `tone = 88.5` — line and tone must agree |
| A set with only audio | `type = none`, `cos_line = none` | `detect = true`, `tone = 88.5` — the tone alone |

```ini
[radio]
type     = none
cos_line = none

[ctcss]
detect   = true
tone     = 79.7

[audio]
deemphasis = true      ; only for audio straight from a discriminator
```

The tone squelch is optional and off by default. It finds the tone in about
0.3 s and holds the audio back by as much (`delay_ms`), so the first word of
an over is not lost and the noise after the carrier drops is not passed on.
It tells its tone from the next one in the CTCSS table and filters it out of
the audio. A set like this has no RSSI either: `[siglev] mode = noise` makes
a signal meter out of the receiver's hiss above the speech band — the
stronger the station, the quieter it is — as SvxLink's noise detector does.
`/status.json` shows the tone level and its margin over the noise
live under `"ctcss"`; `./ctcsstest -f 79.7 recording.wav` does the same for a
16 kHz recording of the receiver. The gate still decides whether what came
through is a voice.

### Morse on the hour, a voice in between

The callsign goes out in Morse on the clock (`[ident]`). On a repeater it is
sent over whatever is on the air; while somebody is talking it is `[tx]
duck_db` (6 dB) quieter, and it comes back up when they stop.

Between the identifications the node can say something: `[announce]`, every
15 minutes by default and not when the Morse is due — unless `with_ident =
true`, which puts it right after the Morse. The text is yours, with
`%CALL`, `%TIME` and `%DATE` in it; `tailchaser-announce` turns it into speech
once a day with an online voice service (an OpenAI-compatible one, or
ElevenLabs — `provider`, `token`, `voice`) and the daemon plays the file for
the time that has come. No file, or one that is not from today, and nothing
is said.

```ini
[announce]
enabled  = true
text     = "This is %CALL, the repeater of Opwijk. It is %TIME."
provider = openai
token    = sk-...
busy     = wait        ; only in a quiet moment — or: duck, under the conversation
```

```sh
sudo -u tailchaser tailchaser-announce -c /etc/tailchaser/duplex.conf --dry-run   # the texts
sudo systemctl enable --now tailchaser-announce@duplex.timer                      # every day
```

With `busy = wait` an announcement that finds the frequency in use waits up to
`wait_s` for the carrier to drop and is left out otherwise. When a station
keys up while it plays, it goes under them like the Morse does.

A pause in an announcement is `%PAUSE` (`pause_ms`, 1000): `This is %CALL
%PAUSE it's %TIME`. And `sudo systemctl kill -s RTMIN+1 tailchaser-<role>`
sends the fast Morse other repeaters put before this one ("ORA" for ON0ORA).

To hear an announcement without waiting for the quarter:
`sudo systemctl kill -s USR2 tailchaser-<role>` plays the next one now (in a
quiet moment, like the real one).

### What the reflector shows of the station

With the login the node sends its node information, as SvxLink does from
`NODE_INFO_FILE`: place, sysop and position (`[reflector] location`, `sysop`,
`latitude`, `longitude`, `locator`), and for the receiver and the transmitter
the frequency, the CTCSS tone, and which tone opens the home talkgroup — taken
from `[radio]` and `[ctcss]`. `[location]` adds what the program cannot know:
`power_w`, `antenna`, `antenna_height_m`, and the frequencies and tones of a
radio it does not program or a receiver at another site.

### The reflector login

With a certificate (the default): on its first connection the node makes a key
and sends a signing request; the reflector's sysop signs it and from then on
the node logs in by itself, renewing in time. Key and certificate live in
`/var/lib/tailchaser/pki` — back that directory up. For a reflector that still
uses passwords: `[reflector] auth_key`.

## Upgrading and replacing SvxLink

One command takes the settings (and the reflector key and certificate) over,
stops the old program and starts the new one. If the new one does not come up —
or does not log in on the reflector within a minute — the old one is put back.

```sh
sudo ./bootstrap.sh simplex  upgrade             # from svxsimplexd
sudo ./bootstrap.sh receiver upgrade             # from svxrxd
sudo ./bootstrap.sh simplex  replace-svxlink     # from SvxLink: SimplexLogic + ReflectorLogic
sudo ./bootstrap.sh receiver replace-remotetrx   # from SvxLink's remotetrx
sudo ./bootstrap.sh <role> rollback              # back to what ran before
```

Nothing of the old setup is removed. An SvxLink *repeater* (RepeaterLogic) has
no automatic take-over yet: write `duplex.conf` or `transmitter.conf` from the
example, and copy the node's key and certificate from SvxLink's
`CERT_PKI_DIR` to `/var/lib/tailchaser/pki` so the reflector keeps knowing it.

On a box that already runs the program, `bootstrap.sh <role>` is an update:
rebuild, install, check the configuration, restart.

## Watching it

```sh
journalctl -u tailchaser-simplex -f
```

```
rx: 4.72 s passed — voice after 180 ms, stream delay 180 ms [speech, voiced 2140 ms, level -27 dBFS, rssi 121] -> TG 8
rx: 0.42 s BLOCKED — no voice [noise, voiced 0 ms of 380 ms loud, level -31 dBFS, peak -24 dBFS]
net: ON4ABC on TG 8, 12.3 s — transmitted
tx: off after 13.6 s
```

`-v` (or `[log] level = debug`) adds one line per event, including what the
normaliser did with each over.

## Tuning

- **A test transmission**: `sudo systemctl kill -s USR1 tailchaser-<role>` sends
  1000 Hz and then a two-tone (700 + 1900 Hz) for `[tx] test_s` seconds at
  `test_level_db`, without the network — for a deviation meter, or for
  `tools/sdrmon.py`, which listens to your repeater and to others on an
  OpenWebRX receiver and compares level and frequency response.
- **Deviation** is set by `[audio] playback_pct` (the sound card's output
  level) and the SA818's `volume`. Set it with a deviation meter or by ear
  against a known-good repeater; `[tx] limit_db` protects against peaks.
- **Receive level**: with the normaliser on, `capture_pct` only has to keep the
  loudest station out of clipping and the quietest above the noise of the
  sound card — around 50 % on a WM8960.
- **The gate**: run `mode = monitor` for a day and read the status page. The
  detector settings and what they trade off are in
  [docs/DETECTION.md](docs/DETECTION.md); `make tools` builds `vadtest`,
  `gatetest`, `dtmftest` and `agctest` to try settings on recordings.

## Building and testing

```sh
make                  # all four, on the Pi
make NOGPIO=1         # on a desktop: no libgpiod, file audio only
make NOGPIO=1 check   # everything that needs no hardware (python3 + openssl), ~3 min
```

`make check` runs the real programs with WAV files as squelch openings and a
recording as the transmitter, against stand-ins for the reflector, the SA818
and the repeater: the gate and DTMF decoder on generated audio, the normaliser
across a 34 dB range of input levels, the spoken announcements (made against
a stand-in for the voice service, played in a quiet moment and under a station), the tone squelch (the right tone, the
neighbouring ones, none, noise — and a receiver with nothing but audio), the reflector protocol (certificates,
encrypted audio, protocol 2 fallback, renewal), the RemoteTrx link, a split
repeater (receiver + transmitter) and the duplex repeater end to end, and the
SvxLink migration script. `make interop REFLECTOR_IMAGE=<image>` puts two nodes
through a real svxreflector in Docker.

## Layout

```
src/bin/*.c        four one-line main()s; src/role.c says what each program has
src/main.c         start-up, the poll loop
src/engine.c       the node logic for all four roles
src/gate.c vad.c dtmf.c      the squelch gate, voice detector, DTMF decoder
src/audioproc.c    normaliser, de-emphasis, transmit shaper
src/ctcss.c        tone squelch in software
src/reflector.c refmsg.c refcrypto.c tls.c pki.c srv.c   SvxReflector client
src/nettrx.c       RemoteTrx server (receiver)     src/netrx.c   its client (transmitter)
src/audio.c mixer.c sa818.c serial_worker.c gpio*.c      the hardware
src/status.c       status page
config/            one example per program
systemd/           one unit per program
tools/             offline test tools, migration scripts
tests/             the hardware-free test suites
```

## License

TailChaser is free software: you can use it, change it and pass it on under
the **GNU General Public License, version 3 or later** ([LICENSE](LICENSE)).
In short: anyone may work on it and run it; whoever distributes it, changed or
not, ships the source with it under the same license and keeps the names of
those who made it — the authors and participants are in [AUTHORS](AUTHORS).
There is no warranty.

Website: [TailChaser.app](https://tailchaser.app)
