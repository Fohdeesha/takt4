# Run clang's frontend over the sources MSVC compiles, to catch what MSVC does not.
#
# Three of the last four CI failures were the same shape: MSVC accepts something and GCC
# and Clang refuse it, so the build goes green here and red on two of three platforms
# thirty billable minutes later. `htons` as a macro, `recvfrom`'s int-vs-size_t length,
# and a `const T&` bound to `.front()` of a by-value accessor — the last of which is a
# real dangling reference and not a style question.
#
# This machine has no clang *compiler* (Visual Studio ships only clang-format and
# clang-tidy), and the Visual Studio generator writes no compile_commands.json, so the
# flags are reconstructed here rather than read from the build tree. That makes this an
# approximation of the CI compile and not a replacement for it: it uses the MSVC standard
# library and headers, so anything that turns on how libstdc++ or libc++ differ still only
# shows up on CI. What it does catch is the whole -Wdangling / -Wconversion family, which
# is where these have actually landed.
#
#   powershell -ExecutionPolicy Bypass -File tools/lint_clang.ps1
#   powershell -ExecutionPolicy Bypass -File tools/lint_clang.ps1 src/core/control
#
# `src/core`, `src/cli` and `tests` by default — the engine, the console and their tests, which
# CI compiles with -Werror. `src/ui` and `tests/ui` are left out, by name: they need Slint's
# headers and the generated `main_window.h`, which this script does not reconstruct. full.yml
# builds them on every push, with GCC as well since the linux-tsan job.
#
# **A file clang could not parse is not a clean file.** It stops at the first header it cannot
# find and then says nothing — which this script used to count as clean, for four tests/ui
# files every run (the audit's build findings). Such a file is listed as not checked now, and
# the run does not end on "clean".
#
# Exits 1 if clang reports anything, 3 if a file could not be checked, 0 only if every file
# was checked and nothing was found.

