# Building

Build the proxy on Windows x64 with Visual Studio Build Tools and a Windows 10 or 11 SDK.

## Requirements

- Visual Studio Build Tools with the **Desktop development with C++** workload.
- The MSVC x64 tools, MASM (`ml64.exe`), and `dumpbin.exe`.
- A Windows SDK.
- The DirectX Shader Compiler package. Download a release ZIP from the [DXC releases page](https://github.com/microsoft/DirectXShaderCompiler/releases) and extract it.

## Build the proxy

Before building, copy `inc\dxcapi.h` from the extracted DXC package into this project folder, beside `build.bat`. Open an **x64 Native Tools Command Prompt for VS**, change to this folder, and run:

```bat
build.bat
```

The script reads the Windows D3D12 export table, generates forwarding stubs, then builds `d3d12.dll`. These exports come from the Windows version used to build it, so build on the Windows version you intend to target. The script must not link `d3d12.lib`, because that would make the proxy import itself.
