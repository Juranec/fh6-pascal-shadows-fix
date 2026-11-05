# Forza Horizon 6 shadows fix for Pascal GPUs

A small Windows x64 `d3d12.dll` proxy that fixes a missing-shadows issue seen on NVIDIA Pascal cards, including the GeForce GTX 10 series.

Some game shaders use Variable Rate Shading, which Pascal GPUs do not support. The proxy changes the shading-rate input in matching shaders to the standard 1×1 value. It patches each matching shader the first time the game uses it and saves copies for later launches.

This is a community workaround, not official support. It only targets shaders that use this input, and may not fix every graphics issue.

![Comparison screenshot](screenshot.png)

## Install

1. Download the Windows x64 release.
2. Open the folder containing the game's executable. Back up any existing `d3d12.dll` or `d3d12_proxy.ini` there.
3. Copy `d3d12.dll` and `d3d12_proxy.ini` into that folder. From the [DirectX Shader Compiler releases page](https://github.com/microsoft/DirectXShaderCompiler/releases), download a release ZIP, open it, and copy `dxcompiler.dll` and `dxil.dll` from `bin/x64` into the game folder. Always use both DLLs from the same package.
4. Start the game. Matching shaders are patched as the game uses them; later launches use the saved copies.

The proxy writes a log and a loaded marker beside itself. It stores patched shaders in `d3d12_proxy_replace`. To uninstall, remove the files you copied and restore your backups.

## Settings

The included `d3d12_proxy.ini` is ready to use. Most players do not need to change it. To collect shader files for troubleshooting, change `Dump=0` to `Dump=1`; dumps are written to `d3d12_proxy_dump` beside the proxy.

## If it does not work

- No `d3d12_proxy_loaded.txt`: check that the DLLs are beside the game executable.
- A DXC error in `d3d12_proxy.log`: check that `dxcompiler.dll` and `dxil.dll` are both 64-bit and from the same package.
- `AutoPatch skipped` in the log: the proxy could not patch that shader. It will keep using the original shader.
- The game still has no shadows: keep `d3d12_proxy.log` and include it when reporting the problem.

## Build

See [BUILD.md](BUILD.md) for build instructions.