param([string[]]$Paths = @('src/core', 'src/cli', 'tests'))
$Exclude = @('src\ui\', 'tests\ui\')

# NOT 'Stop': Windows PowerShell wraps a native command's stderr in ErrorRecords, and
# clang-tidy writes its "N warnings generated" summary there — which under 'Stop' aborts
# the script before it can report anything it found.
$ErrorActionPreference = 'Continue'

$tidy = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang-tidy.exe'
if (-not (Test-Path $tidy)) { Write-Error "clang-tidy not found at $tidy"; exit 2 }

$repo = Split-Path -Parent $PSScriptRoot
$slashed = $repo.Replace('\', '/')
$build = Join-Path $repo 'build\windows-core'
if (-not (Test-Path "$build\generated")) {
    Write-Error "configure the local-core preset first: no $build\generated"
    exit 2
}

# The include roots and defines takt4_tests is compiled with, from src/CMakeLists.txt and
# tests/CMakeLists.txt. Kept by hand; if a target grows a dependency, add it here.
$flags = @(
    '--', '-std=c++20', '-x', 'c++',
    "-I$repo\src", "-I$repo\tests", "-I$build\generated",
    "-isystem$build\_deps\catch2-src\src", "-isystem$build\_deps\catch2-build\generated-includes",
    "-isystem$build\_deps\nlohmann_json-src\include",
    "-isystem$build\_deps\rtneural-src\RTNeural\..",
    "-isystem$build\_deps\rtneural-src\modules\Eigen",
    "-isystem$build\_deps\rtneural-src\RTNeural\..\..\modules\json",
    "-isystem$build\_deps\rtmidi-src",
    "-isystem$build\_deps\r8brain-src",
    "-isystem$build\_deps\kissfft-src",
    # The fixture import's two: one source file each, built as takt4's own targets
    # (cmake/deps.cmake), with the defines those targets give everything that links them.
    "-isystem$build\_deps\pugixml-src\src",
    "-isystem$build\_deps\miniz-src",
    '-DPUGIXML_NO_XPATH', '-DMINIZ_NO_STDIO', '-DMINIZ_NO_TIME', '-DMINIZ_NO_ZLIB_COMPATIBLE_NAMES',
    "-isystem$repo\third_party\portaudio\include",
    "-isystem$repo\third_party\link\include",
    "-isystem$repo\third_party\link\modules\asio-standalone\asio\include",
    # Backslash-escaped quotes, and forward slashes in the value: PowerShell strips bare
    # quotes on the way to a native command, and a Windows path's backslashes would then
    # be escape sequences inside the C string literal these become.
    "-DTAKT4_TEST_DATA_DIR=\`"$slashed/tests/data\`"",
    "-DTAKT4_WEIGHTS_DIR=\`"$slashed/assets/weights\`"",
    "-DTAKT4_STATESPACE_DIR=\`"$slashed/assets/statespace\`"",
    "-DTAKT4_REFERENCES_DIR=\`"$slashed/references\`"",
    '-D_WIN32_WINNT=0x0A00', '-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN',
    '-DLINK_PLATFORM_WINDOWS=1',
    # RTNeural's, copied from CI's own compile line. Without them it picks a different
    # backend and the template instantiation runs clang out of memory rather than
    # compiling — which looks like a tool failure and is actually a missing define.
    '-DEIGEN_STACK_ALLOCATION_LIMIT=0', '-DRTNEURAL_DEFAULT_ALIGNMENT=16',
    '-DRTNEURAL_NAMESPACE=RTNeural', '-DRTNEURAL_USE_EIGEN=1',
    # KissFFT's, also from CI's compile line: without them it builds in float and
    # real_fft.cpp's static_assert that it is double fails, which is the flags, not the code.
    '-Dkiss_fft_scalar=double', '-DKISS_FFT_USE_ALLOCA',
    # The warnings CI turns on that MSVC has no equivalent for, plus the ones it spells
    # differently. -Wdangling-gsl is the one that caught the reference into a temporary.
    '-Wall', '-Wextra', '-Wpedantic', '-Wshadow', '-Wconversion', '-Wsign-conversion',
    '-Wold-style-cast', '-Wdouble-promotion', '-Wdangling-gsl',
    # Not our code's business.
    '-Wno-unknown-warning-option', '-Wno-unused-command-line-argument'
)

$files = @()
foreach ($path in $Paths) {
    $full = Join-Path $repo $path
    if (Test-Path $full -PathType Leaf) { $files += $full; continue }
    $files += Get-ChildItem -Path $full -Recurse -Include *.cpp -File |
        Select-Object -ExpandProperty FullName |
        Where-Object { $relative = $_.Replace("$repo\", ''); -not ($Exclude | Where-Object { $relative.StartsWith($_) }) }
}

Write-Output "clang-tidy over $($files.Count) files"
$found = 0
$skipped = @()
$unchecked = @()
foreach ($file in $files) {
    # The compiler's own diagnostics only. clang-tidy refuses to run with nothing enabled,
    # and its own checks are a different conversation that would drown these in style
    # opinions — `clang-diagnostic-*` is exactly the set CI's -Werror acts on.
    # `clang-diagnostic-*` alone is not enough for clang-tidy to consider anything
    # enabled, so two checks that are on the same subject ride along; neither produces
    # style noise.
    $raw = & $tidy '-quiet' `
        '-checks=-*,clang-diagnostic-*,bugprone-dangling-handle,bugprone-use-after-move' `
        $file @flags 2>&1 | ForEach-Object { "$_" }
    # A header clang cannot find stops the whole file: whatever it would have reported after
    # that point, it never gets to. So the file was not checked, which is said, not hidden.
    $missing = $raw | Where-Object { $_ -match "'(.*)' file not found" } | Select-Object -First 1
    if ($missing) {
        $unchecked += "$($file.Replace("$repo\", ''))  ($($missing -replace '.*?(''[^'']*'' file not found).*', '$1'))"
        continue
    }
    $out = $raw |
        Where-Object { $_ -match 'error:|warning:' } |
        Where-Object { $_ -notmatch 'third_party|_deps|Program Files' }
    # A few translation units run clang out of memory on Windows — the RTNeural model is
    # one, because it instantiates the whole template stack in a single unit. MSVC compiles
    # them and CI compiles them; this tool cannot, and skipping one loudly is worth far
    # more than refusing to check the other forty-four.
    if ($out -match 'LLVM ERROR|out of memory') {
        $skipped += $file.Replace("$repo\", '')
        continue
    }
    if ($out) {
        $found += 1
        Write-Output "--- $($file.Replace("$repo\", ''))"
        $out | ForEach-Object { Write-Output "  $_" }
    }
}

if ($skipped.Count -gt 0) {
    Write-Output "`nclang ran out of memory on $($skipped.Count) file(s); CI still checks them:"
    $skipped | ForEach-Object { Write-Output "  $_" }
}
if ($unchecked.Count -gt 0) {
    Write-Output "`n$($unchecked.Count) file(s) not checked - a header clang could not find, which is this script's include list, not the code:"
    $unchecked | ForEach-Object { Write-Output "  $_" }
}
if ($found -gt 0) { Write-Output "`n$found file(s) with findings"; exit 1 }
if ($unchecked.Count -gt 0) { Write-Output "`nnothing found in the files that were checked, but not every file was"; exit 3 }
Write-Output "clean: $($files.Count - $skipped.Count) file(s) checked"
exit 0
