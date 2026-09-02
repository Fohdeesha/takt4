# Steinberg ASIO SDK, vendored in third_party/asiosdk and used under its GPLv3 option.
#
# Defines ASIO::host in the same shape PortAudio's own FindASIO.cmake produces. Because
# the target already exists when PortAudio configures, PortAudio's
# `if(PA_USE_ASIO AND TARGET ASIO::host)` branch uses the vendored SDK and never reaches
# its fallback of downloading the SDK from steinberg.net at configure time.
#
# host/pc/asiolist.cpp frees an array allocated with new[] using plain delete. PortAudio
# patches that line too. The patched copy is written to the build tree so the vendored
# SDK stays byte-identical to Steinberg's package.

if(NOT WIN32)
  message(FATAL_ERROR "cmake/asiosdk.cmake is Windows-only")
endif()

set(asiosdk_root "${PROJECT_SOURCE_DIR}/third_party/asiosdk")
set(asiosdk_gen "${PROJECT_BINARY_DIR}/generated/asiosdk")

foreach(required IN ITEMS
  common/asio.h
  common/iasiodrv.h
  common/asio.cpp
  host/asiodrivers.cpp
  host/pc/asiolist.cpp
)
  if(NOT EXISTS "${asiosdk_root}/${required}")
    message(FATAL_ERROR "third_party/asiosdk/${required} is missing")
  endif()
endforeach()

set(asiolist_in "${asiosdk_root}/host/pc/asiolist.cpp")
set(asiolist_out "${asiosdk_gen}/patched_asiolist.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${asiolist_in}")

file(READ "${asiolist_in}" asiolist_source)
string(REPLACE "delete lpdrv" "delete[] lpdrv" asiolist_patched "${asiolist_source}")
if(asiolist_patched STREQUAL asiolist_source)
  message(FATAL_ERROR
    "${asiolist_in} no longer contains the 'delete lpdrv' this patch expects. "
    "Check whether the SDK fixed it upstream and update cmake/asiosdk.cmake."
  )
endif()

# Only rewrite when the content changes, so a reconfigure doesn't force a rebuild.
set(asiolist_existing "")
if(EXISTS "${asiolist_out}")
  file(READ "${asiolist_out}" asiolist_existing)
endif()
if(NOT asiolist_existing STREQUAL asiolist_patched)
  file(WRITE "${asiolist_out}" "${asiolist_patched}")
endif()

add_library(ASIO::host INTERFACE IMPORTED)
target_sources(ASIO::host INTERFACE
  "${asiosdk_root}/common/asio.cpp"
  "${asiosdk_root}/host/asiodrivers.cpp"
  "${asiolist_out}"
)
target_include_directories(ASIO::host SYSTEM INTERFACE
  "${asiosdk_root}/common"
  "${asiosdk_root}/host"
  "${asiosdk_root}/host/pc"
)
target_link_libraries(ASIO::host INTERFACE ole32 uuid)
