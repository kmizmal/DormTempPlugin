@echo off
rem Build DormTemp.dll (single-DLL Rainmeter plugin) with zig c++ - no MSVC / no Windows SDK needed.
rem winrt_gen\ holds the C++/WinRT projection headers generated from Windows\System32\WinMetadata.
setlocal
set "HERE=%~dp0"
set "ZIG=%HERE%..\..\tools\zig\zig.exe"
if not exist "%ZIG%" (
  echo [error] zig not found: %ZIG%
  exit /b 1
)
"%ZIG%" c++ -target x86_64-windows-gnu -std=c++20 -fms-extensions -O2 -shared ^
  -I "%HERE%winrt_gen" ^
  -o "%HERE%DormTemp.dll" ^
  "%HERE%DormTempMain.cpp" "%HERE%store.cpp" "%HERE%RoShim.cpp" ^
  -lole32 -loleaut32
if errorlevel 1 ( echo [error] build failed & exit /b 1 )
echo built: %HERE%DormTemp.dll
