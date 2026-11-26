# The TailChaser API

For a display, a knob on the desk, a script: read what a node is doing, and
block its receiver or its transmitter from far away.

- Same port as the status page (`[status] http_port`, 8080), plain HTTP.
- A key is made in the admin portal (`http://<node>:8080/admin` → API keys).
  A **read** key may read; a **control** key may also block.
- The key goes in a header, either one:
  `Authorization: Bearer <key>` or `X-API-Key: <key>`.
- Every answer is JSON. An error is `{"ok":false,"error":"..."}` with the
  status `401` (no key, or not a key of this node), `403` (a read key that
  tries to block) or `404`.
- **Which node to ask:** a transmitter, a duplex or a simplex node. A split
  repeater answers at its transmit site — what its receive site shows is in
  there as `remote_receiver`, and blocking the receiver there blocks what
  comes from the receive site. A `tailchaser-receiver` does not have to be
  asked anything.

## `GET /api/v1/block`

```json
{"rx":false,"tx":false}
```

## `POST /api/v1/block`  (control key)

Name what has to change; what is not named stays as it is. The answer is the
state after the call, in the same form as the `GET`.

```sh
curl -H "X-API-Key: KEY" -d "tx=1"            http://node:8080/api/v1/block   # form
curl -H "X-API-Key: KEY" -d '{"rx":true}'     http://node:8080/api/v1/block   # JSON
curl -H "X-API-Key: KEY" -X POST "http://node:8080/api/v1/block?rx=0&tx=0"    # in the address
```

Values: `1` / `0`, `true` / `false`, `on`.

- **`rx` blocked:** the receiver is a squelch that never opens — nothing it
  hears is repeated or sent to the talkgroup.
- **`tx` blocked:** the transmitter is not keyed, whatever comes in: no
  repeating, no talkgroup audio, no identification, no announcement.
- A block is kept: it is still there after a restart or a power cut, until it
  is released. The node's log says who set it (the key's name) and from where.

## `GET /api/v1/status`

Everything the status page shows; the same as `/status.json`, which needs no
key. What a small device wants from it:

| Field | |
|---|---|
| `role` | `simplex` · `duplex` · `receiver` · `transmitter` · `sdrreceiver` |
| `has.rx` `has.tx` `has.reflector` `has.repeater` | what this program has |
| `block.rx` `block.tx` | what is blocked |
| `ident.callsign` | the callsign |
| `radio.freq_mhz` `radio.freq_tx_mhz` `radio.ctcss_rx` `radio.ctcss_tx` `radio.bandwidth` | the radio |
| `tx.keyed` `tx.why` `tx.keyed_ms` | on the air, why (`talkgroup audio`, `announcement`, …), for how long |
| `gate.cos` | the squelch is open |
| `gate.open` | …and what is received is passed on (a voice was heard) |
| `gate.rssi` (`gate.rssi_lo`, `gate.rssi_hi`) | signal strength, raw; `rssi_lo` = S1, `rssi_hi` = S9+60; −1 = no signal. On a transmit site it is what the receiver that has the over reports (0..100), so the same field works on every program; `remote_receiver.gate.rssi` is that receiver's own raw reading |
| `gate.level_db` | receive audio level, dBFS |
| `ctcss.open` `ctcss.level_db` `ctcss.snr_db` | the tone squelch in software, when `ctcss.detect` |
| `link.connected` `link.tg` `link.home_tg` `link.talker` | the talkgroup link: logged in, talkgroup now, talkgroup at rest, who is talking (`""` = nobody) |
| `receiver.connected` `receiver.squelch` `receiver.siglev` | a transmit site: its link to the receive site |
| `voter.active` | who has the over: `-1` nobody, `0` the receiver in the box, `1`… the first, second… of `voter.receivers` |
| `voter.receivers[]` | the receivers elsewhere this node votes with: `name`, `peer`, `connected`, `squelch`, `ignored` (open, but not listened to: a late copy, or the node's own transmitter), `siglev`, `active`, `overs`, `lag_ms` (how far behind it is, 0 = not seen yet), `lag_set` (true: from `[receiverN] lag_ms`; false: learned from shared overs) |
| `sdr` | a receiver that listens with an SDR (tailchaser-sdrreceiver, or a tailchaser-receiver with an `[sdr]` section): `type`, `local` (true: an RTL-SDR or HydraSDR on the USB port; false: a web SDR), `url`, `connected`, `listening`, `state` (in words), `smeter_db` (a web SDR's own meter; on USB the channel power in dB below full scale), `buffer_ms`, `rate`, `connects`, `underruns`, `dropped_ms` |
| `noise.level_db` | the hiss above the speech band, dBFS — what an SDR receiver's squelch and signal strength are made from |
| `remote_receiver` | a transmit site: the receive site's whole status (the same fields), or `null` when it does not answer |
| `history[]` | the last squelch openings: `t`, `ms`, `pass`, `like`, `level_db`, `rssi`, `tg`, `sent` |
| `net_history[]` | the last overs from the talkgroup: `t`, `ms`, `call`, `tg`, `outcome` |
| `uptime_s` `version` `board` | the node |

The status changes about three times a second; asking once or twice a second
is plenty. The server answers one request at a time and closes the
connection after each (`Connection: close`).

## The audio: `/monitor/rx`, `/monitor/tx`

With `[status] monitor = true` (the status has `"monitor": true` then), two
websockets carry the audio of the receiver and of what is transmitted:
binary messages of 16-bit little-endian mono PCM, 16 kHz, about 100 ms each.
`/monitor/tx` has messages only while something is sent — none in the quiet
tail; the last one of an over is shorter and follows within 60 ms.
No key: it is the status page's "Listen" card. `404` when it is off, `503` when
four listeners are there already.

## On a microcontroller

```
GET /api/v1/status HTTP/1.0
Host: node
X-API-Key: tc_...

```

HTTP/1.0 is fine, there is no TLS, no chunked encoding, and `Content-Length`
is always there. The status is 2–8 kB of JSON (more with `remote_receiver`
and a full history): a streaming parser that picks the few fields it shows
keeps it out of RAM.
