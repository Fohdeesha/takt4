# **Skia's own copy of the C++ runtime's type information, kept to Skia** (macOS only).
#
# rust-skia's prebuilt skia-bindings ships an object, `<hash>-bindings.o`, that defines its own
# weak, hidden `typeinfo for std::exception` and `std::length_error` (and may define others of
# the standard library's classes). Linked into takt4 inside libslint_cpp.a, that copy wins over
# libc++abi's — the linker takes a definition in an object over one in a library — so every
# `catch (const std::exception&)` in takt4 compared against Skia's copy, while takt4's own
# exceptions descend from libc++abi's. macOS's runtime matches exception types by address, so
# nothing matched: a MIDI port that was not there ended the program (the first macOS runs,
# 2026-10-08; Linux and Windows compare names, and never noticed).
#
# So the standard library's typeinfo, vtables and names that the object defines are made local
# to it with Apple's nmedit: Skia's own code goes on using its copy, and everything else binds to
# libc++abi's. Every libslint_cpp*.a under the build tree is patched — the one CMake links and
# cargo's own, so Corrosion's copy-if-different finds them alike and nothing relinks for nothing —
# and an archive already patched is left alone.
#
#   cmake -DBINARY_DIR=<build tree> -P cmake/apple_skia_typeinfo.cmake

if(NOT BINARY_DIR)
  message(FATAL_ERROR "apple_skia_typeinfo.cmake: BINARY_DIR not given")
endif()

file(GLOB_RECURSE archives "${BINARY_DIR}/lib/libslint_cpp*.a" "${BINARY_DIR}/cargo/libslint_cpp*.a")
if(NOT archives)
  message(FATAL_ERROR "apple_skia_typeinfo.cmake: no libslint_cpp*.a under ${BINARY_DIR}")
endif()

set(patched_any FALSE)
foreach(archive IN LISTS archives)
  execute_process(COMMAND ar -t "${archive}" OUTPUT_VARIABLE listing RESULT_VARIABLE failed)
  if(failed)
    message(FATAL_ERROR "apple_skia_typeinfo.cmake: cannot list ${archive}")
  endif()
  string(REPLACE "\n" ";" members "${listing}")
  list(FILTER members INCLUDE REGEX "-bindings\\.o$")
  foreach(member IN LISTS members)
    get_filename_component(work "${archive}" DIRECTORY)
    set(work "${work}/typeinfo-patch")
    file(REMOVE_RECURSE "${work}")
    file(MAKE_DIRECTORY "${work}")
    execute_process(COMMAND ar -x "${archive}" "${member}" WORKING_DIRECTORY "${work}"
                    RESULT_VARIABLE failed)
    if(failed)
      message(FATAL_ERROR "apple_skia_typeinfo.cmake: cannot extract ${member} from ${archive}")
    endif()
    execute_process(COMMAND nm -m "${work}/${member}" OUTPUT_VARIABLE symbols)
    # The standard library's: typeinfo (_ZTI), its name (_ZTS) and vtables (_ZTV) of std::
    # classes, defined here as hidden ("private external"), weak or not.
    string(REGEX MATCHALL "private external (__ZT[ISV]St[0-9A-Za-z_]+)" found "${symbols}")
    list(TRANSFORM found REPLACE "private external " "")
    list(REMOVE_DUPLICATES found)
    if(NOT found)
      continue() # already patched, or a skia-bindings that no longer does this
    endif()
    list(JOIN found "\n" list_text)
    file(WRITE "${work}/local.txt" "${list_text}\n")
    execute_process(COMMAND nmedit -R "${work}/local.txt" "${work}/${member}"
                    RESULT_VARIABLE failed ERROR_VARIABLE said)
    if(failed)
      message(FATAL_ERROR "apple_skia_typeinfo.cmake: nmedit failed on ${member}: ${said}")
    endif()
    execute_process(COMMAND nm -m "${work}/${member}" OUTPUT_VARIABLE after)
    foreach(symbol IN LISTS found)
      string(REGEX MATCH "private external ${symbol}\n" still "${after}")
      if(still)
        message(FATAL_ERROR "apple_skia_typeinfo.cmake: ${symbol} is still global in ${member}")
      endif()
    endforeach()
    execute_process(COMMAND ar -r "${archive}" "${member}" WORKING_DIRECTORY "${work}"
                    RESULT_VARIABLE failed)
    if(failed)
      message(FATAL_ERROR "apple_skia_typeinfo.cmake: cannot put ${member} back into ${archive}")
    endif()
    execute_process(COMMAND ranlib "${archive}")
    file(REMOVE_RECURSE "${work}")
    list(LENGTH found count)
    message(STATUS "Made ${count} of the C++ runtime's symbols local to Skia in ${archive}: ${found}")
    set(patched_any TRUE)
  endforeach()
endforeach()
