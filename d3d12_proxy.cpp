// d3d12_proxy.cpp
//
// D3D12 proxy and shader patching hooks. See README.md and BUILD.md.
//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <dxcapi.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstring>

// ---------------------------------------------------------------------------------------------
// Export forwarding via jump stubs.
// gen_exports.ps1 reads the real System32\d3d12.dll's export table and generates:
//   fwd.asm        one "jmp [g_fwd + 8*i]" stub per export, plus the g_fwd pointer table
//   fwd_names.inc  the export names, in the same order
//   exports.rsp    /EXPORT: options for the linker
// At load we fill g_fwd with GetProcAddress() on the real DLL. D3D12CreateDevice is ours.
// ---------------------------------------------------------------------------------------------
extern "C" void* g_fwd[];
#include "fwd_names.inc"   // kFwdNames[] (nullptr = nameless export) and kFwdOrds[]

// ---------------------------------------------------------------------------------------------
// Config / logging
// ---------------------------------------------------------------------------------------------
static HMODULE g_self;
static wchar_t g_iniPath[MAX_PATH];
static wchar_t g_logPath[MAX_PATH];
static SRWLOCK g_logLock = SRWLOCK_INIT;

static struct Config {
    int installHooks = 1;      // 0 = no hooking at all: proxy only forwards D3D12CreateDevice (diagnostic)
    int dump = 0;              // save matching pixel shaders for troubleshooting
    int replace = 1;           // substitute d3d12_proxy_replace\<hash>.dxil where such a file exists
    int autoPatch = 1;         // if no replacement exists, patch SV_ShadingRate to 1x1 on first use
    int cachePatches = 1;      // persist auto-patched shaders under their original hash for later launches
    int hookLibraries = 1;     // also intercept ID3D12PipelineLibrary loads
    int hookStreams = 1;       // also intercept ID3D12Device2::CreatePipelineState (stream form)
    int allowUnsigned = 0;     // enable D3D12 experimental shader models (accepts unsigned DXIL; needs Developer Mode)
} g_cfg;

static void Log(const char* fmt, ...) {
    char buf[700];
    va_list a;
    va_start(a, fmt);
    vsnprintf(buf, sizeof buf, fmt, a);
    va_end(a);
    OutputDebugStringA("[d3d12_proxy] ");
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
    AcquireSRWLockExclusive(&g_logLock);
    FILE* f = _wfopen(g_logPath, L"a");
    if (f) {
        fprintf(f, "%s\n", buf);
        fclose(f);
    }
    ReleaseSRWLockExclusive(&g_logLock);
}

static void ModuleOf(const void* addr, wchar_t* out, size_t n) {
    HMODULE mod = nullptr;
    out[0] = 0;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)addr, &mod) && mod) {
        wchar_t full[MAX_PATH];
        GetModuleFileNameW(mod, full, MAX_PATH);
        const wchar_t* base = wcsrchr(full, L'\\');
        swprintf(out, n, L"%ls+0x%llx", base ? base + 1 : full, (unsigned long long)((const char*)addr - (const char*)mod));
    } else {
        wcscpy_s(out, n, L"(no module)");
    }
}

// Breadcrumb: which of our hooks the current thread is inside (shown in crash log).
static thread_local const char* t_where = "";
struct Crumb {
    const char* prev;
    explicit Crumb(const char* w) : prev(t_where) { t_where = w; }
    ~Crumb() { t_where = prev; }
};
#define FIRST(tag) do { static std::atomic<bool> f_{false}; if (!f_.exchange(true)) Log("first call: %s", tag); } while (0)

