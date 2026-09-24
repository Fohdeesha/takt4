/* What takt4 hands PortAudio's ASIO host in place of loading each driver to ask it.
 *
 * C, because the patched pa_asio.cpp (cmake/pa_asio_patch.cmake) includes it as well as
 * takt4's scan (asio_scan.cpp). The scan asks every driver in a process of its own, and this
 * carries what it found into the one that uses it: see asio_scan.hpp for why. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* One ASIO driver as the scanning process's PortAudio described it. */
typedef struct PaAsioTakt4Device {
    long inputChannels;
    long outputChannels;
    long minBufferSize;
    long maxBufferSize;
    long preferredBufferSize;
    long bufferGranularity;
    double defaultSampleRate;
    double defaultLowInputLatency;
    double defaultLowOutputLatency;
    double defaultHighInputLatency;
    double defaultHighOutputLatency;
    /* inputChannels + outputChannels names, the inputs first. */
    const char* const* channelNames;
} PaAsioTakt4Device;

/* Fills `device` and returns nonzero when the scan found the driver called `driverName`;
   zero leaves the driver out, exactly as a driver that would not load is left out. */
typedef int (*PaAsioTakt4Probe)(const char* driverName, PaAsioTakt4Device* device);

/* Set before Pa_Initialize. Null — the default — has PortAudio load each driver itself. */
void PaAsio_Takt4_SetProbe(PaAsioTakt4Probe probe);

#ifdef __cplusplus
}
#endif
