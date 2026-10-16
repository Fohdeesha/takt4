# Pro DJ Link packets from real players

What takt4's Pro DJ Link code (`src/core/timing/prolink/`) is tested against: datagrams that
Pioneer players and mixers sent to each other, cut from packet captures and written out one to a
line as text. Every byte in a datagram is as it was captured; nothing was edited or made up.

Each line is the seconds since the excerpt's first datagram, the UDP port it went to (50000
announcements, 50001 beats, 50002 status), the sender's address, the receiver's address, and the
datagram in hex. Lines starting with `#` say what the file shows. Player and mixer status — five
a second from every device — is thinned in the longer files to every change of state plus one
each half second per device, as each file's header says; no other datagram is left out.

| File | Hardware | What happens |
|---|---|---|
| `cold-boot.txt` | CDJ-2000nexus, firmware 1.44 | A player starts up alone: hello, the three stages of its claim, keep-alives |
| `joins-occupied.txt` | two CDJ-2000nexus, firmware 1.44 | A player starts onto a network another holds; its claim is cut short by "assignment finished" |
| `load-and-play.txt` | two CDJ-2000nexus, firmware 1.44 | A track played for 30 s, paused, hot cues and cue pressed while paused, played again |
| `nexus-and-djm.txt` | two CDJ-2000nexus, a DJM-2000nexus | Status (0xd4 bytes) to a virtual player; the mixer's beats as a backup metronome |
| `master-handoff.txt` | two CDJ-2000nexus, a DJM-2000nexus | Tempo master handed from player to player and to the mixer |

## Where they come from

All five are excerpts of captures published by Deep Symmetry in the
[dysentery](https://github.com/Deep-Symmetry/dysentery) repository, the work behind the DJ Link
packet analysis takt4's implementation follows:

- `cold-boot.txt`, `joins-occupied.txt` and `load-and-play.txt`: `doc/assets/captures/`
  sessions S01, S13 and S06, recorded on 2026-07-29 on two CDJ-2000nexus players (the
  captures' own README describes the rig).
- `nexus-and-djm.txt`: `doc/assets/to-virtual.pcapng`.
- `master-handoff.txt`: `doc/assets/Sync-Master.pcapng`.

dysentery is published under the Eclipse Public License 1.0. These files are recordings of what
the hardware sent, reproduced so that takt4 can be tested against real devices it does not have,
and no dysentery code is used in takt4. If you hold rights in these recordings and want them
taken out, open an issue and they will be removed.