static LONG CALLBACK CrashLogger(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != 0xC0000409 && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_PRIV_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    static std::atomic<int> n{0};
    if (n.fetch_add(1) >= 6) return EXCEPTION_CONTINUE_SEARCH;
    const CONTEXT* c = ep->ContextRecord;
    wchar_t m1[MAX_PATH + 40], m2[MAX_PATH + 40];
    ModuleOf((void*)c->Rip, m1, MAX_PATH + 40);
    void* ret = nullptr;
    SIZE_T got = 0;
    ReadProcessMemory(GetCurrentProcess(), (void*)c->Rsp, &ret, sizeof ret, &got);
    ModuleOf(ret, m2, MAX_PATH + 40);
    Log("EXCEPTION 0x%08lx rip=%p [%ls] rsp=%p [rsp]=%p [%ls] info0=%llu info1=%p inside_hook='%s'", (unsigned long)code,
        (void*)c->Rip, m1, (void*)c->Rsp, ret, m2, (unsigned long long)ep->ExceptionRecord->ExceptionInformation[0],
        (void*)ep->ExceptionRecord->ExceptionInformation[1], t_where);
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------------------------------------
// Vtable patching with per-vtable originals
// ---------------------------------------------------------------------------------------------
struct OrigMap {
    struct E { void** vt; void* fn; } e[32];
    std::atomic<int> n{0};
    void* Find(const void* obj) const {
        void** vt = *(void***)obj;
        int c = n.load(std::memory_order_acquire);
        for (int i = 0; i < c; i++)
            if (e[i].vt == vt) return e[i].fn;
        return nullptr;
    }
};

static SRWLOCK g_patchLock = SRWLOCK_INIT;

static bool Patch(void** vt, int idx, void* hook, OrigMap& m) {
    for (int i = 0, c = m.n.load(std::memory_order_acquire); i < c; i++)
        if (m.e[i].vt == vt) return true;          // fast path: already patched
    bool ok = true;
    AcquireSRWLockExclusive(&g_patchLock);
    int c = m.n.load();
    bool have = false;
    for (int i = 0; i < c; i++)
        if (m.e[i].vt == vt) have = true;
    if (!have) {
        if (c >= 32 || vt[idx] == hook) {
            ok = false;
        } else {
            m.e[c].vt = vt;
            m.e[c].fn = vt[idx];
            m.n.store(c + 1, std::memory_order_release);   // publish original before patching
            DWORD old;
            VirtualProtect(&vt[idx], sizeof(void*), PAGE_READWRITE, &old);
            vt[idx] = hook;
            VirtualProtect(&vt[idx], sizeof(void*), old, &old);
            wchar_t mod[MAX_PATH + 40];
            ModuleOf(m.e[c].fn, mod, MAX_PATH + 40);
            Log("patched vtable %p slot %d, original %p [%ls]", (void*)vt, idx, m.e[c].fn, mod);
        }
    }
    ReleaseSRWLockExclusive(&g_patchLock);
    return ok;
}

// vtable slots (from d3d12.h interface order)
static const int kDev_CreateGraphicsPipelineState = 10;   // ID3D12Device
static const int kDev_CreatePipelineLibrary = 44;         // ID3D12Device1 and newer
static const int kDev_CreatePipelineState = 47;           // ID3D12Device2 and newer
static const int kLib_LoadGraphicsPipeline = 9;           // ID3D12PipelineLibrary
static const int kLib_LoadPipeline = 13;                  // ID3D12PipelineLibrary1

static OrigMap g_mCreateGPS, g_mCreateLib, g_mCreatePS, g_mLoadGPS, g_mLoadPipeline;

// IIDs of ID3D12Device1..10 (unknown/unsupported ones just fail QueryInterface).
static const GUID kDeviceIIDs[] = {
{0x77acce80, 0x638e, 0x4e65, {0x88, 0x95, 0xc1, 0xf2, 0x33, 0x86, 0x86, 0x3e}},
    {0x30baa41e, 0xb15b, 0x475c, {0xa0, 0xbb, 0x1a, 0xf5, 0xc5, 0xb6, 0x43, 0x28}},
    {0x81dadc15, 0x2bad, 0x4392, {0x93, 0xc5, 0x10, 0x13, 0x45, 0xc4, 0xaa, 0x98}},
    {0xe865df17, 0xa9ee, 0x46f9, {0xa4, 0x63, 0x30, 0x98, 0x31, 0x5a, 0xa2, 0xe5}},
    {0x8b4f173b, 0x2fea, 0x4b80, {0x8f, 0x58, 0x43, 0x07, 0x19, 0x1a, 0xb9, 0x5d}},
    {0xc70b221b, 0x40e4, 0x4a17, {0x89, 0xaf, 0x02, 0x5a, 0x00, 0x27, 0xa2, 0xb4}},
    {0x5c014b53, 0x68a1, 0x4b9b, {0x8b, 0xd1, 0xdd, 0x60, 0x46, 0xb9, 0x35, 0x8b}},
    {0x9218e6bb, 0xf944, 0x4f7e, {0xa7, 0x5c, 0xb4, 0xba, 0x60, 0xf6, 0xd4, 0xd2}},
    {0x4c80e962, 0xf032, 0x4f60, {0xbc, 0x9e, 0xeb, 0xc2, 0xcf, 0xa1, 0xd8, 0x3c}},
    {0x517f8718, 0xaa66, 0x49f9, {0xb0, 0x2b, 0xa7, 0xab, 0x89, 0xc0, 0x60, 0x12}}
};
static const GUID kIID_PipelineLibrary = {0xc64226a8, 0x9201, 0x46af, {0xb4, 0xcc, 0x53, 0xfb, 0x9f, 0xf7, 0x41, 0x4f}};   // ID3D12PipelineLibrary
static const GUID kIID_PipelineLibrary1 = {0x80eabf42, 0x2568, 0x4e5e, {0xbd, 0x82, 0xc3, 0x7f, 0x86, 0x96, 0x1d, 0xc3}};
static const GUID kExperimentalShaderModels = {0x76f5573e, 0xf13a, 0x40f5, {0xb2, 0x97, 0x81, 0xce, 0x9e, 0x18, 0x93, 0x3f}};

// ---------------------------------------------------------------------------------------------
// Shader registry: identify shaders that read SV_ShadingRate, dump them, find replacements
// ---------------------------------------------------------------------------------------------
struct ShaderInfo {
    bool usesRate = false;
    std::vector<uint8_t> repl;   // empty = no replacement
};
static SRWLOCK g_shLock = SRWLOCK_INIT;
static SRWLOCK g_shaderBuildLock = SRWLOCK_INIT;
static SRWLOCK g_dxcInitLock = SRWLOCK_INIT;
static SRWLOCK g_dxcCallLock = SRWLOCK_INIT;
static std::unordered_map<std::string, ShaderInfo> g_shaders;   // key: 32 hex chars of the container hash
static wchar_t g_dumpDir[MAX_PATH], g_replDir[MAX_PATH];
static std::atomic<long> g_nReplaced{0};

using Microsoft::WRL::ComPtr;
static HMODULE g_dxcModule = nullptr;
static DxcCreateInstanceProc g_dxcCreate = nullptr;
static IDxcUtils* g_dxcUtils = nullptr;
static IDxcCompiler* g_dxcCompiler = nullptr;
static IDxcAssembler* g_dxcAssembler = nullptr;
static IDxcValidator* g_dxcValidator = nullptr;
static bool g_dxcInitAttempted = false;
static bool g_dxcReady = false;

// Load DXC from beside d3d12.dll. The proxy is often loaded by the game before the
// current directory is stable, so don't rely on a relative LoadLibrary search.
static bool EnsureDxc() {
    AcquireSRWLockExclusive(&g_dxcInitLock);
    if (g_dxcInitAttempted) {
        bool ready = g_dxcReady;
        ReleaseSRWLockExclusive(&g_dxcInitLock);
        return ready;
    }
    g_dxcInitAttempted = true;

    wchar_t dir[MAX_PATH];
    GetModuleFileNameW(g_self, dir, MAX_PATH);
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (slash) slash[1] = 0;
    wchar_t dxcPath[MAX_PATH];
    swprintf(dxcPath, MAX_PATH, L"%lsdxcompiler.dll", dir);
    g_dxcModule = LoadLibraryExW(dxcPath, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!g_dxcModule) {
        Log("AutoPatch: couldn't load %ls (error %lu)", dxcPath, GetLastError());
        ReleaseSRWLockExclusive(&g_dxcInitLock);
        return false;
    }
    g_dxcCreate = (DxcCreateInstanceProc)GetProcAddress(g_dxcModule, "DxcCreateInstance");
    HRESULT hr = g_dxcCreate ? g_dxcCreate(CLSID_DxcUtils, IID_PPV_ARGS(&g_dxcUtils)) : E_FAIL;
    if (SUCCEEDED(hr)) hr = g_dxcCreate(CLSID_DxcCompiler, IID_PPV_ARGS(&g_dxcCompiler));
    if (SUCCEEDED(hr)) hr = g_dxcCreate(CLSID_DxcAssembler, IID_PPV_ARGS(&g_dxcAssembler));
    if (SUCCEEDED(hr)) hr = g_dxcCreate(CLSID_DxcValidator, IID_PPV_ARGS(&g_dxcValidator));
    if (FAILED(hr)) {
        Log("AutoPatch: DXC initialization failed (0x%08lx); ensure dxcompiler.dll and dxil.dll are beside d3d12.dll",
            (unsigned long)hr);
        ReleaseSRWLockExclusive(&g_dxcInitLock);
        return false;
    }
    g_dxcReady = true;
    Log("AutoPatch: DXC initialized from %ls", dxcPath);
    ReleaseSRWLockExclusive(&g_dxcInitLock);
    return true;
}

static void PrintDxcErrors(IDxcOperationResult* r, const char* what) {
    ComPtr<IDxcBlobEncoding> err;
    ComPtr<IDxcBlobUtf8> err8;
    if (r && SUCCEEDED(r->GetErrorBuffer(&err)) && err && err->GetBufferSize()) {
        Log("AutoPatch %s: %.*s", what, (int)err->GetBufferSize(), (const char*)err->GetBufferPointer());
    }
}

// Rewrite every load of the SV_ShadingRate signature input to the defined 1x1
// value (0), then assemble and validate/sign the DXIL container in memory.
static bool PatchShadingRateInput(const uint8_t* bytes, size_t size, std::vector<uint8_t>& patched,
                                  std::string& why) {
    if (!EnsureDxc()) { why = "DXC unavailable"; return false; }
    if (!bytes || size < 36 || size > (64u << 20) || size > UINT32_MAX || memcmp(bytes, "DXBC", 4) != 0) {
        why = "invalid DXIL container";
        return false;
    }

    AcquireSRWLockExclusive(&g_dxcCallLock);

    ComPtr<IDxcBlobEncoding> input, disassembly;
    HRESULT hr = g_dxcUtils->CreateBlob(bytes, (UINT32)size, DXC_CP_ACP, &input);
    if (SUCCEEDED(hr)) hr = g_dxcCompiler->Disassemble(input.Get(), &disassembly);
    if (FAILED(hr) || !disassembly) {
        ReleaseSRWLockExclusive(&g_dxcCallLock);
        why = "DXIL disassembly failed";
        return false;
    }

    std::string text((const char*)disassembly->GetBufferPointer(), disassembly->GetBufferSize());
    while (!text.empty() && text.back() == '\0') text.pop_back();
    std::smatch signature;
    static const std::regex signaturePattern(R"(!\{i32 (\d+), !"SV_ShadingRate")");
    if (!std::regex_search(text, signature, signaturePattern)) {
        ReleaseSRWLockExclusive(&g_dxcCallLock);
        why = "SV_ShadingRate signature not found";
        return false;
    }

    const std::string id = signature[1].str();
    const std::regex loadPattern("^(\\s*)(%[A-Za-z0-9_.]+) = call i32 @dx\\.op\\.loadInput\\.i32\\(i32 4, i32 " + id +
        ", i32 \\d+, i8 \\d+, i32 [^)]*\\).*$");
    std::istringstream inputLines(text);
    std::vector<std::string> lines;
    std::string line;
    int replaced = 0;
    bool otherI32Loads = false;
    while (std::getline(inputLines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::smatch match;
        if (std::regex_match(line, match, loadPattern)) {
            lines.push_back(match[1].str() + match[2].str() + " = add i32 0, 0");
            ++replaced;
        } else {
            if (line.find("call i32 @dx.op.loadInput.i32(") != std::string::npos) otherI32Loads = true;
            lines.push_back(line);
        }
    }
    if (!replaced) {
        ReleaseSRWLockExclusive(&g_dxcCallLock);
        why = "no SV_ShadingRate loads found";
        return false;
    }

    std::ostringstream patchedText;
    for (const std::string& l : lines) {
        if (!otherI32Loads && l.rfind("declare i32 @dx.op.loadInput.i32(", 0) == 0) continue;
        patchedText << l << "\n";
    }
    const std::string assembledText = patchedText.str();
    ComPtr<IDxcBlobEncoding> textBlob;
    ComPtr<IDxcOperationResult> asmResult;
    hr = g_dxcUtils->CreateBlob(assembledText.data(), (UINT32)assembledText.size(), DXC_CP_UTF8, &textBlob);
    if (SUCCEEDED(hr)) hr = g_dxcAssembler->AssembleToContainer(textBlob.Get(), &asmResult);
    HRESULT status = E_FAIL;
    if (SUCCEEDED(hr) && asmResult) asmResult->GetStatus(&status);
    if (FAILED(hr) || FAILED(status) || !asmResult) {
        PrintDxcErrors(asmResult.Get(), "assembly failed");
        ReleaseSRWLockExclusive(&g_dxcCallLock);
        why = "DXIL assembly failed";
        return false;
    }

    ComPtr<IDxcBlob> container;
    hr = asmResult->GetResult(&container);
    if (FAILED(hr) || !container || !container->GetBufferSize()) {
        ReleaseSRWLockExclusive(&g_dxcCallLock);
        why = "DXC returned no assembled container";
        return false;
    }

    ComPtr<IDxcOperationResult> validation;
    hr = g_dxcValidator->Validate(container.Get(), DxcValidatorFlags_InPlaceEdit, &validation);
    status = E_FAIL;
    if (SUCCEEDED(hr) && validation) validation->GetStatus(&status);
    if (FAILED(hr) || FAILED(status) || !validation) {
        PrintDxcErrors(validation.Get(), "validation failed");
        ReleaseSRWLockExclusive(&g_dxcCallLock);
        why = "DXIL validation/signing failed";
        return false;
    }
    ComPtr<IDxcBlob> signedContainer;
    hr = validation->GetResult(&signedContainer);
    if (FAILED(hr) || !signedContainer || !signedContainer->GetBufferSize()) {
        ReleaseSRWLockExclusive(&g_dxcCallLock);
        why = "DXC returned no signed container";
        return false;
    }
    const uint8_t* first = (const uint8_t*)signedContainer->GetBufferPointer();
    if (signedContainer->GetBufferSize() < 36 || memcmp(first, "DXBC", 4) != 0) {
        ReleaseSRWLockExclusive(&g_dxcCallLock);
        why = "DXC produced an invalid container";
        return false;
    }
    patched.assign(first, first + signedContainer->GetBufferSize());
    ReleaseSRWLockExclusive(&g_dxcCallLock);
    Log("AutoPatch: replaced %d SV_ShadingRate load(s) with 1x1", replaced);
    return true;
}

static bool IsReadable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    size_t avail = (const char*)mbi.BaseAddress + mbi.RegionSize - (const char*)p;
    return avail >= n;
}

static bool Contains(const uint8_t* h, size_t n, const char* pat, size_t m) {
    if (n < m) return false;
    const uint8_t* end = h + (n - m + 1);
    while (h < end) {
        h = (const uint8_t*)memchr(h, (unsigned char)pat[0], (size_t)(end - h));
        if (!h) return false;
        if (!memcmp(h, pat, m)) return true;
        h++;
    }
    return false;
}

static void WriteNewFile(const wchar_t* path, const void* data, size_t n) {
    HANDLE f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;     // already exists (or cannot write): leave it alone
    DWORD wr = 0;
    WriteFile(f, data, (DWORD)n, &wr, nullptr);
    CloseHandle(f);
}

static bool ReadWholeFile(const wchar_t* path, std::vector<uint8_t>& out) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    bool ok = false;
    if (GetFileSizeEx(f, &sz) && sz.QuadPart > 0 && sz.QuadPart < (64 << 20)) {
        out.resize((size_t)sz.QuadPart);
        DWORD rd = 0;
        ok = ReadFile(f, out.data(), (DWORD)out.size(), &rd, nullptr) && rd == out.size();
        if (!ok) out.clear();
    }
    CloseHandle(f);
    return ok;
}

