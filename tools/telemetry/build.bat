@echo off
rem Builds sampler.exe, threadtop.exe and burner.exe with the portable LLVM (docs/BUILDS.md: $WORK/toolchain/llvm-20.1.8).
rem   build.bat [ADLX_DIR [OUT_DIR]]
rem     ADLX_DIR  a checkout of the ADLX SDK (default %WORK%\toolchain\ADLX; see README.md for the fetch command)
rem     OUT_DIR   where the exes go (default %WORK%\toolchain\telemetry, where tools\bench.py looks for sampler.exe)
rem   %WORK% is %MCLA_WORK%, else C:\Users\Ali\Development\mcla-x86-work (the same default as tools\run_game.py).
rem No Visual Studio developer prompt is needed: clang finds the MSVC and Windows SDK libraries by itself.
setlocal
if "%MCLA_WORK%"=="" (set "WORK=C:\Users\Ali\Development\mcla-x86-work") else (set "WORK=%MCLA_WORK%")
set "CLANG=%WORK%\toolchain\llvm-20.1.8\bin\clang++.exe"
set "ADLX=%~1"
if "%ADLX%"=="" set "ADLX=%WORK%\toolchain\ADLX"
set "OUT=%~2"
if "%OUT%"=="" set "OUT=%WORK%\toolchain\telemetry"
set "HERE=%~dp0"

if not exist "%CLANG%" (echo build.bat: no clang at %CLANG% & exit /b 2)
if not exist "%ADLX%\SDK\ADLXHelper\Windows\Cpp\ADLXHelper.cpp" (echo build.bat: no ADLX SDK at %ADLX% - see tools\telemetry\README.md & exit /b 2)
if not exist "%OUT%" mkdir "%OUT%"

set "FLAGS=--target=x86_64-pc-windows-msvc -std=c++17 -O2 -fuse-ld=lld"

rem sampler: ADLX (headers, ADLXHelper.cpp and WinAPIs.cpp are compiled in); vendor code, so its warnings are off.
"%CLANG%" %FLAGS% -Wno-everything -I"%ADLX%" "%HERE%sampler.cpp" "%ADLX%\SDK\ADLXHelper\Windows\Cpp\ADLXHelper.cpp" "%ADLX%\SDK\Platform\Windows\WinAPIs.cpp" -lwinmm -lpowrprof -lpdh -o "%OUT%\sampler.exe" || exit /b 1
"%CLANG%" %FLAGS% "%HERE%threadtop.cpp" -o "%OUT%\threadtop.exe" || exit /b 1
"%CLANG%" %FLAGS% "%HERE%burner.cpp" -ld3d11 -ld3dcompiler -o "%OUT%\burner.exe" || exit /b 1
echo built sampler.exe threadtop.exe burner.exe in %OUT%
