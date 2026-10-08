# RtMidi 6.0.0's WinMM input, patched at configure time — the audit's M14.
#
# `MidiInWinMM::closePort` takes the input's critical section and, holding it, calls
# `midiInReset`, which hands every pending SysEx buffer back through the input callback — and
# the callback takes the same critical section to queue the buffer again. The driver's thread
# waits on the lock, and closePort waits on the driver. takt4 opens the MOTU's MIDI In as a
# control port, so this is the close on Stop, on a port change and on the way out. The error
# path of the same function also returned — or threw, since RtMidi's `error` throws — with the
# lock still held, so the next close would wait for ever.
#
# Upstream fixed the first on master (`e06e12bd`, "WinMM: avoid deadlock", after 6.0.0): a
# `closing` flag set before the lock is taken, and the callback not requeueing while it is set.
# That fix is taken here as it stands, with two things it leaves out: the flag is cleared again
# when a port is opened — upstream never clears it, so after a close and a reopen no SysEx
# buffer would ever be requeued — and the error path lets go of the lock before it reports.
#
# Written like pa_asio_patch.cmake: the fetched file is not touched, the patched copy goes to
# the build tree, and every edit is an exact-text replacement that stops the configure if
# RtMidi's text has changed, so a patch cannot silently stop applying. Drop it when takt4 moves
# to an RtMidi release that carries e06e12bd.

set(rtmidi_in "${rtmidi_SOURCE_DIR}/RtMidi.cpp")
set(rtmidi_out "${PROJECT_BINARY_DIR}/generated/rtmidi/RtMidi.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${rtmidi_in}" "${CMAKE_CURRENT_LIST_FILE}")

file(READ "${rtmidi_in}" rtmidi_source)
string(REPLACE "\r\n" "\n" rtmidi_patched "${rtmidi_source}")

function(takt4_patch_rtmidi what anchor replacement)
  string(FIND "${rtmidi_patched}" "${anchor}" first)
  string(FIND "${rtmidi_patched}" "${anchor}" last REVERSE)
  if(first EQUAL -1 OR NOT first EQUAL last)
    message(FATAL_ERROR
      "cmake/rtmidi_patch.cmake: the text for \"${what}\" is not in ${rtmidi_in} exactly once. "
      "RtMidi has changed; re-check the patch against it before building.")
  endif()
  string(REPLACE "${anchor}" "${replacement}" patched "${rtmidi_patched}")
  set(rtmidi_patched "${patched}" PARENT_SCOPE)
endfunction()

takt4_patch_rtmidi("the closing flag" [==[
  CRITICAL_SECTION _mutex; // [Patrice] see https://groups.google.com/forum/#!topic/mididev/6OUjHutMpEo
};]==] [==[
  CRITICAL_SECTION _mutex; // [Patrice] see https://groups.google.com/forum/#!topic/mididev/6OUjHutMpEo
  // takt4 (cmake/rtmidi_patch.cmake, from upstream e06e12bd): set while closePort runs, so
  // the input callback neither requeues a buffer nor waits on _mutex, which closePort holds.
  std::atomic<bool> closing{false};
};]==])

takt4_patch_rtmidi("the callback's requeue, begun" [==[
      EnterCriticalSection( &(apiData->_mutex) );
      MMRESULT result = midiInAddBuffer( apiData->inHandle, apiData->sysexBuffer[sysex->dwUser], sizeof(MIDIHDR) );]==] [==[
      MMRESULT result = MMSYSERR_NOERROR;
      if ( !apiData->closing ) {  // takt4 (cmake/rtmidi_patch.cmake)
      EnterCriticalSection( &(apiData->_mutex) );
      result = midiInAddBuffer( apiData->inHandle, apiData->sysexBuffer[sysex->dwUser], sizeof(MIDIHDR) );]==])

takt4_patch_rtmidi("the callback's requeue, ended" [==[
      LeaveCriticalSection( &(apiData->_mutex) );
      if ( result != MMSYSERR_NOERROR )]==] [==[
      LeaveCriticalSection( &(apiData->_mutex) );
      }
      if ( result != MMSYSERR_NOERROR )]==])

takt4_patch_rtmidi("closePort raises the flag" [==[
    WinMidiData *data = static_cast<WinMidiData *> (apiData_);
    EnterCriticalSection( &(data->_mutex) );
    midiInReset( data->inHandle );]==] [==[
    WinMidiData *data = static_cast<WinMidiData *> (apiData_);
    data->closing = true;  // takt4 (cmake/rtmidi_patch.cmake): before the lock, not under it
    EnterCriticalSection( &(data->_mutex) );
    midiInReset( data->inHandle );]==])