// Returns the replacement bytes for this pixel shader, or nullptr to leave it alone.
static const std::vector<uint8_t>* LookupReplacement(const D3D12_SHADER_BYTECODE& ps) {
    if (!g_cfg.dump && !g_cfg.replace && !g_cfg.autoPatch) return nullptr;
    if (!ps.pShaderBytecode || ps.BytecodeLength < 36) return nullptr;
    const uint8_t* b = (const uint8_t*)ps.pShaderBytecode;
    if (!IsReadable(b, 36) || memcmp(b, "DXBC", 4) != 0) return nullptr;
    char hex[33];
    for (int i = 0; i < 16; i++) snprintf(hex + 2 * i, 3, "%02x", b[4 + i]);
    std::string key(hex, 32);

    AcquireSRWLockShared(&g_shLock);
    auto it = g_shaders.find(key);
    if (it != g_shaders.end()) {
        const std::vector<uint8_t>* r = it->second.repl.empty() ? nullptr : &it->second.repl;
        ReleaseSRWLockShared(&g_shLock);
        return r;
    }
    ReleaseSRWLockShared(&g_shLock);

    // Serialize cache misses so the same shader isn't dumped/patched multiple
    // times if pipeline creation happens concurrently on several game threads.
    AcquireSRWLockExclusive(&g_shaderBuildLock);
    AcquireSRWLockShared(&g_shLock);
    it = g_shaders.find(key);
    if (it != g_shaders.end()) {
        const std::vector<uint8_t>* r = it->second.repl.empty() ? nullptr : &it->second.repl;
        ReleaseSRWLockShared(&g_shLock);
        ReleaseSRWLockExclusive(&g_shaderBuildLock);
        return r;
    }
    ReleaseSRWLockShared(&g_shLock);

    // First time we see this shader: analyse it.
    ShaderInfo info;
    info.usesRate = Contains(b, ps.BytecodeLength, "SV_ShadingRate", 14);
    if (info.usesRate) {
        wchar_t wname[40];
        swprintf(wname, 40, L"%hs.dxil", hex);
        wchar_t path[MAX_PATH];
        if (g_cfg.dump) {
            swprintf(path, MAX_PATH, L"%ls%ls", g_dumpDir, wname);
            WriteNewFile(path, b, ps.BytecodeLength);
        }
        if (g_cfg.replace || (g_cfg.autoPatch && g_cfg.cachePatches)) {
            swprintf(path, MAX_PATH, L"%ls%ls", g_replDir, wname);
            ReadWholeFile(path, info.repl);
        }
        if (info.repl.empty() && g_cfg.autoPatch) {
            std::string why;
            if (PatchShadingRateInput(b, ps.BytecodeLength, info.repl, why)) {
                if (g_cfg.cachePatches) {
                    swprintf(path, MAX_PATH, L"%ls%ls", g_replDir, wname);
                    WriteNewFile(path, info.repl.data(), info.repl.size());
                }
                Log("pixel shader %s auto-patched and cached=%d", hex, g_cfg.cachePatches);
            } else {
                Log("pixel shader %s AutoPatch skipped: %s", hex, why.c_str());
            }
        }
        Log("pixel shader %s (%zu bytes) reads SV_ShadingRate; dumped=%d, replacement %s", hex,
            (size_t)ps.BytecodeLength, g_cfg.dump, info.repl.empty() ? "not available" : "FOUND");
    }
    AcquireSRWLockExclusive(&g_shLock);
    auto ins = g_shaders.emplace(key, std::move(info));
    const std::vector<uint8_t>* r = ins.first->second.repl.empty() ? nullptr : &ins.first->second.repl;
    ReleaseSRWLockExclusive(&g_shLock);
    ReleaseSRWLockExclusive(&g_shaderBuildLock);
    return r;
}

