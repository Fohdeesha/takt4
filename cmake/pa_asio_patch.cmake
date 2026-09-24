# PortAudio's ASIO host, patched at configure time — the audit's C4 and C5, option (a) of its
# Q9 (2026-09-23): keep PortAudio as the ASIO host and give it the three things it lacks.
#
#   1. The device's "default sample rate" is the rate the interface is **running at now**.
#      Upstream reports the first rate the driver accepts from a fixed list that starts at
#      44100, which is not the interface's clock at all (PortAudio issue #970, PR #971).
#   2. Opening a stream **never re-clocks the interface**. Upstream calls ASIOSetSampleRate
#      whenever the requested rate differs from the current one, so pressing Start with Live
#      or a front-of-house mix running on the MOTU at 48 kHz moved everyone to 44.1 kHz. Here
#      the open is refused instead, with the rate the interface is running at left where
#      takt4 can read it (PaAsio_Takt4_ClockedRate) — and takt4 opens again at that rate,
#      which costs it nothing because it resamples everything to 22050 Hz anyway.
#   3. What the driver says is **kept rather than dropped**: a reset request, a buffer-size
#      change, a sample-rate change and a resync each set a bit that takt4 reads and clears
#      (PaAsio_Takt4_TakeDriverEvents) and answers by reopening the stream. Upstream
#      acknowledges all four and does nothing ("FIXME … ticket #108"; #472, PR #519).
#   4. A process with TAKT4_NO_ASIO in its environment **gets no ASIO host at all**. The test
#      binaries set it (tests/support/crt_dialogs.cpp) unless TAKT4_TEST_HARDWARE is set:
#      every window test builds a real tracker, a tracker enumerates the devices, and
#      enumerating ASIO loads and initialises every installed driver — sixty times over, on the
#      machine a show may be running from (the audit's T2).
#   5. PortAudio can be handed **what each driver would have said**, and then loads none of
#      them to find out (PaAsio_Takt4_SetProbe, src/core/audio/asio_probe.h). takt4 asks the
#      drivers from a process of its own and hands the answers over, so that a driver which
#      falls over as it is released — the MOTU's does, now and then — takes that process with
#      it rather than takt4 (src/core/audio/asio_scan.hpp).
#
# Written like the asiolist.cpp patch in asiosdk.cmake: the vendored file is never touched,
# the patched copy goes to the build tree, and every edit is an exact-text replacement that
# stops the configure if upstream has changed the text it expects — a patch that silently
# stopped applying would put back exactly the behaviour it exists to remove.

set(pa_asio_in "${TAKT4_THIRD_PARTY_DIR}/portaudio/src/hostapi/asio/pa_asio.cpp")
set(pa_asio_out "${PROJECT_BINARY_DIR}/generated/portaudio/pa_asio.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${pa_asio_in}" "${CMAKE_CURRENT_LIST_FILE}")

file(READ "${pa_asio_in}" pa_asio_source)
# Line endings depend on how the submodule was checked out — CRLF on this machine, LF on a
# runner with autocrlf off — and every anchor below spans lines. Normalised first, so the
# patch applies to either; the compiler does not care which it gets.
string(REPLACE "\r\n" "\n" pa_asio_patched "${pa_asio_source}")

# Replaces the one occurrence of `anchor` in pa_asio_patched, and fails if there is not
# exactly one.
function(takt4_patch_pa_asio what anchor replacement)
  string(FIND "${pa_asio_patched}" "${anchor}" first)
  string(FIND "${pa_asio_patched}" "${anchor}" last REVERSE)
  if(first EQUAL -1 OR NOT first EQUAL last)
    message(FATAL_ERROR
      "cmake/pa_asio_patch.cmake: the text for \"${what}\" is not in ${pa_asio_in} exactly "
      "once. PortAudio has changed; re-check the patch against it before building.")
  endif()
  string(REPLACE "${anchor}" "${replacement}" patched "${pa_asio_patched}")
  set(pa_asio_patched "${patched}" PARENT_SCOPE)
endfunction()

