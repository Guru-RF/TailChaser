# Detecting a carrier without speech

The question: how can the receiver tell, from the audio alone, that somebody
opened the squelch without saying anything?

The detector and the gate around it are the same in all four TailChaser
programs. They were first written for the repeater receiver (SVXRepeaterRX,
now `tailchaser-receiver`), where they decide whether a repeater is keyed, and
then used on the simplex node (SVXSimplexRepeater, now `tailchaser-simplex`),
where they decide whether a transmission is sent to the talkgroup. The
receiver statistics below were collected on the repeater receiver; the lab
results are measured with the simplex node's settings. Where the two roles
differ is listed at the end, followed by what the first node showed on the
air.

## What actually opens the squelch

From the log of a remote receiver with the same radio chip (ON0ORA RX: SvxLink
remotetrx, SA818 at squelch 1 with CTCSS 131.8 Hz), 36 days:

| | |
|---|---|
| squelch openings | 32 128 (about 890 a day) |
| shorter than 2 s | 31 988 — **99.6 %** |
| 2 s or longer | 140 (about 4 a day) |

The short ones arrive at the same rate at 03:00 as at 17:00 (1 270–1 410 per
hour-of-day, summed over the 36 days) and the gaps between them are
exponentially distributed. That is not people: it is the chip's squelch and
tone decoder firing on noise, roughly once every 94 seconds, around the clock.
The longer openings do follow the day (most between 16:00 and 21:00). Those are
the people — talking, or kerchunking.

A simplex node without an access tone has only the carrier squelch between the
band and the talkgroup: it will see more of this, not less.

(The log has one-second resolution, so "shorter than 2 s" is as fine as it
gets.)

So there are three things to keep away from the talkgroup, and they sound
different:

| | Sounds like |
|---|---|
| noise burst / squelch falsing | loud hiss, no pitch |
| dead carrier (kerchunk, stuck PTT, somebody listening for the tail) | near silence if strong, hiss if weak |
| tone only (1750 Hz, whistle) | one steady sine |

and one thing to let through: a voice.

## Why not something simpler

- **Minimum duration** ("ignore openings under 1 s") removes the noise bursts
  but not a two-second kerchunk, and delays everything by that second.
- **Audio level** ("is it loud?") fails both ways: squelch noise is louder than
  speech, a strong dead carrier is quieter than a soft voice.
- **RSSI** separates noise from a real carrier but not a silent carrier from a
  talking one. It is logged per opening; it does not decide.

Positive evidence of a voice is the only test that covers all three cases.

## What is measured

Every 20 ms, on the last 40 ms of audio, band-limited to 300–3000 Hz (4th-order
high-pass, so a CTCSS tone leaking past the chip's filter is removed — 131.8 Hz
is down 29 dB):

1. **Level.** Below `min_level_db` (−50 dBFS) the frame is silence. This is
   only a floor; loudness is never evidence of speech.

2. **Pitch periodicity.** The normalised autocorrelation is searched over lags
   of 2.5–13.3 ms, i.e. a pitch of 75–400 Hz. Vowels are produced by the vocal
   cords opening and closing at that rate, so a 40 ms slice of a vowel looks
   like itself shifted by one pitch period: peak ≥ `voiced_threshold` (0.50).
   Squelch noise averages 0.25 and reaches 0.50 in 0.03 % of frames; voiced
   speech averages 0.8.

3. **Not a single sine.** A sine is also periodic. Two tests catch it:
   - *Spectral purity.* The share of the 250–3200 Hz energy that lies within
     ±75 Hz of the strongest spectral line. A sine — steady, or gliding like a
     whistle — measures 1.00; voiced speech spreads its energy over its
     harmonics (in the lab speech, 12 % of the female and 0.2 % of the male
     voiced frames reach 0.90). At `tone_purity` (0.90) or more the frame is a
     tone.
   - *Autocorrelation comb.* A voice has one pitch period in the search range,
     plus at most four multiples. A sine above ~550 Hz repeats many more
     times, giving six or more equally good peaks. This one keeps working when
     the tone is buried in noise and its purity drops.

4. **Not steady.** Over the voiced frames in the window, the pitch must move
   (by 3 samples of lag, octave jumps not counted) or the level by 4 dB. Speech
   does within a syllable; mains hum and steady tones do not.

A frame that passes 1–3 is *voiced*. When `min_voiced_ms` (100 ms: five
frames) of voiced frames fall within `window_ms` (1 s) and pass 4, somebody is
talking.

Unvoiced sounds (s, f, t) are deliberately not counted: they are noise, and
indistinguishable from the noise we are rejecting. Nobody speaks for long
without a vowel.

Cost: roughly 8 million multiply-adds per second in total. Small next to what
a Pi Zero 2W can do.

## Lab results