static void NoteReplaced(const char* where, const std::vector<uint8_t>& r) {
    long c = g_nReplaced.fetch_add(1) + 1;
    if (c <= 5 || c == 10 || c == 100 || c == 1000)
        Log("replaced pixel shader via %s (count=%ld, new size %zu)", where, c, r.size());
}

// ---------------------------------------------------------------------------------------------
// Pipeline state stream (CreatePipelineState / LoadPipeline) handling
// ---------------------------------------------------------------------------------------------
struct RtFormatArray { DXGI_FORMAT f[8]; UINT n; };
// Local copy of D3D12_PIPELINE_STATE_STREAM_DESC (same layout) so we don't depend on the SDK version.
struct PsoStreamDesc { SIZE_T SizeInBytes; void* pPipelineStateSubobjectStream; };
static inline size_t AlignUp(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

// Walks the subobject stream using the known layout of each subobject type. Returns false if it meets a type
// it does not know. Output: offsets of the PS payload and of the CACHED_PSO payload (-1 if absent).
static bool ParseStream(const uint8_t* s, size_t size, size_t* psInner, size_t* cachedInner) {
    *psInner = *cachedInner = (size_t)-1;
    size_t off = 0;
    while (off + 8 <= size) {
        uint32_t type = *(const uint32_t*)(s + off);
        size_t isz = 0, ial = 0;
        switch (type) {
            case 0:  isz = sizeof(void*); ial = alignof(void*); break;                                   // ROOT_SIGNATURE
            case 1: case 2: case 3: case 4: case 5: case 6: case 24: case 25:                           // VS PS DS HS GS CS AS MS
                isz = sizeof(D3D12_SHADER_BYTECODE); ial = alignof(D3D12_SHADER_BYTECODE); break;
            case 7:  isz = sizeof(D3D12_STREAM_OUTPUT_DESC); ial = alignof(D3D12_STREAM_OUTPUT_DESC); break;
            case 8:  isz = sizeof(D3D12_BLEND_DESC); ial = alignof(D3D12_BLEND_DESC); break;
            case 9:  isz = 4; ial = 4; break;                                                            // SAMPLE_MASK
            case 10: isz = sizeof(D3D12_RASTERIZER_DESC); ial = alignof(D3D12_RASTERIZER_DESC); break;
            case 11: isz = sizeof(D3D12_DEPTH_STENCIL_DESC); ial = alignof(D3D12_DEPTH_STENCIL_DESC); break;
            case 12: isz = sizeof(D3D12_INPUT_LAYOUT_DESC); ial = alignof(D3D12_INPUT_LAYOUT_DESC); break;
            case 13: case 14: isz = 4; ial = 4; break;                                                   // IB_STRIP_CUT, PRIM_TOPOLOGY
            case 15: isz = sizeof(RtFormatArray); ial = alignof(RtFormatArray); break;                   // RENDER_TARGET_FORMATS
            case 16: isz = 4; ial = 4; break;                                                            // DEPTH_STENCIL_FORMAT
            case 17: isz = sizeof(DXGI_SAMPLE_DESC); ial = alignof(DXGI_SAMPLE_DESC); break;
            case 18: isz = 4; ial = 4; break;                                                            // NODE_MASK
            case 19: isz = sizeof(D3D12_CACHED_PIPELINE_STATE); ial = alignof(D3D12_CACHED_PIPELINE_STATE); break;
            case 20: isz = 4; ial = 4; break;                                                            // FLAGS
            case 21: isz = 56; ial = 4; break;                                                           // DEPTH_STENCIL1 (sizeof D3D12_DEPTH_STENCIL_DESC1)
            case 22: isz = 24; ial = 8; break;                                                           // VIEW_INSTANCING (sizeof D3D12_VIEW_INSTANCING_DESC)
            default: return false;   // newer subobject types: not handled
        }
        size_t inner = AlignUp(off + 4, ial);
        if (inner + isz > size) return false;
        if (type == 2) *psInner = inner;
        if (type == 19) *cachedInner = inner;
        off = AlignUp(inner + isz, 8);
    }
    return true;
}

// Fallback when ParseStream fails: look for an 8-byte aligned {type==PS, ptr->'DXBC', size} entry.
static size_t FindPsHeuristic(const uint8_t* s, size_t size) {
    for (size_t off = 0; off + 24 <= size; off += 8) {
        if (*(const uint32_t*)(s + off) != 2) continue;
        const uint8_t* p = *(const uint8_t* const*)(s + off + 8);
        size_t len = *(const size_t*)(s + off + 16);
        if (len >= 36 && len < (64u << 20) && IsReadable(p, 4) && !memcmp(p, "DXBC", 4)) return off + 8;
    }
    return (size_t)-1;
}

// Builds a patched copy of the stream. Returns false if this PSO needs no change.
static bool MakePatchedStream(const PsoStreamDesc* d, std::vector<uint8_t>& buf,
                              PsoStreamDesc& nd, const std::vector<uint8_t>** replOut) {
    if (!d || !d->pPipelineStateSubobjectStream || d->SizeInBytes < 24) return false;
    const uint8_t* s = (const uint8_t*)d->pPipelineStateSubobjectStream;
    size_t n = d->SizeInBytes, psOff = (size_t)-1, cOff = (size_t)-1;
    if (!ParseStream(s, n, &psOff, &cOff)) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) Log("pipeline stream contains a subobject type I don't know; using heuristic PS search");
        psOff = FindPsHeuristic(s, n);
        cOff = (size_t)-1;
    }
    if (psOff == (size_t)-1) return false;
    D3D12_SHADER_BYTECODE ps;
    memcpy(&ps, s + psOff, sizeof ps);
    const std::vector<uint8_t>* r = LookupReplacement(ps);
    if (!r) return false;
    buf.assign(s, s + n);
    D3D12_SHADER_BYTECODE nps = {r->data(), r->size()};
    memcpy(buf.data() + psOff, &nps, sizeof nps);
    if (cOff != (size_t)-1) memset(buf.data() + cOff, 0, sizeof(D3D12_CACHED_PIPELINE_STATE));
    nd.SizeInBytes = n;
    nd.pPipelineStateSubobjectStream = buf.data();
    *replOut = r;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Pipeline-creation hooks
