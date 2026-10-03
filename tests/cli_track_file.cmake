# `takt4-cli track` over the "01 - Pirates" excerpt, as it ships: the weights built into it, and
# the decoder and the settings it uses when nobody asks for anything else. Run by ctest with
# -DCLI=<takt4-cli> -DWAV=<pirates.wav>.
#
# A script rather than a pass expression, because a pass expression is all ctest then looks at:
# this test was one, matching the header line — printed before tracking starts — so a console
# that crashed a frame later, or tracked nothing, passed. And it asked for the particle filter,
# which has not been the default since 2026-09-08 (the audit of 2026-09-25, T10).

execute_process(COMMAND "${CLI}" track "${WAV}"
                OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE tracked)
message("${out}${err}")
if(NOT tracked EQUAL 0)
  message(FATAL_ERROR "takt4-cli track exited with ${tracked}")
endif()

# From inside it, not from this tree — the audit's H19, which is why this test exists at all.
if(NOT out MATCHES "weights electronic \\(built in\\)")
  message(FATAL_ERROR "not tracked with the weights built into the console")
endif()
# The decoder that ships, whatever it is called on the day: the header names it, and the
# default must not be the particle filter.
if(out MATCHES "particle filter")
  message(FATAL_ERROR "tracked with the particle filter, which is not the default")
endif()
if(NOT out MATCHES "forward filter")
  message(FATAL_ERROR "the header does not name the forward filter")
endif()

# And what it tracked: Beat This! puts the excerpt at 136.36 BPM in 4/4 (the engine test "the
# configuration that ships tracks real music at the tempo it is played" uses the same reference),
# and the console ends locked on it — 137.3 on 2026-10-03. Two and a half per cent either way is
# that test's tolerance; a wrong octave or a different tempo is not.
#
# This was the synthetic drum machine until 2026-10-03, ending locked at 128.3 — by luck: with the
# window off the same build never locked on it, the decoder's tempo dipping as each beat arrived
# and restarting the half second of agreement a lock needs. A pure drum machine now locks in about
# eleven seconds, the operator's choice of the strict lock over one that also locks wrong tempi
# more often, and its ten seconds are not enough.
if(NOT out MATCHES "\n([0-9]+) frames, ([0-9]+) beats \\(([0-9]+) downbeats\\), ending at ([0-9.]+) BPM in ([0-9]+)/4, (locked|hunting)")
  message(FATAL_ERROR "no summary line")
endif()
set(frames "${CMAKE_MATCH_1}")
set(beats "${CMAKE_MATCH_2}")
set(bpm "${CMAKE_MATCH_4}")
set(meter "${CMAKE_MATCH_5}")
set(state "${CMAKE_MATCH_6}")
if(frames LESS 900 OR beats LESS 18)
  message(FATAL_ERROR "only ${frames} frames and ${beats} beats over ten seconds")
endif()
if(bpm LESS 132.95 OR bpm GREATER 139.77)
  message(FATAL_ERROR "ended at ${bpm} BPM, not near 136.36")
endif()
if(NOT meter EQUAL 4 OR NOT state STREQUAL "locked")
  message(FATAL_ERROR "ended ${state} in ${meter}/4, not locked in 4/4")
endif()
