# TODO

Things that are wanted and not there yet.

## EchoLink module

The repeaters that came from SvxLink lost their EchoLink node when they moved
to TailChaser (ON0OST-L, ON0UHF-R). An EchoLink module of TailChaser's own:

- an EchoLink node in the repeater program: registers with the EchoLink
  directory under its own callsign (`-L` / `-R`), accepts incoming connections
  and can call out;
- configurable in its own section, off by default: callsign, password, sysop
  name, location, description, how many stations at once, who may connect;
- **where an EchoLink station is heard and what it hears — a setting:**
  - `local`: only this repeater. The EchoLink station hears the repeater's own
    input and is transmitted on the repeater, and nothing of it goes to the
    reflector talkgroup (no network);
  - `network`: as a station on the repeater — it also goes to the talkgroup
    and hears what comes from it;
- an EchoLink station identified on the air like any other over that is not
  local (callsign in the status page and the history).
