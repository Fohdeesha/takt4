#include "core/audio/asio_driver.hpp"

#if defined(_WIN32)
// Defined at the end of PortAudio's patched pa_asio.cpp (cmake/pa_asio_patch.cmake).
extern "C" unsigned long PaAsio_Takt4_TakeDriverEvents(void);
extern "C" double PaAsio_Takt4_ClockedRate(void);
extern "C" void PaAsio_Takt4_ForgetClockedRate(void);
#endif

namespace takt4::audio {

AsioDriverEvents takeAsioDriverEvents() noexcept {
    AsioDriverEvents events;
#if defined(_WIN32)
    const unsigned long bits = PaAsio_Takt4_TakeDriverEvents();
    events.resetRequest = (bits & 0x1U) != 0;
    events.bufferSizeChange = (bits & 0x2U) != 0;
    events.sampleRateChange = (bits & 0x4U) != 0;
    events.resync = (bits & 0x8U) != 0;
#endif
    return events;
}

double asioClockedRate() noexcept {
#if defined(_WIN32)
    return PaAsio_Takt4_ClockedRate();
#else
    return 0.0;
#endif
}

void forgetAsioClockedRate() noexcept {
#if defined(_WIN32)
    PaAsio_Takt4_ForgetClockedRate();
#endif
}

} // namespace takt4::audio
