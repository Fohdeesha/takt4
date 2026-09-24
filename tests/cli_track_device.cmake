# `takt4-cli track --device` on this machine's default input, for a few seconds: the console's
# live path, which is the application's own chain (`LiveTracker` and `OutputRunner`) since the
# audit's Low items. Run by ctest with -DCLI=<takt4-cli>. Opens a real input, so it is a
# `hardware` test; with no input device at all it is skipped rather than failed.

execute_process(COMMAND "${CLI}" devices OUTPUT_VARIABLE listing RESULT_VARIABLE listed)
if(NOT listed EQUAL 0)
  message(FATAL_ERROR "takt4-cli devices failed (${listed}):\n${listing}")
endif()
string(REGEX MATCH "\n *([0-9]+) [^\n]*default\\)" found "${listing}")
if(NOT found)
  message("no default input device on this machine:\n${listing}")
  message(FATAL_ERROR "SKIP")
endif()
set(device "${CMAKE_MATCH_1}")

execute_process(COMMAND "${CLI}" track --device "${device}" --channel 1 --seconds 3
                OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE tracked)
message("${out}${err}")
if(NOT tracked EQUAL 0)
  message(FATAL_ERROR "takt4-cli track --device ${device} exited with ${tracked}")
endif()
# Three seconds is 150 hops of 20 ms and 300 decoder frames; a chain that opened the stream and
# fed nothing would say 0.
if(NOT out MATCHES "stopped after ([0-9]+) hops, ([0-9]+) frames")
  message(FATAL_ERROR "no summary line")
endif()
if(CMAKE_MATCH_1 LESS 100 OR CMAKE_MATCH_2 LESS 100)
  message(FATAL_ERROR "only ${CMAKE_MATCH_1} hops and ${CMAKE_MATCH_2} frames in three seconds")
endif()
