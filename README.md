<p align="center"><img src="docs/logo.svg" alt="TailChaser — repeater software" width="560"></p>

# TailChaser

**Repeater software** for FM repeaters and simplex nodes on a reflector
talkgroup — on a Raspberry Pi with a sound card and a radio. Website:
**[TailChaser.app](https://tailchaser.app)**.

One source tree, five small C programs. Pick the one that matches the site:

| program | the site | it has |
|---|---|---|
| **TailChaser-Simplex** `tailchaser-simplex` | a simplex node: one radio, one frequency | receiver + transmitter + reflector link |
| **TailChaser-Duplex** `tailchaser-duplex` | a repeater in one box | receiver + transmitter + repeater logic + reflector link |
| **TailChaser-Receiver** `tailchaser-receiver` | the receive site of a split repeater | receiver; serves it to the transmit site |
| **TailChaser-Transmitter** `tailchaser-transmitter` | the transmit site of a split repeater | transmitter + repeater logic + reflector link; gets its receiver over the network |
| **TailChaser-SDRReceiver** `tailchaser-sdrreceiver` | an extra receiver, anywhere | no radio: listens on a web SDR (OpenWebRX, KiwiSDR, Web-888, UberSDR) and serves that to a repeater or node, which votes |

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
| Programs | `svxlink` + `remotetrx`, one big configuration | five programs with configurations that look alike; each has only what its site needs |
| What reaches the talkgroup | whatever opens the squelch | only a voice: a gate with a voice detector keeps noise, dead carriers and tone bursts off the reflector (`docs/DETECTION.md`) |
| Levels | set per site with gains; every node sounds different | every over is levelled to one speech level before it goes anywhere |
| Squelch | many detectors (CTCSS, SigLev, GPIO, …) with thresholds to tune | the radio's line, a CTCSS tone found in the audio, or both — with the audio held back so the first word and the noise tail are right |
| Signal meter | SigLevDetNoise / tone, calibrated with `siglevdetcal` | SA818 RSSI, or the receiver noise — calibrated from one recording |
| Identification | spoken, from sound files and TCL | Morse on the clock; spoken announcements from text, made once a day by an online voice |
| Roger beep | TCL | built in, and it tells you where the over came from: "blop" local, "bleep blop" network |
| Radio | any, wired by you | any — and an SA818 module is programmed by the daemon itself |
| Setting up the audio | an oscilloscope, a deviation meter, or patience | every source is levelled by itself; the deviation is measured in Hz on any web SDR and the tool says what to change (below) |
| Watching and running it | log file, an editor over ssh | a status page in every program, an admin portal (configuration, restart, block receiver/transmitter), an API with keys |
| Moving over | — | `bootstrap.sh … replace-svxlink` reads SvxLink's files, takes key and certificate along, and `rollback` goes back |
| Size | C++, TCL, sound packs | about 14 000 lines of C, no interpreter in the daemon |

What SvxLink has and TailChaser does not: modules, EchoLink, voice
announcements from sound packs, APRS, scripting, DTMF command trees beyond talkgroup selection.

## Calibrating a repeater's audio without test equipment

Getting a repeater to sound like the others on the talkgroup normally takes a
deviation meter or a service monitor. TailChaser needs neither: a web SDR that
hears the transmitter is enough.

It comes down to two things, and only one of them is yours to set.

**1. Every source has one level — nothing to set.** Whatever is transmitted is
brought to the same level before it reaches the modulator:

| What is transmitted | Its level |
|---|---|
| a station on the input | normalised to `[audio] normalize_target_db` (per over) |
| audio from the talkgroup | normalised to the same target (`[tx] normalize`), looking 100 ms ahead so a programme that turns loud does not spike |
| spoken announcements | `[announce] level_db` |
| Morse, roger beep, test tones | their own fixed `level_db` |

So a quiet station, a loud one, a SvxLink node that sends 10 dB too little:
on the air they are alike, and there is no per-source gain to tune.

**2. The deviation — set once, measured in Hz.** What is left is how far full
level swings the transmitter. `tools/devcal.py` measures that on a web SDR and
says what to change:

```sh
python3 tools/devcal.py --sdr http://your.websdr.example/ --profile 70cm \
        --freq 438.800 --ctcss 131.8 --max-dev 2500 \
        --trigger "ssh node sudo systemctl kill -s USR1 tailchaser-transmitter"
```
```
carrier            +1030 Hz off (transmitter and SDR together)
CTCSS 131.8 Hz     266 Hz deviation
test tone 1000 Hz  625 Hz deviation at -20 dBFS
full level (0 dBFS) would be 6250 Hz — wanted 2500 Hz
change the transmit level by -8.0 dB: [tx] level_db -8.0 from what it is now
```

Set `[tx] level_db` (or the sound card, `[audio] playback_pct`), run it again,
and it says "that is right". That is the whole alignment.

How it can be exact without a calibrated receiver: the SDR listens to the
carrier in SSB. There an FM signal is a row of tones — the carrier and, one
tone-frequency apart, its sidebands — and how strong the sidebands are next to
the carrier depends on the deviation alone (Bessel functions), not on the
SDR's gain, its AGC or how strong the signal is. The test transmission sends
1000 Hz at a known level with receiver and talkgroup muted, so nothing else is
in it.

- `--sdr` is any OpenWebRX receiver that hears you (or `TAILCHASER_SDR` in the
  environment). `--profile` is part of the name of the band profile the
  frequency is in; devcal moves its own connection to the SDR device that has
  it, and never switches a profile — that would change it for everybody
  listening. If the SDR is showing another band, it says so and stops.
- `--max-dev`: 2500 for a 12.5 kHz channel (the default), 5000 for a 25 kHz one.
- `--ctcss`: the tone's deviation is measured too (it is usually made by the
  radio, so that one is set on the radio).