// ---------------------------------------------------------------------------------------------
using PFN_CreateGPS = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
static HRESULT STDMETHODCALLTYPE Hook_CreateGPS(ID3D12Device* self, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
                                                REFIID riid, void** pp) {
    Crumb crumb("CreateGraphicsPipelineState");
    FIRST("CreateGraphicsPipelineState");
    PFN_CreateGPS o = (PFN_CreateGPS)g_mCreateGPS.Find(self);
    if (!o) { Log("CreateGraphicsPipelineState: no original for vtable %p", (void*)*(void***)self); return E_FAIL; }
    if (desc && desc->PS.pShaderBytecode) {
        const std::vector<uint8_t>* r = LookupReplacement(desc->PS);
        if (r) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC d = *desc;
            d.PS.pShaderBytecode = r->data();
            d.PS.BytecodeLength = r->size();
            d.CachedPSO = D3D12_CACHED_PIPELINE_STATE{};
            HRESULT hr = o(self, &d, riid, pp);
            if (SUCCEEDED(hr)) { NoteReplaced("CreateGraphicsPipelineState", *r); return hr; }
            Log("patched CreateGraphicsPipelineState failed (0x%08lx); using the original shader", (unsigned long)hr);
        }
    }
    return o(self, desc, riid, pp);
}