takt4_patch_pa_asio("the driver-event state"
"static long asioMessages(long selector, long value, void* message, double* opt);
"
"static long asioMessages(long selector, long value, void* message, double* opt);

/* takt4 (cmake/pa_asio_patch.cmake): what the driver told the host, kept for takt4 to read
   rather than dropped, and the rate a refused open found the interface running at. The
   driver calls in on its own thread, so the bits are set with interlocked operations. */
static volatile LONG takt4_driverEvents_ = 0;
static volatile double takt4_clockedRate_ = 0.;

/* takt4: what each driver would say, from a scan made in another process; null to ask them. */
#include \"asio_probe.h\"
static PaAsioTakt4Probe takt4_probe_ = 0;
")

takt4_patch_pa_asio("describe a driver without loading it"
"    asioDeviceInfo->asioChannelInfos = 0; /* we check this below to handle error cleanup */

    result = LoadAsioDriver( asioHostApi, driverName, &paAsioDriver.info, asioHostApi->systemSpecific );"
"    asioDeviceInfo->asioChannelInfos = 0; /* we check this below to handle error cleanup */

    /* takt4: described by a scan that asked the driver in another process, so it is not loaded
       here at all — a driver that falls over as it is released takes that process with it. */
    if( takt4_probe_ )
    {
        PaAsioTakt4Device probed;
        memset( &probed, 0, sizeof(probed) );
        if( !takt4_probe_( driverName, &probed ) )
            return paDeviceUnavailable;
        deviceInfo->maxInputChannels = probed.inputChannels;
        deviceInfo->maxOutputChannels = probed.outputChannels;
        deviceInfo->defaultSampleRate = probed.defaultSampleRate;
        deviceInfo->defaultLowInputLatency = probed.defaultLowInputLatency;
        deviceInfo->defaultLowOutputLatency = probed.defaultLowOutputLatency;
        deviceInfo->defaultHighInputLatency = probed.defaultHighInputLatency;
        deviceInfo->defaultHighOutputLatency = probed.defaultHighOutputLatency;
        asioDeviceInfo->minBufferSize = probed.minBufferSize;
        asioDeviceInfo->maxBufferSize = probed.maxBufferSize;
        asioDeviceInfo->preferredBufferSize = probed.preferredBufferSize;
        asioDeviceInfo->bufferGranularity = probed.bufferGranularity;
        const long channels = probed.inputChannels + probed.outputChannels;
        if( channels > 0 )
        {
            asioDeviceInfo->asioChannelInfos = (ASIOChannelInfo*)PaUtil_GroupAllocateZeroInitializedMemory(
                    asioHostApi->allocations, sizeof(ASIOChannelInfo) * channels );
            if( !asioDeviceInfo->asioChannelInfos )
                return paInsufficientMemory;
            for( long a = 0; a < channels; ++a )
            {
                ASIOChannelInfo *channel = &asioDeviceInfo->asioChannelInfos[a];
                channel->isInput = a < probed.inputChannels ? ASIOTrue : ASIOFalse;
                channel->channel = a < probed.inputChannels ? a : a - probed.inputChannels;
                if( probed.channelNames && probed.channelNames[a] )
                {
                    /* Zeroed above, so the name stays terminated. */
                    size_t length = strlen( probed.channelNames[a] );
                    if( length > sizeof(channel->name) - 1 )
                        length = sizeof(channel->name) - 1;
                    memcpy( channel->name, probed.channelNames[a], length );
                }
            }
        }
        return paNoError;
    }

    result = LoadAsioDriver( asioHostApi, driverName, &paAsioDriver.info, asioHostApi->systemSpecific );")

takt4_patch_pa_asio("the current rate as the default rate"
"        deviceInfo->defaultSampleRate = 0.;
        bool foundDefaultSampleRate = false;
        for( int j=0; j < PA_DEFAULTSAMPLERATESEARCHORDER_COUNT_; ++j )"
"        deviceInfo->defaultSampleRate = 0.;
        bool foundDefaultSampleRate = false;
        /* takt4: the rate the interface is running at now, when the driver will say. The
           list below is only a fallback for a driver that will not. */
        {
            ASIOSampleRate currentRate = 0.;
            if( ASIOGetSampleRate( &currentRate ) == ASE_OK && currentRate > 0.
                    && ASIOCanSampleRate( currentRate ) == ASE_OK )
            {
                deviceInfo->defaultSampleRate = currentRate;
                foundDefaultSampleRate = true;
            }
        }
        for( int j=0; !foundDefaultSampleRate && j < PA_DEFAULTSAMPLERATESEARCHORDER_COUNT_; ++j )")

takt4_patch_pa_asio("never re-clock the interface"
"    if (oldRate != sampleRate){
        /* Set sample rate */"
"    /* takt4: never move a shared interface's clock. Refused, with the rate it is running at
       left for PaAsio_Takt4_ClockedRate, so the caller can open again at that rate. */
    if (oldRate != sampleRate){
        takt4_clockedRate_ = oldRate;
        result = paInvalidSampleRate;
        goto error;
    }
    if (oldRate != sampleRate){
        /* Set sample rate */")

takt4_patch_pa_asio("keep a sample-rate change"
"    (void) sRate; /* unused parameter */"
"    /* takt4: kept, for PaAsio_Takt4_TakeDriverEvents. */
    takt4_clockedRate_ = sRate;
    InterlockedOr( &takt4_driverEvents_, 0x4 );")

takt4_patch_pa_asio("keep a buffer-size change"
"        case kAsioBufferSizeChange:
            //printf(\"kAsioBufferSizeChange \\n\");
            break;"
"        case kAsioBufferSizeChange:
            //printf(\"kAsioBufferSizeChange \\n\");
            /* takt4: kept. Still answered 0, so a driver that needs the buffers rebuilt
               follows with kAsioResetRequest, which is kept too. */
            InterlockedOr( &takt4_driverEvents_, 0x2 );
            break;")

takt4_patch_pa_asio("keep a reset request"
"            //asioDriverInfo.stopped;  // In this sample the processing will just stop"
"            //asioDriverInfo.stopped;  // In this sample the processing will just stop
            /* takt4: kept; takt4 answers it by closing and reopening the stream. */
            InterlockedOr( &takt4_driverEvents_, 0x1 );")

takt4_patch_pa_asio("keep a resync request"
"            // However a driver can issue it in other situations, too.
            ret = 1L;"
"            // However a driver can issue it in other situations, too.
            InterlockedOr( &takt4_driverEvents_, 0x8 ); /* takt4: kept, for the record */
            ret = 1L;")

string(APPEND pa_asio_patched "
/* takt4 (cmake/pa_asio_patch.cmake): the reading side of the state above, declared for takt4
   in src/core/audio/asio_driver.hpp. */
extern \"C\" unsigned long PaAsio_Takt4_TakeDriverEvents( void )
{
    return (unsigned long)InterlockedExchange( &takt4_driverEvents_, 0 );
}

extern \"C\" double PaAsio_Takt4_ClockedRate( void )
{
    return takt4_clockedRate_;
}

extern \"C\" void PaAsio_Takt4_ForgetClockedRate( void )
{
    takt4_clockedRate_ = 0.;
}

extern \"C\" void PaAsio_Takt4_SetProbe( PaAsioTakt4Probe probe )
{
    takt4_probe_ = probe;
}
")

takt4_patch_pa_asio("no ASIO in a test process"
"PaError PaAsio_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex hostApiIndex )
{
    PaError result = paNoError;"
"PaError PaAsio_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex hostApiIndex )
{
    /* takt4 (cmake/pa_asio_patch.cmake): a process that asks for no ASIO gets none. The test
       binaries ask, unless they are run against the rig on purpose (tests/support/
       crt_dialogs.cpp). A null host API is one PortAudio skips. */
    if( GetEnvironmentVariableA( \"TAKT4_NO_ASIO\", NULL, 0 ) != 0 )
    {
        *hostApi = NULL;
        return paNoError;
    }
    PaError result = paNoError;")

# Only rewrite when the content changes, so a reconfigure does not force a rebuild.
set(pa_asio_existing "")
if(EXISTS "${pa_asio_out}")
  file(READ "${pa_asio_out}" pa_asio_existing)
endif()
if(NOT pa_asio_existing STREQUAL pa_asio_patched)
  file(WRITE "${pa_asio_out}" "${pa_asio_patched}")
endif()

# The patched copy in place of the original in PortAudio's own target. It lives in the build
# tree now, so the directory beside the original has to be searched for the headers it pulls
# in by a quoted name (iasiothiscallresolver.h).
get_target_property(pa_sources portaudio SOURCES)
list(FIND pa_sources "src/hostapi/asio/pa_asio.cpp" pa_asio_at)
if(pa_asio_at EQUAL -1)
  message(FATAL_ERROR
    "cmake/pa_asio_patch.cmake: PortAudio's target no longer lists src/hostapi/asio/pa_asio.cpp")
endif()
list(REMOVE_AT pa_sources ${pa_asio_at})
list(INSERT pa_sources ${pa_asio_at} "${pa_asio_out}")
set_property(TARGET portaudio PROPERTY SOURCES "${pa_sources}")
target_include_directories(portaudio PRIVATE "${TAKT4_THIRD_PARTY_DIR}/portaudio/src/hostapi/asio")
# And takt4's own header for the fifth part, the one thing the patch shares with takt4's code.
target_include_directories(portaudio PRIVATE "${PROJECT_SOURCE_DIR}/src/core/audio")

unset(pa_asio_source)
unset(pa_asio_patched)
unset(pa_asio_existing)
unset(pa_sources)
