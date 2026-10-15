# MIDI timing measurements

## `held-up-deliveries.txt`

How late each of 2081 MIDI clock ticks reached takt4's input, in microseconds, one a line, in
the order they came.

Measured on 2026-10-15 on a Windows 10 desktop. `takt4-cli follow --metronome 174` sent its MIDI
clock to a teVirtualMIDI port, and a small program on that port sent every message straight back.
`takt4-cli midi-in` listened to the port's input. Both ends stamped each message on the same
clock (QueryPerformanceCounter, which is what Link's clock reads on Windows). Each number is
takt4's stamp less the moment the echo received the tick.

Most ticks took 30 to 50 µs. The machine was busy for part of the run: 183 ticks arrived more
than a millisecond late, up to 28.7 ms, and up to sixteen of them in a row. None arrived early.

`tests/timing/tick_fit_test.cpp` plays a clock through these delays.