using PFN_CreatePS = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const PsoStreamDesc*, REFIID, void**);
static HRESULT STDMETHODCALLTYPE Hook_CreatePS(ID3D12Device* self, const PsoStreamDesc* desc,
                                               REFIID riid, void** pp) {
    Crumb crumb("CreatePipelineState");
    FIRST("CreatePipelineState");
    PFN_CreatePS o = (PFN_CreatePS)g_mCreatePS.Find(self);
    if (!o) { Log("CreatePipelineState: no original for vtable %p", (void*)*(void***)self); return E_FAIL; }
    std::vector<uint8_t> buf;
    PsoStreamDesc nd = {};
    const std::vector<uint8_t>* r = nullptr;
    if (MakePatchedStream(desc, buf, nd, &r)) {
        HRESULT hr = o(self, &nd, riid, pp);
        if (SUCCEEDED(hr)) { NoteReplaced("CreatePipelineState", *r); return hr; }
        Log("patched CreatePipelineState failed (0x%08lx); using the original shader", (unsigned long)hr);
    }
    return o(self, desc, riid, pp);
}

// ---- pipeline libraries: a hit in the library would hand back the ORIGINAL compiled pipeline, so for shaders
// ---- we replace we build the pipeline ourselves instead.
// ID3D12DeviceChild::GetDevice is vtable slot 7 (we avoid the SDK's ID3D12PipelineLibrary type).
static HRESULT GetDeviceOf(void* obj, REFIID riid, void** out) {
    using PFN_GetDevice = HRESULT(STDMETHODCALLTYPE*)(void*, REFIID, void**);
    return ((PFN_GetDevice)(*(void***)obj)[7])(obj, riid, out);
}

using PFN_LoadGPS = HRESULT(STDMETHODCALLTYPE*)(void*, LPCWSTR, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*,
                                                REFIID, void**);
