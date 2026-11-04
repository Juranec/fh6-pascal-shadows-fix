@echo off
rem Run from a Visual Studio x64 developer prompt (needs the Windows SDK and MSVC x64 tools incl. ml64).
rem Do NOT link d3d12.lib: the proxy must not import itself.
if not exist dxcapi.h (
  echo dxcapi.h not found. Copy it from the DXC package's inc folder beside this script.
  exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File gen_exports.ps1
if errorlevel 1 (
  echo Could not generate stubs - see message above.
  exit /b 1
)
ml64 /nologo /c /Cp /Fofwd.obj fwd.asm
if errorlevel 1 exit /b 1
cl /nologo /c /O2 /MT /EHsc /std:c++17 /I. d3d12_proxy.cpp
if errorlevel 1 exit /b 1
link /nologo /DLL /OUT:d3d12.dll d3d12_proxy.obj fwd.obj kernel32.lib user32.lib @exports.rsp
if errorlevel 1 exit /b 1
echo.
echo Built d3d12.dll. Exports:
dumpbin /exports d3d12.dll | findstr /i "D3D12 GetBehavior SetAppCompat"
echo.
echo Copy d3d12.dll, d3d12_proxy.ini, dxcompiler.dll, and matching dxil.dll beside the game exe.
