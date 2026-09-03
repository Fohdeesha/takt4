// alloca.h for MSVC, seen only by KissFFT's own compilation (cmake/deps.cmake).
//
// Built with KISS_FFT_USE_ALLOCA, KissFFT includes <alloca.h> for its stack-allocated
// scratch buffers. MSVC has no such header; its alloca is declared in <malloc.h>.
#pragma once

#include <malloc.h>