Default settings. The corpus is built by `tests/make_corpus.py`: three minutes
of recorded speech (the SvxLink English voice prompts — one female voice, and a
copy shifted down to a male pitch), filtered to a radio pass-band and mixed with
modelled FM squelch noise; plus the non-speech signals. Each file is cut into
"squelch openings" of a given length and the detector is reset for each.

**Openings wrongly taken for speech** (180 s of each, cut into 360 / 180 / 60 openings):

| Signal | 0.5 s | 1 s | 3 s |
|---|---|---|---|
| FM squelch noise | 0 | 0 | 0 |
| white noise, 300–3000 Hz | 0 | 0 | 0 |
| quiet dead carrier | 0 | 0 | 0 |
| dead carrier + mains hum | 0 | 0 | 0 |
| dead carrier + CTCSS leak | 0 | 0 | 0 |
| 1750 Hz tone | 0 | 0 | 0 |
| whistle (gliding 950–1250 Hz, with vibrato) | 0 | 0 | 0 |
| low gliding tone (380–520 Hz) | 0 | 0 | 0 |

Steady sines at 350, 400, 450, 500, 600, 800, 1000, 2000 and 2800 Hz, a
1000 + 1500 Hz tone pair, and 1000 Hz and 1750 Hz tones buried in squelch noise
(12 dB and 6 dB above it): 0 of 60 three-second openings each.

One thing did get through: a steady **400 Hz** tone only 6 dB above heavy
squelch noise was taken for speech in 15 of 60 three-second openings (0 of 60
at 12 dB). Below ~550 Hz the comb test does not apply, and the noise both
lowers the purity and makes the tone look unsteady.

In the whole 180 s of squelch noise, three 20 ms frames were called voiced;
five within a second are needed.

**Speech detected**, 3 s openings, and how long it took:

| Speech-to-noise | female | male |
|---|---|---|
| clean | 56/56, 267 ms | 58/58, 288 ms |
| 12 dB | 56/56, 259 ms | 58/58, 344 ms |
| 6 dB | 56/56, 290 ms | 58/58, 455 ms |
| 3 dB | 56/56, 401 ms | 58/58, 629 ms |
| 0 dB | 56/56, 648 ms | 58/58, 838 ms |

With `lookback_ms = 500`, a detection later than 500 ms costs the start of the
over: at 3 dB the male voice loses its first ~0.1 s, at 0 dB the female voice
~0.15 s and the male ~0.3 s.

**How much voiced sound to ask for.** The clean recordings cut into slices as
short as a single word, and how many of the slices were taken for speech
(the two voices):

| `min_voiced_ms` | 0.3 s slices | 0.5 s | 1 s | wrongly taken for speech |
|---|---|---|---|---|
| 140 | 63–65 % | 84–87 % | 96–98 % | only the 400 Hz tone in noise, above |
| **100** | 77–78 % | 89 % | 99 % | the same |
| 80 | 80–84 % | 90–91 % | 99–100 % | the same |
| 60 | 84–87 % | 91–94 % | 100 % | the same |

(Slices that fall on a pause in the reading cannot be detected, so the short
ones never reach 100 %.) In noise the step from 140 to 100 does more: at 6 dB,
0.3 s slices go from 24–50 % to 46–69 %. `tailchaser-receiver` asks for 140 ms. On a
talkgroup the worse mistake is a word that is not relayed — "test", "break" —
so this node asks for 100 ms: in none of the signals above did that let
anything else through.

## What the lab does not show

- The speech is studio-recorded and the noise is modelled. Real off-air audio
  has the SA818's own filters, the sound card, deviation limiting, mobile
  flutter, and voices that are not a prompt reader's.
- The "male" voice is a pitch-shifted female one.
- Level thresholds depend on the capture gain of the actual card.

That is the reason for `mode = monitor`: it changes nothing for the users and
records, for every opening, what the gate would have done.

## Known behaviour

- **The first over on an idle channel runs late.** It is streamed from
  `lookback_ms` before the detection, so it reaches the talkgroup up to that
  much behind, start to end. When the talker unkeys, that much audio is still
  to come, and it is sent (`tail_silence = false`): the over arrives whole.
  Overs that pass on the hold time are only `min_open_ms` behind.
- **Music or data bursts** are not specifically handled; music will usually
  pass, data usually not.
- **Within `hold_s` of a real over**, openings that last `min_open_ms` pass
  unchecked — including a noise burst that long. It does not extend the hold.
  Shorter squelch blips are dropped.
- **A talker who keys up and stays silent for more than `lookback_ms`** is
  streamed from `lookback_ms` before the detection; the silence before that is
  dropped.

## Where the simplex node differs from the repeater receiver

