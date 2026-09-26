#include "core/audio/asio_driver.hpp"

#include <atomic>

#if defined(_WIN32)
// Defined at the end of PortAudio's patched pa_asio.cpp (cmake/pa_asio_patch.cmake).
extern "C" unsigned long PaAsio_Takt4_TakeDriverEvents(void);
extern "C" double PaAsio_Takt4_ClockedRate(void);
extern "C" void PaAsio_Takt4_ForgetClockedRate(void);
#endif

namespace takt4::audio {

namespace {

// The patch's bits: what a test posts goes into the same word the driver's messages do.
constexpr unsigned long kResetRequest = 0x1U;
constexpr unsigned long kBufferSizeChange = 0x2U;
constexpr unsigned long kSampleRateChange = 0x4U;
constexpr unsigned long kResync = 0x8U;

std::atomic<unsigned long> g_posted{0};
std::atomic<double> g_postedRate{0.0};

} // namespace

AsioDriverEvents takeAsioDriverEvents() noexcept {
    AsioDriverEvents events;
    unsigned long bits = g_posted.exchange(0);
    double rate = g_postedRate.exchange(0.0);
#if defined(_WIN32)
    const unsigned long driver = PaAsio_Takt4_TakeDriverEvents();
    if ((driver & kSampleRateChange) != 0) {
        // The rate the message named is kept beside the bit (the patch's "keep a sample-rate
        // change"). An open refused after the message would put another rate there, but the
        // window takes what the driver said after every open (`WindowController::watchOpenedInput`),
        // so a message still here is newer than any open.
        rate = PaAsio_Takt4_ClockedRate();
    }
    bits |= driver;
#endif
    events.resetRequest = (bits & kResetRequest) != 0;
    events.bufferSizeChange = (bits & kBufferSizeChange) != 0;
    events.sampleRateChange = (bits & kSampleRateChange) != 0;
    events.resync = (bits & kResync) != 0;
    events.reportedRate = events.sampleRateChange ? rate : 0.0;
    return events;
}

void postAsioDriverEvents(const AsioDriverEvents& events) noexcept {
    unsigned long bits = 0;
    bits |= events.resetRequest ? kResetRequest : 0U;
    bits |= events.bufferSizeChange ? kBufferSizeChange : 0U;
    bits |= events.sampleRateChange ? kSampleRateChange : 0U;
    bits |= events.resync ? kResync : 0U;
    if (events.sampleRateChange) {
        g_postedRate.store(events.reportedRate);
    }
    g_posted.fetch_or(bits);
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