- `--trigger` starts the test transmission; without it devcal asks you to.
- Lowering the level makes the node quieter than neighbours that over-deviate
  into their limiter. That is them, not you.

The other tools:

| Tool | What it answers |
|---|---|
| **Test transmission** — `sudo systemctl kill -s USR1 tailchaser-<role>` | Keys up without the network and sends 1000 Hz, then 700 + 1900 Hz, at a known level (`[tx] test_s`, `test_level_db`). Something exact to measure. |
| **`tools/sdrmon.py`** — listens on an OpenWebRX receiver | Tunes your repeater and reference repeaters in turns and prints, for each: speech level, peaks, bass/mid/treble balance, CTCSS tone. For the test transmission: level, harmonics (over-deviation shows as distortion), and whether 1900 Hz comes back as loud as 700 Hz (pre-emphasis). |
| **Level in the log** — every over | `net: … [level −15 dBFS, peak −1]` for talkgroup audio and `rx: normaliser — −24 dBFS in, +8 dB, −16 dBFS out` for a station on the input: one number to compare the two paths with. |
| **`ctcsstest`** — on a recording of the receiver | Whether the tone squelch would open, the tone's level and margin, and the two numbers the noise signal meter needs. |
| **`agctest`, `gatetest`, `vadtest`** — on recordings | What the normaliser and the gate would have done with real overs, before changing a live repeater. |
| **Status page / `status.json`** | Live: receive level, tone level and margin, noise level, normaliser gain, S-meter. |

`sdrmon.py` is for the other question — how do we sound next to the
neighbours:

```sh
python3 tools/sdrmon.py --sdr http://your.websdr.example/ --profile 70cm \
        --listen ON0AAA:131.8,ON0BBB,ON0CCC --seconds 240
```

Its numbers come from the SDR's audio, which has passed the SDR's own filters
and AGC: they compare stations on the same receiver, they are not kHz. For
kHz, `devcal.py`.

## What they all have in common

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
sections with the same meaning in all of them, each program having only the
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
- **which repeater an over from the network came through** is sent in fast
  Morse after it: ON0OST → `OST` (`[repeater] net_call`, 34 wpm), in place of
  the "bleep blop". A station that is not on a repeater is sent by its whole
  callsign (`net_call_full`; off, it gets the bleep blop). A station on the
  repeater's own input gets the roger beep. The Morse is
  dropped when somebody talks again before it. `net_call_when = before` sends
  it ahead of the over instead (the speech waits in a buffer, and the bleep
  blop follows the over); `net_call_once = true` only when it is another
  repeater than the one sent last;
- **one level on the transmitter**: talkgroup audio is levelled like a station
  on the input (`[tx] normalize`), announcements and Morse have their own fixed
  levels — so the only thing left to set per site is the deviation
  (`[audio] playback_pct`), once;
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