- **DTMF does not open the gate** (`[dtmf] opens_gate = false`). On a repeater a
  DTMF command has to reach the repeater's logic, so there a digit opens the
  gate. Here the commands are for this node; a transmission that carries
  nothing but tones stays local.

  That needed one change in the detector. To it, a row of DTMF digits looks
  like speech: each digit is periodic, and from one digit to the next the
  "pitch" and the level move, just as they do between syllables (the test
  sequence `9123#` was declared a voice after 560 ms). So the DTMF decoder's
  verdict is handed to the detector: a frame in which a digit is on the
  channel is counted as a tone, and the two frames before the one that
  confirmed the digit are no longer counted as voiced — they held its
  beginning (`vad_process_ex`). Speech around the digits is unaffected — a
  voice that then sends a digit still passes, with the tones muted.
- **The DTMF decoder asks whether there is anything besides the two tones.**
  The decoder that came with the code asked only whether the two tones were
  there (5 % of the energy each). A vowel has a harmonic near 697 Hz and
  another near 1209 Hz often enough: it heard seven digits in the three
  minutes of recorded female speech, and on the air eleven in the first three
  minutes of a QSO — each one cutting 100 ms out of the talker's audio, and
  taking voiced frames away from the detector. Now a digit needs 80 % of the
  energy in the pair, in three half-overlapping 20 ms windows in a row
  (`dtmf.c`). In the recorded speech no window reaches that; neither do
  synthetic talkers that are purer than any voice (`tests/dtmf.py`). Digits
  are decoded from 50 ms, at 12 dB of twist, 1.5 % off frequency and down to
  about 7 dB of signal-to-noise. And nothing is muted or un-voiced on the
  strength of one window that looked like a digit: only once it is confirmed,
  backwards, which is why the audio to the talkgroup is kept 40 ms back.
- **Five voiced frames, not seven** (`min_voiced_ms = 100`): see the table above.
- **The first frame of an opening is not analysed.** The detector looks at
  40 ms: the frame and the one before. For the first frame of an opening "the
  one before" was the end of the previous opening, and when that had ended on
  a vowel the new opening began with a voiced frame it did not have.
- **Monitor mode gives the verdict of mode on.** It used to say "would be
  blocked" for every opening without a voice, including those the hold time
  would have let through, and not for those where the voice came after
  `voice_deadline_ms`. The decision is now one routine that both modes run,
  on the same "in conversation" clock. What monitor mode still does not do is
  listen on through a squelch drop-out: it has passed the audio on already, so
  the drop-out ends the opening, and the pieces of a fluttering signal are
  judged one by one.
- **The end of a delayed over is sent, not silenced.** On a repeater the talker
  would hear their own last words come back; nobody on a talkgroup does.
- **While the node transmits, and for `[tx] rx_recover_ms` after**, the receiver
  is not listened to at all: same radio, same frequency.

## The first node on the air

ON0ORA simplex (Pi Zero 2 W, WM8960, SA818 at squelch 1, no access tone, wide),
19 November 2026, one strong local station:

- **Levels.** In ten overs that were sampled, the middle frame of the speech
  sat at −38 to −26 dBFS and the loudest tenth above −25 to −19 dBFS. The
  quietest tenth — the carrier between the words — was at −52 to −54 dBFS in
  most of them: just under `min_level_db`, so with this sound card setting a
  dead carrier is "silence" by 2–4 dB. No frame was below −55 dBFS.
- **Sixteen overs of 3–30 s**: all taken for speech, with 26–52 % of their
  frames voiced; the eight for which the moment was noted, 200–600 ms after
  the squelch opened.
- **Three transmissions of 1.5–2.3 s in the first two minutes were blocked**:
  20, 100 and 100 ms of voiced sound in them, the last two at an average of
  −41 dBFS with more than half of the frames above the level floor. Seven
  voiced frames were asked for that day; with five, the last two may have
  passed — the log did not say whether their five fell within one second. What
  was on them is not known: nothing was recorded. That is where the per-frame
  line in the log comes from, and why a node that is taken over from SvxLink
  now starts in `mode = monitor`.
- **DTMF**: see above — eleven digits that nobody pressed.
- **The same afternoon, with the changes above** (five voiced frames, the new
  decoder, the per-frame line) and the same station: six transmissions of
  0.6–2.0 s. One of 1.6 s was taken for a voice, 1.1 s after the squelch
  opened, on exactly five voiced frames. One was a dead carrier (all `.`), one
  a carrier with two seconds of faint noise (−47 dBFS, all `1` and `2`). Three
  of 0.6–1.4 s were loud for 0.6–0.9 s with one to three voiced frames in it:

  ```
  00.....................V422322232233....32332221232VTT3344223V3233332
  ```

  No digit was heard in any of them. Whether those three were a word is not
  known. If they were, a short word from this station reaches the detector
  with far less periodicity than its long overs do, and that is the next
  thing to find out — with a recording, which the node does not make.

