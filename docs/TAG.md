# The callsign tag

A repeater can say who is talking in a way a machine can read: a noise-like
signal added to the transmit audio, about 32 dB under the speech. A listener
hears a little more hiss in the quiet tail. A phone or a small device next to
the radio's loudspeaker reads the callsign from it.

```ini
[tag]
enabled  = true
level_db = -43      ; dBFS RMS, as speech is measured: 32 dB under speech at -11
```

**Format v0**, on the air since 2026-11-23. The same day it was read through
the air: off ON0UHF, from a radio's loudspeaker, by a VFO-Knob running its
TailChaser Tag firmware, at `level_db = -43`. (A first try with the tag 6 dB
weaker and a shorter quiet tail was not caught.) It may still change: there
is no version field.

The example configs switch it on; a config without a `[tag]` section has it
off.

## What is sent, and when

- The tag is on for as long as the transmitter is keyed, under whatever else
  is sent, and in the tail where there is nothing else.
- Whole frames of 2.624 s, back to back, on a frame clock that starts when the
  transmitter comes on. A frame never holds two messages.
- **An over from the talkgroup:** the callsign of the node it came from, with
  `local` = 0.
- **A station on the repeater's own input:** the repeater's own callsign, with
  `local` = 1. The repeater does not know who that is.
- **Nobody yet** (Morse identification, a spoken announcement, a test tone
  after the carrier came up): the repeater's own callsign, `local` = 1.
- **A simplex node** (`tailchaser-simplex`) does the same, but it transmits
  only what comes from the talkgroup, so its tag carries the node each over
  came from. Its own callsign, with `local` = 1, is only where nobody is
  talking yet. A simplex node usually has no hang time (`hang_ms = 0`), so
  there is no quiet tail: a reader has to catch the tag in the pauses.
- **The first frame** starts with the carrier, before anybody's audio, so it
  carries the repeater's own callsign; a talker's callsign follows from the
  next frame on.
- **The tail carries the last talker** until the transmitter drops. The quiet
  seconds after an over are where it is read best.
- When the talker changes without the transmitter dropping, the tag changes at
  the next frame boundary.
- A callsign is sent without what follows a dash — ON4ABC-KNOB is ON4ABC, and
  a repeater that logs in as ON0ABC-S is ON0ABC — in upper case. (The format
  itself holds nine characters and has the dash; TailChaser does not use it.)
- It is added after the transmit level and the limiter, at `level_db` plus
  `[tx] level_db`: its place under the speech stays the same when the
  transmit level is changed.
- It is only in what goes to the transmitter: the status page's listen-in
  (`/monitor/tx`) is the audio before the tag is added.

## The signal

| | |
|---|---|
| Chips | 2000 chips/s, +1/−1, on a carrier of **1650.152439 Hz** (= 4330 cycles per frame), low-passed at complex baseband to about ±1150 Hz → roughly 500..2800 Hz |
| Symbol | 32 chips = 16 ms, BPSK; data is **differential**: bit 1 = this symbol has the opposite sign of the one before |
| Frame | 164 symbols = 5248 chips = **2.624 s**, repeated back to back without a gap for as long as the transmitter is on |
| Symbol 0 | the reference (+1) |
| Symbols 1..15 | the known sync bits (bit k of 0x9A6D for symbol k; `syncbits_16` in [tag-vectors.json](tag-vectors.json), index 0 unused) |
| Symbols 16..163 | 148 coded bits, interleaved |
| Chips | one LFSR sequence of 5248 chips for the whole frame, the same in every frame (`chips_164x32` in tag-vectors.json): symbol k is multiplied by its own 32 chips, so nothing repeats inside a frame |

Chip generator (17-bit state, seed 0x1ACE5), per chip:
`bit = (r ^ r>>2 ^ r>>3 ^ r>>5) & 1; r = (r >> 1) | (bit << 16); chip = (r & 1) ? +1 : -1`.
The first 16 chips are `-+--+++--++-+-++`.

## The message

68 bits: 48 bits callsign, 1 bit local (1 = heard on the repeater's own input,
0 = came from the network), 3 bits zero, 16 bits CRC.

- Callsign: up to 9 characters, upper case, padded with spaces on the right,
  alphabet `" 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-/"` (index 0..38), value =
  base-40 number with the first character most significant, sent as 48 bits
  MSB first.
- CRC-16: polynomial 0x1021, start value 0xFFFF, over the first 52 bits, MSB
  first, no final XOR.
- Convolutional code: K = 7, rate 1/2, generators 171 and 133 (octal), 6 zero
  tail bits → (68 + 6) × 2 = 148 coded bits. Shift register:
  `sr = ((sr << 1) | bit) & 0x7F; out1 = parity(sr & 0171); out2 = parity(sr & 0133)`.
- Interleaver: coded bit i goes to position `(i * 59) % 148`.

[tag-vectors.json](tag-vectors.json) has the chips, the sync bits, the
interleaver, and the payload bits, coded bits and symbol signs of three
messages (ON6URE network, ON0ORA local, ON4ABC-XY network) to check an
implementation against.

## Decoding

1. Mix the audio down by the carrier, low-pass to ±1250 Hz, keep 4000 complex
   samples/s (2 per chip, 64 per symbol).
2. Reference: the frame's chips through the same low-pass, 2 samples per chip.
3. Sync: for every lag, despread the 16 sync symbols; the metric is
   `| Σ_{k=1..15} ±(z_k · conj(z_{k-1})) |` with the sign from the sync bits,
   each z divided by the square root of the local power (so loud speech does
   not win). Frames repeat every 10496 samples (at 4 kHz): add the metric of
   lags one frame apart.
4. At the best lags: despread all 164 symbols, `d_k = Re(z_k · conj(z_{k-1}))`
   divided by the local power at both symbols, summed over all the frames that
   are in the buffer. Soft bit = −d_k. De-interleave, Viterbi, check the CRC.
5. Try the sample-clock drift hypotheses 0 and ±120 ppm and lags ±1 sample.
6. A CRC that is right is a reading. Nothing else is: never show a callsign
   whose CRC failed. For display, two identical readings in a row are safer
   still.

It is read where no speech covers it: in pauses and in the tail after an over
(TailChaser repeaters hang 3..5 s). Under continuous speech it is hit and
miss. A decoder that needs one whole frame from the sync position needs about
3 s of quiet; treating the frame as a ring lowers that.

On a microcontroller (ESP32-S3): an I2S microphone at 16 kHz is plenty (8 kHz
works: the band ends at 2.8 kHz). The sync search is the cost: 4000 lags/s ×
16 symbols × 64 complex products ≈ 4 million multiply-adds per second, or FFT
correlation. Everything after sync is small. The VFO-Knob's decoder uses about
a quarter of one core of an ESP32-S3.

## For a reader

- A CRC that is right is a reading; nothing else is. Two identical readings
  in a row are safer still.
- The frame clock does not jump at a talker change, so the timing can be kept
  for as long as the signal is there. The symbols that are added up belong to
  one message: keep the last three or four frames, and start a new sum after a
  good reading. A sum of two talkers fails the CRC.
- One over, one entry: the same callsign in the frames that follow is the same
  over. A new one when the callsign or `local` changes, or when the tag was
  gone for some ten seconds.

`tests/tag.py` has the format once more in plain Python (a decoder for a
recording whose frame start is known), and `make tagtest` builds a tool that
writes the tag alone: `./tagtest wav ON6URE network 8 out.wav`.