- **A carrier squelch for a radio without a squelch line.** Such a receiver
  needs a CTCSS tone on its input (`[ctcss]`, below). (A squelch made from
  the receiver's hiss exists, but only in `tailchaser-sdrreceiver`.)
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
Its two ends are the set's own: `noise_open_db` is the hiss with the squelch
open and nobody there (reads 0), `noise_full_db` the hiss under a strong
station (reads 100); `/status.json` shows the hiss live as `noise.level_db`.
The status page draws 0..100 as S1..S9+60, and the reflector is told the
same number — so leave `[status] rssi_lo`/`rssi_hi` out on such a set.
On a repeater whose own receiver hears a last word of the station come back
when it lets go, `[gate] tail_silence = true` drops what is still waiting to
be sent at that moment.
`/status.json` shows the tone level and its margin over the noise
live under `"ctcss"`; `./ctcsstest -f 79.7 recording.wav` does the same for a
16 kHz recording of the receiver. The gate still decides whether what came
through is a voice.

### Morse on the clock, and a voice

The callsign goes out in Morse on the clock (`[ident] interval_min`: 60, 15,
whatever the licence asks). On a repeater it is
sent over whatever is on the air; while somebody is talking it is `[tx]
duck_db` (6 dB) quieter, and it comes back up when they stop.

The node can also say something: `[announce]`, every `interval_min` (15 by
default) and not when the Morse is due — unless `with_ident = true`, which
puts it right after the Morse. The two clocks are independent: Morse every
quarter and the voice on the hour after it is `[ident] interval_min = 15`,
`[announce] interval_min = 60`, `with_ident = true`. The text is yours, with
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

How it is said:

| `[announce]` | |
|---|---|
| `call_spelling = phonetic` | `%CALL` as "Oscar, November, Zero, …" instead of letter by letter |
| `time_style = spoken` | `%TIME` as "quarter past two" instead of 14:15 (English) |
| `%PAUSE` in the text, `pause_ms` | a pause: `This is %CALL, %PAUSE it's %TIME`. The parts are spoken separately and joined, so it works with every voice service. The comma before it stays: the voice then says the callsign as a sentence that goes on |
| `level_db` | how loud, dBFS RMS like `[audio] normalize_target_db` (−16; −8 is about as loud as a station). The peaks of a synthetic voice go under a limiter |
| `speed` | slower or faster speech (ElevenLabs 0.7–1.2, e.g. `0.8`) |
| `voice`, `model`, `style`, `url` | which voice; for ElevenLabs `voice` is the voice id |

Every different phrase is asked for once and kept, so a text with the time in
it costs one day of requests and nothing after that. The token is sent only
to an `https://` address.

And `sudo systemctl kill -s RTMIN+1 tailchaser-<role>`
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

While it runs the node also tells the reflector, as an SvxLink node does, what
its receivers hear: per receiver the signal (0..100), whether its squelch is
open and whether it is the one listened to — and whether the transmitter is on
the air. The reflector passes that on in its status, where a dashboard can
show it. A node that votes lists every receiver under its own letter: `A` the
one in the box, `B`, `C`.. those of `[receiver]`, `[receiver2]`.. by their
`name`. A change goes out at once, the signal twice a second during an over.
(With a password login, the old protocol, the node sends no node information:
the values go out, but without the receivers' names to go with them.)

### The reflector login

With a certificate (the default): on its first connection the node makes a key
and sends a signing request; the reflector's sysop signs it and from then on
the node logs in by itself, renewing in time. Key and certificate live in
`/var/lib/tailchaser/pki` — back that directory up. For a reflector that still
uses passwords: `[reflector] auth_key`.

The reflector is checked too. Its CA bundle is kept from the first connection
(`ca-bundle.crt` in the same directory) and from then on the reflector has to
show a certificate from that CA, made out to its name (`verify_host`); a
different bundle that is offered later is put aside as `ca-bundle.crt.offered`,
not believed. A node that holds a certificate refuses to fall back to the old
protocol without encryption. When the reflector really gets a new CA, the log
says which file to move.

### An amplifier

`[radio] pa_line` is a second output for a power amplifier. It goes up just
before the PTT and down just after it; with `pa_delay_ms` it is a sequencer
instead — the exciter first, the amplifier that many milliseconds later, and
off again before the exciter. Keep `[tx] delay_ms` at least as long.

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

## Listening in

`[status] monitor = true` puts a **Listen** card under the signal meter of
the status page, with **Input** and **Output** under each other: the audio
of the receiver (all of it, squelch open or not) and of what goes to the
transmitter, in the browser. Each is off until it is switched on, and has a
level meter (dBFS, with a peak mark), a spectrum of 0 to 8 kHz and a mute
button — muted, the meter and the spectrum keep running. For setting a node up and
for finding out why something sounds the way it does — is the hiss there, is
the tone, what does the normaliser make of a quiet station, what does an
announcement sound like before the radio.

It is off unless it is turned on: whoever can open the status page can then
listen (four listeners at most). The streams are websockets,
`ws://<node>:8080/monitor/rx` and `/monitor/tx`: binary messages of 16-bit
little-endian mono PCM at 16 kHz, so a script can record them too. A
transmit site's input is the receiver that has the over; a receiver has an
input only.

## More receivers: voting, and a receiver on a web SDR

A repeater or a node can listen with more than one receiver. Up to four
receivers elsewhere, `[receiver]` and `[receiver2]` to `[receiver4]`, of
either kind and in any mix: a `tailchaser-receiver` on another hill, or a
**`tailchaser-sdrreceiver`**: a receiver without a radio, that listens to
the frequency on a web SDR. A duplex or simplex node has them next to the
radio in the box (five in all); a transmit site has nothing else — and its
only receiver may well be a web SDR: a repeater with no receiver of its own.

```
                        ┌─ the receiver in the box / the receive site
 station ──► on the air ┤                                              ├─► voter ─► repeated, talkgroup
                        └─ web SDR ─► tailchaser-sdrreceiver ─ 5211 ───┘
```

**tailchaser-sdrreceiver** (`sudo ./bootstrap.sh sdrreceiver`, settings in
`/etc/tailchaser/sdrreceiver.conf`) needs no hardware and can run on the same
box as the repeater, or anywhere else.

| | |
|---|---|
| **Web SDRs** | `[sdr] type =` `openwebrx` (OpenWebRX and OpenWebRX+), `kiwisdr`, `web888`, `ubersdr`. One connection, narrow FM on `[sdr] freq` — for a repeater that is its **input** frequency. |
| **A good guest** | It is one of the SDR's listeners, all day: ask the owner. It never changes what listeners share — on an OpenWebRX with several devices it moves its own connection to the device that is on the band, and waits when none is. A KiwiSDR or Web-888 with a time limit wants the owner's password (`[sdr] password`); an UberSDR's hourly session limit is answered by connecting again. |
| **Squelch** | A web SDR gives audio only, so the squelch is made from it: an FM receiver without a carrier hisses, a carrier quiets it (`[sdr] squelch_open_db` / `squelch_close_db`; the status page shows the hiss). `[ctcss] detect` adds the tone, so it opens for the same stations as the repeater. |
| **Signal strength** | From the same hiss: full quieting = 100 (`[siglev]`). That is what the repeater votes with. |
| **Then** | the same as a receive site: the gate (a voice, not a carrier), levelling, DTMF, the RemoteTrx link on `[uplink] listen_port` (5211), a status page (8081). |

**Voting** (in tailchaser-transmitter, -duplex and -simplex, `[voter]`):

- The receiver that hears a station first has the over.
- Another one takes it when it hears the station clearly better for a while
  (`margin`, `hold_ms`; `switch = false` turns that off) — but only between
  receivers that are in step. A web SDR, a second or more behind, never takes
  a running over: the listener would hear the last words twice
  (`switch_lag_ms`). It is there for what the other receivers do not hear.
- **How far behind a web SDR is** does not change from over to over, so it can
  be measured once and written down: `lag_ms` in that receiver's section.
  `tools/sdrlag.py`, run on the node, starts the test transmission, listens to
  the transmitter on the web SDR and times the tone:

  ```sh
  sudo python3 tools/sdrlag.py --sdr http://your.websdr.example/ --profile 70cm-high \
       --freq 438.800 --trigger "systemctl kill -s USR1 tailchaser-transmitter"
  ```

  That is for an OpenWebRX receiver. A KiwiSDR, a Web-888 and an UberSDR are
  timed on the time signal DCF77 instead (Europe), with nothing transmitted:
  `python3 tools/sdrlag.py --type kiwisdr --sdr http://kiwi.example:8073`.
  Measured in November 2026, the receiver and the internet without our own
  buffer: OpenWebRX about 0.7 s, KiwiSDR and Web-888 60–90 ms, UberSDR about
  30 ms. Add `[sdr] buffer_ms` (300) and, with a tone squelch, `[ctcss]
  delay_ms` (300); sdrlag does that sum (`--buffer-ms`, `--ctcss-delay-ms`).

  Without `lag_ms` the voter learns the delay from the overs two receivers
  share (`voter.receivers[].lag_ms` in `/status.json` shows what it has); with
  it, the late copy of an over is known for what it is from the first over
  on, and a simplex node stops ignoring that receiver exactly that long after
  its own transmission instead of `tx_blank_ms`.
- A receiver on the internet is late — a web SDR by a second or two. What it
  still brings of an over that has ended on another receiver is the same over,
  and is not repeated; only when it goes on for longer than it was late is it
  a station the other receiver lost, and it gets the over. How late each
  receiver is, is learnt as it goes.
- On a simplex node the other receivers hear the node's own transmitter, late:
  they are not listened to during a transmission and `tx_blank_ms` after it.

The status page and `/api/v1/status` (`voter`) say which receiver has the over
and how many each has had; the log says where every over came from.

Not tested against the real thing yet: KiwiSDR, Web-888 and UberSDR are
written from their source code and tested against stand-ins only. OpenWebRX
is tested on a live receiver.

## A receiver on the USB port: RTL-SDR and HydraSDR RFOne

A receive site does not have to be an SA818 or a radio on a sound card. With
an `[sdr]` section, `tailchaser-receiver` listens with an SDR on the USB port:

```ini
[sdr]
type = rtlsdr        # or hydrasdr
freq = 431.200       # the repeater's input
```

The radio's own command line tool gives the samples (`rtl_sdr` from the
`rtl-sdr` package, `hydrasdr_rx` from
[hydrasdr/rfone_host](https://github.com/hydrasdr/rfone_host)); no library of
either is linked. The FM receiver is made in TailChaser: the channel is
mixed out of the spectrum, filtered, demodulated and de-emphasised. From
there on it is the receive site you know — squelch and signal strength from
the hiss as with a web SDR, `[ctcss] detect` for the tone, the gate, the
link to the repeater, voting. Nothing is late: such a receiver is in step
with the others, so the voter may hand it a running over. No sound card, no
hat, no GPIO: any Linux box with a USB port will do, and
`tailchaser-sdrreceiver` takes the same two types.

The two are not the same kind of radio:

| | RTL-SDR (Blog V3 / V4 and the like) | HydraSDR RFOne |
|---|---|---|
| What it is | a TV stick put to better use: **the budget receiver** | a measuring receiver: **the engineering one** |
| Converter | 8 bit — about 50 dB between the weakest and the strongest signal it can hold at once | 12 bit at 20 MS/s — 80 dB dynamic range, 10.4 effective bits |
| Front end | a tuner chip and nothing before it | tracking filters before the tuner; 3.5 dB noise figure, +35 dBm IIP3 |
| Frequency | 1 ppm with a TCXO (ask for one: a stick without is 20 kHz off on 70 cm; `ppm` corrects) | 0.5 ppm, and a 10 MHz reference input for a GPSDO |
| Range | about 24–1766 MHz | 24–1800 MHz, three antenna inputs |
| Samples | 240 kS/s is enough here: little processor, a Pi Zero does it | 2.5 MS/s at least: wants a Pi 4 or better |
| Extras | bias tee (V3 / V4) | bias tee, clock outputs, GPIO, expansion boards |
| Price | a few tens of euros | several times that |

What that means on a hill:

- **RTL-SDR**: fine where the band is quiet — a receive site of its own, away
  from the transmitter, or a fill-in receiver in a valley. On a site with a
  transmitter, pagers or broadcast next door it goes deaf when they key: 8
  bits cannot hold a strong signal and a weak one together. A cavity or a
  band-pass filter in front of it is not a luxury.
- **HydraSDR RFOne**: for the site where that matters, and for the receiver
  you want to trust the numbers of. Its filters and its range of 80 dB keep a
  weak station readable next to a strong neighbour, and with a GPSDO it is on
  frequency for good.

TailChaser uses both for the same thing, one narrow FM channel. The
difference is in what the radio can stand, not in what the program does.

Not tested on the real thing yet: both are written from the tools' source
and tested against a stand-in that gives the same samples. The receive chain
itself — squelch, tone, level, timing — is tested with those.

## The admin portal and the API

`http://<the node>:8080/admin` — on the same port as the status page.

| | |
|---|---|
| **Log in** | one user, `admin`. On a new node the password is `admin`; the first login has to change it before anything else opens, and that first login works from a private address only (the local network, a VPN). |
| **Configuration** | the node's configuration file in the browser. *Check and save* lets the program itself check it first (the same check as `-t`); what it refuses is not written, and the file before is kept. |
| **Restart** | at the next quiet moment — that is when a saved configuration takes effect. |
| **Block receiver / transmitter** | the receiver: nothing it hears is repeated or sent to the talkgroup. The transmitter: it is not keyed, whatever comes in. A block stays, also over a restart, until it is released. |
| **API keys** | for other programs and devices. `read` keys see the status, `control` keys can also block. A key is shown once; only its hash is kept. |

The API, for a display, a knob on the desk, a script:

```sh
curl -H "Authorization: Bearer KEY" http://node:8080/api/v1/status      # everything the status page shows
curl -H "Authorization: Bearer KEY" http://node:8080/api/v1/block       # {"rx":false,"tx":false}
curl -H "Authorization: Bearer KEY" -d "tx=1" http://node:8080/api/v1/block          # block the transmitter
curl -H "Authorization: Bearer KEY" -d '{"rx":true}' http://node:8080/api/v1/block   # ...JSON works too
```

The whole of it — fields, errors, what a microcontroller needs — is in
[docs/API.md](docs/API.md). `X-API-Key: KEY` is accepted as well. `rx` and `tx` are `1`/`0` or
`true`/`false`; what is not named stays as it is; the answer is the state
after the call.

**A split repeater has one address: the transmit site.** It fetches its
receive site's status and passes it on as `"remote_receiver"` in its own
(`[receiver] status_port`), and blocking the receiver there blocks what comes
from the receive site. So the API is spoken to a transmitter, a duplex or a
simplex node — never to a receiver.

It is plain HTTP: keep it on the local network or a VPN, or put a TLS proxy in
front of it. `[status] admin = false` turns portal and API off; the state
(password hash, key hashes, blocks) is in `/var/lib/tailchaser/admin-<role>.conf`
— delete that file to start again from `admin` / `admin`. For the portal to
save the configuration the service has to be allowed to write that one file;
`bootstrap.sh` sets that up.

## Tuning

Three signals put something on the air on request, each in a quiet moment:

| Signal | Sends | Command |
|---|---|---|
| `USR1` | test tones: 1000 Hz, then 700 + 1900 Hz — receiver and talkgroup are muted meanwhile | `sudo systemctl kill -s USR1 tailchaser-<role>` |
| `USR2` | the next spoken announcement, now | `sudo systemctl kill -s USR2 tailchaser-<role>` |
| `RTMIN+1` | this repeater's call in fast Morse, as other repeaters send it ("ORA") | `sudo systemctl kill -s RTMIN+1 tailchaser-<role>` |

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
make                  # all five, on the Pi
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
src/bin/*.c        five one-line main()s; src/role.c says what each program has
src/main.c         start-up, the poll loop
src/engine.c       the node logic for all roles, the voter
src/gate.c vad.c dtmf.c      the squelch gate, voice detector, DTMF decoder
src/audioproc.c    normaliser, de-emphasis, transmit shaper
src/ctcss.c        tone squelch in software
src/reflector.c refmsg.c refcrypto.c tls.c pki.c srv.c   SvxReflector client
src/nettrx.c       RemoteTrx server (receiver)     src/netrx.c   its client (transmitter)
src/audio.c mixer.c sa818.c serial_worker.c gpio*.c      the hardware
src/sdr.c sdr_kiwi.inc sdr_uber.inc   the web SDR client: OpenWebRX, KiwiSDR / Web-888, UberSDR
src/status.c       status page, the HTTP server
src/monitor.c      listening in: the audio over a websocket
src/sdr_local.inc  an SDR on the USB port: RTL-SDR, HydraSDR RFOne (the FM receiver)
src/admin.c admin.html   the admin portal and the API
config/            one example per program
systemd/           one unit per program
tools/             offline test tools, migration scripts, sdrmon.py (web SDR
                   comparison), devcal.py (deviation in Hz from a web SDR),
                   sdrlag.py (how far behind a web SDR is),
                   tailchaser-announce (the spoken announcements)
docs/              DETECTION.md (how the gate decides), API.md, the logo
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