static HRESULT STDMETHODCALLTYPE Hook_LoadGPS(void* self, LPCWSTR name,
                                              const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID riid, void** pp) {
    Crumb crumb("LoadGraphicsPipeline");
    FIRST("LoadGraphicsPipeline");
    PFN_LoadGPS o = (PFN_LoadGPS)g_mLoadGPS.Find(self);
    if (!o) { Log("LoadGraphicsPipeline: no original for vtable %p", (void*)*(void***)self); return E_FAIL; }
    if (desc && desc->PS.pShaderBytecode) {
        const std::vector<uint8_t>* r = LookupReplacement(desc->PS);
        if (r) {
            ID3D12Device* dev = nullptr;
            if (SUCCEEDED(GetDeviceOf(self, __uuidof(ID3D12Device), (void**)&dev)) && dev) {
                D3D12_GRAPHICS_PIPELINE_STATE_DESC d = *desc;
                d.PS.pShaderBytecode = r->data();
                d.PS.BytecodeLength = r->size();
                d.CachedPSO = D3D12_CACHED_PIPELINE_STATE{};
                HRESULT hr = dev->CreateGraphicsPipelineState(&d, riid, pp);
                dev->Release();
                if (SUCCEEDED(hr)) { NoteReplaced("LoadGraphicsPipeline", *r); return hr; }
                Log("patched pipeline from LoadGraphicsPipeline failed (0x%08lx); using the library's pipeline", (unsigned long)hr);
            }
        }
    }
    return o(self, name, desc, riid, pp);
}

using PFN_LoadPipeline = HRESULT(STDMETHODCALLTYPE*)(void*, LPCWSTR, const PsoStreamDesc*,
                                                     REFIID, void**);
static HRESULT STDMETHODCALLTYPE Hook_LoadPipeline(void* self, LPCWSTR name,
                                                   const PsoStreamDesc* desc, REFIID riid, void** pp) {
    Crumb crumb("LoadPipeline");
    FIRST("LoadPipeline");
    PFN_LoadPipeline o = (PFN_LoadPipeline)g_mLoadPipeline.Find(self);
    if (!o) { Log("LoadPipeline: no original for vtable %p", (void*)*(void***)self); return E_FAIL; }
    std::vector<uint8_t> buf;
    PsoStreamDesc nd = {};
    const std::vector<uint8_t>* r = nullptr;
    if (MakePatchedStream(desc, buf, nd, &r)) {
        IUnknown* dev2 = nullptr;
        if (SUCCEEDED(GetDeviceOf(self, kDeviceIIDs[1], (void**)&dev2)) && dev2) {   // kDeviceIIDs[1] = ID3D12Device2
            PFN_CreatePS create = (PFN_CreatePS)(*(void***)dev2)[kDev_CreatePipelineState];
            HRESULT hr = create((ID3D12Device*)dev2, &nd, riid, pp);
            dev2->Release();
            if (SUCCEEDED(hr)) { NoteReplaced("LoadPipeline", *r); return hr; }
            Log("patched pipeline from LoadPipeline failed (0x%08lx); using the library's pipeline", (unsigned long)hr);
        }
    }
    return o(self, name, desc, riid, pp);
}

static void PatchLibrary(IUnknown* lib) {
    if (!lib) return;
    IUnknown* base = nullptr;
    if (SUCCEEDED(lib->QueryInterface(kIID_PipelineLibrary, (void**)&base)) && base) {
        Patch(*(void***)base, kLib_LoadGraphicsPipeline, (void*)Hook_LoadGPS, g_mLoadGPS);
        base->Release();
    }
    IUnknown* l1 = nullptr;
    if (SUCCEEDED(lib->QueryInterface(kIID_PipelineLibrary1, (void**)&l1)) && l1) {
        void** vt = *(void***)l1;
        Patch(vt, kLib_LoadGraphicsPipeline, (void*)Hook_LoadGPS, g_mLoadGPS);
        Patch(vt, kLib_LoadPipeline, (void*)Hook_LoadPipeline, g_mLoadPipeline);
        l1->Release();
    }
}

using PFN_CreateLib = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const void*, SIZE_T, REFIID, void**);
static HRESULT STDMETHODCALLTYPE Hook_CreateLib(ID3D12Device* self, const void* blob, SIZE_T len, REFIID riid, void** pp) {
    Crumb crumb("CreatePipelineLibrary");
    FIRST("CreatePipelineLibrary");
    PFN_CreateLib o = (PFN_CreateLib)g_mCreateLib.Find(self);
    if (!o) { Log("CreatePipelineLibrary: no original for vtable %p", (void*)*(void***)self); return E_FAIL; }
    HRESULT hr = o(self, blob, len, riid, pp);
    if (SUCCEEDED(hr) && pp && *pp) PatchLibrary((IUnknown*)*pp);
    return hr;
}

// ---------------------------------------------------------------------------------------------
// Device hooks + exported D3D12CreateDevice
// ---------------------------------------------------------------------------------------------
// level 0 = ID3D12Device, 1 = ID3D12Device1+, 2 = ID3D12Device2+ (which slots exist in that interface's vtable)
static void PatchDeviceVtable(void** vt, int level) {
    Patch(vt, kDev_CreateGraphicsPipelineState, (void*)Hook_CreateGPS, g_mCreateGPS);
    if (level >= 1 && g_cfg.hookLibraries) Patch(vt, kDev_CreatePipelineLibrary, (void*)Hook_CreateLib, g_mCreateLib);
    if (level >= 2 && g_cfg.hookStreams) Patch(vt, kDev_CreatePipelineState, (void*)Hook_CreatePS, g_mCreatePS);
}

static void InstallDeviceHooks(IUnknown* unk) {
    ID3D12Device* dev = nullptr;
    if (FAILED(unk->QueryInterface(__uuidof(ID3D12Device), (void**)&dev)) || !dev) return;
    PatchDeviceVtable(*(void***)dev, 0);
    // Each interface version may have its own vtable: patch the one it actually uses, with only the slots it has.
    for (size_t i = 0; i < sizeof(kDeviceIIDs) / sizeof(kDeviceIIDs[0]); i++) {
        IUnknown* p = nullptr;
        if (SUCCEEDED(unk->QueryInterface(kDeviceIIDs[i], (void**)&p)) && p) {
            PatchDeviceVtable(*(void***)p, i == 0 ? 1 : 2);
            p->Release();
        }
    }
    // The exact pointer the game receives (base slot only; its version is unknown).
    PatchDeviceVtable(*(void***)unk, 0);
    Log("device hooks installed (device %p)", (void*)dev);
    dev->Release();
}