takt4_patch_rtmidi("closePort's error path lets go of the lock" [==[
        midiInClose( data->inHandle );
        data->inHandle = 0;
        errorString_ = "MidiInWinMM::openPort: error closing Windows MM MIDI input port (midiInUnprepareHeader).";
        error( RtMidiError::DRIVER_ERROR, errorString_ );]==] [==[
        midiInClose( data->inHandle );
        data->inHandle = 0;
        connected_ = false;                        // takt4 (cmake/rtmidi_patch.cmake): the
        LeaveCriticalSection( &(data->_mutex) );   // lock is let go before `error` throws
        errorString_ = "MidiInWinMM::openPort: error closing Windows MM MIDI input port (midiInUnprepareHeader).";
        error( RtMidiError::DRIVER_ERROR, errorString_ );]==])

takt4_patch_rtmidi("openPort lowers the flag" [==[
  WinMidiData *data = static_cast<WinMidiData *> (apiData_);
  MMRESULT result = midiInOpen( &data->inHandle,]==] [==[
  WinMidiData *data = static_cast<WinMidiData *> (apiData_);
  data->closing = false;  // takt4 (cmake/rtmidi_patch.cmake): upstream never clears it
  MMRESULT result = midiInOpen( &data->inHandle,]==])

# **CoreMIDI's client, created where an error may be thrown** (macOS; the first macOS runs,
# 2026-10-08). `getCoreMidiClientSingleton` is declared `throw()` and reports a client it could
# not create through `error`, which throws when no error callback is set — and none can be,
# inside RtMidi's own constructor, where this is called. RtMidi is C++11, so the exception left
# through the `throw()` into std::unexpected and ended the program: on a Mac whose MIDI server
# would not start, every RtMidiIn and RtMidiOut was a crash rather than an RtMidiError takt4
# reports. Without the `throw()`, it reaches the caller as RtMidi's other errors do.
takt4_patch_rtmidi("the input's client, declared" [==[
  std::string getPortName( unsigned int portNumber );

 protected:
  MIDIClientRef getCoreMidiClientSingleton(const std::string& clientName) throw();]==] [==[
  std::string getPortName( unsigned int portNumber );

 protected:
  MIDIClientRef getCoreMidiClientSingleton(const std::string& clientName);  // takt4: no throw()]==])

takt4_patch_rtmidi("the output's client, declared" [==[
  void sendMessage( const unsigned char *message, size_t size );

 protected:
  MIDIClientRef getCoreMidiClientSingleton(const std::string& clientName) throw();]==] [==[
  void sendMessage( const unsigned char *message, size_t size );

 protected:
  MIDIClientRef getCoreMidiClientSingleton(const std::string& clientName);  // takt4: no throw()]==])

takt4_patch_rtmidi("the input's client, defined" [==[
MIDIClientRef MidiInCore::getCoreMidiClientSingleton(const std::string& clientName) throw() {]==] [==[
MIDIClientRef MidiInCore::getCoreMidiClientSingleton(const std::string& clientName) {  // takt4]==])

takt4_patch_rtmidi("the output's client, defined" [==[
MIDIClientRef MidiOutCore::getCoreMidiClientSingleton(const std::string& clientName) throw() {]==] [==[
MIDIClientRef MidiOutCore::getCoreMidiClientSingleton(const std::string& clientName) {  // takt4]==])

set(rtmidi_existing "")
if(EXISTS "${rtmidi_out}")
  file(READ "${rtmidi_out}" rtmidi_existing)
endif()
if(NOT rtmidi_existing STREQUAL rtmidi_patched)
  file(WRITE "${rtmidi_out}" "${rtmidi_patched}")
endif()

# The patched copy in place of the original in RtMidi's own target, with the original's
# directory searched for RtMidi.h.
get_target_property(rtmidi_sources rtmidi SOURCES)
list(FIND rtmidi_sources "RtMidi.cpp" rtmidi_at)
if(rtmidi_at EQUAL -1)
  message(FATAL_ERROR "cmake/rtmidi_patch.cmake: RtMidi's target no longer lists RtMidi.cpp")
endif()
list(REMOVE_AT rtmidi_sources ${rtmidi_at})
list(INSERT rtmidi_sources ${rtmidi_at} "${rtmidi_out}")
set_property(TARGET rtmidi PROPERTY SOURCES "${rtmidi_sources}")
target_include_directories(rtmidi PRIVATE "${rtmidi_SOURCE_DIR}")

unset(rtmidi_source)
unset(rtmidi_patched)
unset(rtmidi_existing)
unset(rtmidi_sources)