static HMODULE g_realMod = nullptr;
static void ResolveFwd() {
    if (!g_realMod) {
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        wcscat_s(sys, L"\\d3d12.dll");
        g_realMod = LoadLibraryW(sys);
    }
    if (!g_realMod) return;
    for (size_t i = 0; i < sizeof(kFwdNames) / sizeof(kFwdNames[0]); i++)
        g_fwd[i] = (void*)GetProcAddress(g_realMod, kFwdNames[i] ? kFwdNames[i] : MAKEINTRESOURCEA(kFwdOrds[i]));
}

using PFN_CreateDevice = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using PFN_EnableExperimental = HRESULT(WINAPI*)(UINT, const IID*, void*, UINT*);
static PFN_CreateDevice g_realCreateDevice = nullptr;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK InitFn(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t dir[MAX_PATH];
    GetModuleFileNameW(g_self, dir, MAX_PATH);
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (slash) slash[1] = 0;          // keep the directory incl. trailing backslash
    swprintf(g_iniPath, MAX_PATH, L"%lsd3d12_proxy.ini", dir);
    swprintf(g_logPath, MAX_PATH, L"%lsd3d12_proxy.log", dir);
    swprintf(g_dumpDir, MAX_PATH, L"%lsd3d12_proxy_dump\\", dir);
    swprintf(g_replDir, MAX_PATH, L"%lsd3d12_proxy_replace\\", dir);
    DeleteFileW(g_logPath);

    g_cfg.installHooks = GetPrivateProfileIntW(L"Settings", L"InstallHooks", g_cfg.installHooks, g_iniPath);
    g_cfg.dump = GetPrivateProfileIntW(L"Settings", L"Dump", g_cfg.dump, g_iniPath);
    g_cfg.replace = GetPrivateProfileIntW(L"Settings", L"Replace", g_cfg.replace, g_iniPath);
    g_cfg.autoPatch = GetPrivateProfileIntW(L"Settings", L"AutoPatch", g_cfg.autoPatch, g_iniPath);
    g_cfg.cachePatches = GetPrivateProfileIntW(L"Settings", L"CachePatches", g_cfg.cachePatches, g_iniPath);
    g_cfg.hookLibraries = GetPrivateProfileIntW(L"Settings", L"HookLibraries", g_cfg.hookLibraries, g_iniPath);
    g_cfg.hookStreams = GetPrivateProfileIntW(L"Settings", L"HookStreams", g_cfg.hookStreams, g_iniPath);
    g_cfg.allowUnsigned = GetPrivateProfileIntW(L"Settings", L"AllowUnsigned", g_cfg.allowUnsigned, g_iniPath);
    if (g_cfg.dump) CreateDirectoryW(g_dumpDir, nullptr);
    if (g_cfg.replace || (g_cfg.autoPatch && g_cfg.cachePatches)) CreateDirectoryW(g_replDir, nullptr);
    AddVectoredExceptionHandler(1, CrashLogger);

    ResolveFwd();
    HMODULE real = g_realMod;
    if (real) g_realCreateDevice = (PFN_CreateDevice)GetProcAddress(real, "D3D12CreateDevice");
    Log("d3d12_proxy init: real d3d12 %p, installHooks=%d dump=%d replace=%d autoPatch=%d cachePatches=%d hookLibraries=%d hookStreams=%d allowUnsigned=%d",
        (void*)real, g_cfg.installHooks, g_cfg.dump, g_cfg.replace, g_cfg.autoPatch, g_cfg.cachePatches,
        g_cfg.hookLibraries, g_cfg.hookStreams, g_cfg.allowUnsigned);
    if (g_cfg.allowUnsigned && real) {
        PFN_EnableExperimental f = (PFN_EnableExperimental)GetProcAddress(real, "D3D12EnableExperimentalFeatures");
        if (f) {
            HRESULT hr = f(1, &kExperimentalShaderModels, nullptr, nullptr);
            Log("D3D12EnableExperimentalFeatures(ExperimentalShaderModels) -> 0x%08lx", (unsigned long)hr);
        }
    }
    return TRUE;
}

extern "C" HRESULT WINAPI Proxy_D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL fl, REFIID riid,
                                                  void** ppDevice) {
    InitOnceExecuteOnce(&g_once, InitFn, nullptr, nullptr);
    if (!g_realCreateDevice) return E_FAIL;
    HRESULT hr = g_realCreateDevice(adapter, fl, riid, ppDevice);
    Log("D3D12CreateDevice -> 0x%08lx (riid {%08lx-...})", (unsigned long)hr, (unsigned long)riid.Data1);
    if (g_cfg.installHooks && SUCCEEDED(hr) && ppDevice && *ppDevice) InstallDeviceHooks((IUnknown*)*ppDevice);
    return hr;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = (HMODULE)h;
        DisableThreadLibraryCalls(h);
        // Marker proving the proxy was loaded at all (independent of any later init).
        wchar_t p[MAX_PATH];
        GetModuleFileNameW(g_self, p, MAX_PATH);
        wchar_t* sl = wcsrchr(p, L'\\');
        if (sl) { wcscpy_s(sl + 1, p + MAX_PATH - (sl + 1), L"d3d12_proxy_loaded.txt"); }
        HANDLE f = CreateFileW(p, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            char msg[64];
            int len = wsprintfA(msg, "proxy loaded, pid %lu\r\n", GetCurrentProcessId());
            DWORD wr; WriteFile(f, msg, len, &wr, nullptr); CloseHandle(f);
        }
        ResolveFwd();  // stubs must work even if the game calls another export first
    }
    return TRUE;
}
