// DisplayWakeUp: hides the HDMI-audio "ghost" monitor, keeps its audio endpoint alive, exits.
//
//  1. Detect: an HDMI target whose EDID (vendor + serial) duplicates another connected display
//     on a non-HDMI connector, the signature of an HDMI audio extractor in front of a monitor
//     that is already driven over DisplayPort.
//  2. Hide: the exact call behind Settings > Advanced display > "Remove display from desktop"
//     (RE: SettingsHandlers_PCDisplay.dll!DisplaySettingsManager::SetMonitorSpecialized ->
//     SystemSettingsAdminFlows.exe "SpecializeDisplay" -> DisplayConfigSetDeviceInfo type -23).
//     Needs admin: one UAC prompt on the first run, the setting persists after that.
//  3. Wake: activate every SpecialPurpose display at its lowest mode so its audio comes up.
//
// No CRT, no exceptions, no RTTI, no C++/WinRT, no console window. Steady-state imports:
// kernel32 + ntdll + WinRT API sets; user32/shell32 load only on the one-time hide path.
//
// Args (case-insensitive):
//   --min         lowest resolution instead of preferred resolution when waking
//   --verbose/-v  print what happens (to the calling console, or stdout if redirected)
//   --dry-run     detect and print only, change nothing (implies --verbose)
//   --restore     undo: put hidden (SpecialPurpose) displays back on the desktop
//   --no-hide     skip ghost detection, only wake already-hidden displays
//   --no-console  ignored (compatibility)
// Exit: 0 ok | 1 no SpecialPurpose display | 2 a step failed | 3 WinRT init failed | 4 UAC declined

#include <windows.h>
#include <winternl.h>
#include <shellapi.h>
#include <roapi.h>
#include <winstring.h>
#include <intrin.h>
#include <windows.devices.display.core.h>

using namespace ABI::Windows::Devices::Display::Core;
using ABI::Windows::Devices::Display::IDisplayMonitor;
using ABI::Windows::Devices::Display::DisplayMonitorUsageKind;
using ABI::Windows::Devices::Display::DisplayMonitorPhysicalConnectorKind;
using ABI::Windows::Foundation::Collections::IIterable;
using ABI::Windows::Foundation::Collections::IIterator;
using ABI::Windows::Foundation::Collections::IVectorView;
using ABI::Windows::Foundation::Numerics::Rational;
using ABI::Windows::Graphics::SizeInt32;

extern "C" NTSYSAPI NTSTATUS NTAPI RtlHashUnicodeString(PCUNICODE_STRING, BOOLEAN, ULONG, PULONG);

// ---------------------------------------------------------------- no-CRT shims
extern "C" {
#pragma function(memset, memcpy)
void* __cdecl memset(void* d, int c, size_t n) { __stosb(static_cast<unsigned char*>(d), static_cast<unsigned char>(c), n); return d; }
void* __cdecl memcpy(void* d, const void* s, size_t n) { __movsb(static_cast<unsigned char*>(d), static_cast<const unsigned char*>(s), n); return d; }
int __cdecl _purecall() { ExitProcess(0xC0000025); }
}

constexpr auto kUsageSpecial   = ABI::Windows::Devices::Display::DisplayMonitorUsageKind_SpecialPurpose;
constexpr auto kConnectorHdmi  = ABI::Windows::Devices::Display::DisplayMonitorPhysicalConnectorKind_Hdmi;
constexpr auto kDescriptorEdid = ABI::Windows::Devices::Display::DisplayMonitorDescriptorKind_Edid;

enum Exit : UINT { kOk = 0, kNoDisplay = 1, kFailed = 2, kInitFailed = 3, kUacDeclined = 4 };

// ---------------------------------------------------------------- logging (--verbose only)
static HANDLE g_out;

struct Line {
    char b[200];
    int n = 0;
    Line& s(const char* p) { while (*p && n < 196) b[n++] = *p++; return *this; }
    Line& w(const wchar_t* p, UINT32 len) { for (UINT32 i = 0; i < len && n < 196; ++i) b[n++] = p[i] < 128 ? char(p[i]) : '?'; return *this; }
    Line& u(UINT64 v) { char t[20]; int k = 0; do { t[k++] = char('0' + v % 10); v /= 10; } while (v); while (k && n < 196) b[n++] = t[--k]; return *this; }
    Line& x(UINT32 v) { s("0x"); for (int i = 28; i >= 0; i -= 4) if (n < 196) b[n++] = "0123456789ABCDEF"[(v >> i) & 0xF]; return *this; }
    void out() { if (!g_out) return; b[n++] = '\r'; b[n++] = '\n'; DWORD wr; WriteFile(g_out, b, n, &wr, nullptr); }
};

static HRESULT Fail(const char* what, HRESULT hr) { Line().s(what).s(" failed: ").x(static_cast<UINT32>(hr)).out(); return FAILED(hr) ? hr : E_FAIL; }

// ---------------------------------------------------------------- tiny COM helpers
template <class T> struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { if (p) p->Release(); }
    T** put() { return &p; }
    T* operator->() const { return p; }
    operator T*() const { return p; }
};

static constexpr IID kIID_IAgileObject = { 0x94ea2b94, 0xe9cc, 0x49e0, { 0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90 } };

// Non-refcounted IInspectable for objects that live in Entry()'s frame (= process lifetime).
template <class I> struct StaticInspectable : I {
    STDMETHODIMP QueryInterface(REFIID iid, void** pp) override {
        if (InlineIsEqualGUID(iid, __uuidof(I)) || InlineIsEqualGUID(iid, __uuidof(IInspectable)) ||
            InlineIsEqualGUID(iid, __uuidof(IUnknown)) || InlineIsEqualGUID(iid, kIID_IAgileObject)) {
            *pp = static_cast<I*>(this);
            return S_OK;
        }
        *pp = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP GetIids(ULONG* n, IID** iids) override { *n = 0; *iids = nullptr; return S_OK; }
    STDMETHODIMP GetRuntimeClassName(HSTRING* name) override { *name = nullptr; return S_OK; }
    STDMETHODIMP GetTrustLevel(TrustLevel* t) override { *t = BaseTrust; return S_OK; }
};

// Single-element IIterable<DisplayTarget> for TryAcquireTargetsAndCreateEmptyState.
struct OneTargetIterator final : StaticInspectable<IIterator<DisplayTarget*>> {
    IDisplayTarget* target = nullptr;
    bool has = false;
    STDMETHODIMP get_Current(IDisplayTarget** cur) override {
        if (!has) { *cur = nullptr; return E_BOUNDS; }
        target->AddRef();
        *cur = target;
        return S_OK;
    }
    STDMETHODIMP get_HasCurrent(boolean* v) override { *v = has; return S_OK; }
    STDMETHODIMP MoveNext(boolean* v) override { has = false; *v = false; return S_OK; }
    STDMETHODIMP GetMany(UINT cap, IDisplayTarget** items, UINT* actual) override {
        *actual = 0;
        if (has && cap) { target->AddRef(); items[0] = target; *actual = 1; has = false; }
        return S_OK;
    }
};

struct OneTargetIterable final : StaticInspectable<IIterable<DisplayTarget*>> {
    OneTargetIterator* it = nullptr;
    STDMETHODIMP First(IIterator<DisplayTarget*>** first) override { it->has = true; *first = it; return S_OK; }
};

static IDisplayManager* CreateManager()
{
    static constexpr wchar_t kClass[] = L"Windows.Devices.Display.Core.DisplayManager";
    HSTRING_HEADER hdr;
    HSTRING cls = nullptr;
    WindowsCreateStringReference(kClass, ARRAYSIZE(kClass) - 1, &hdr, &cls);

    Com<IDisplayManagerStatics> statics;
    IDisplayManager* mgr = nullptr;
    HRESULT hr = RoGetActivationFactory(cls, __uuidof(IDisplayManagerStatics), reinterpret_cast<void**>(statics.put()));
    if (FAILED(hr)) { Fail("RoGetActivationFactory(DisplayManager)", hr); return nullptr; }
    if (FAILED(hr = statics->Create(DisplayManagerOptions_None, &mgr))) { Fail("DisplayManager::Create", hr); return nullptr; }
    return mgr;
}

// ================================================================ 1. ghost detection
struct Target {
    IDisplayTarget* t;          // ref intentionally kept until exit
    bool special, hdmi;
    UINT16 vendor;              // EDID bytes 8-9 (PNP id)
    UINT32 serialNum;           // EDID bytes 12-15
    char serial[14];            // EDID descriptor 0xFF (serial string)
};
static Target g_targets[32];
static UINT g_count;

static void ParseEdid(Target& d, const BYTE* e, UINT32 len)
{
    static constexpr BYTE kHeader[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    if (len < 128) return;
    for (int i = 0; i < 8; ++i) if (e[i] != kHeader[i]) return;
    d.vendor = static_cast<UINT16>(e[8] << 8 | e[9]);
    d.serialNum = e[12] | e[13] << 8 | e[14] << 16 | static_cast<UINT32>(e[15]) << 24;
    for (int off = 54; off <= 108; off += 18) {
        const BYTE* dsc = e + off;
        if (dsc[0] || dsc[1] || dsc[3] != 0xFF) continue;
        int k = 0;
        for (; k < 13 && dsc[5 + k] != 0x0A && dsc[5 + k] >= 0x20; ++k) d.serial[k] = static_cast<char>(dsc[5 + k]);
        d.serial[k] = 0;
    }
}

static void CollectTargets(IDisplayManager* mgr)
{
    g_count = 0;
    IVectorView<DisplayTarget*>* list = nullptr;
    if (FAILED(mgr->GetCurrentTargets(&list))) return;
    UINT n = 0;
    list->get_Size(&n);
    for (UINT i = 0; i < n && g_count < ARRAYSIZE(g_targets); ++i) {
        IDisplayTarget* t = nullptr;
        if (FAILED(list->GetAt(i, &t))) continue;
        boolean connected = false;
        Com<IDisplayMonitor> mon;
        if (FAILED(t->get_IsConnected(&connected)) || !connected || FAILED(t->TryGetMonitor(mon.put())) || !mon) { t->Release(); continue; }

        Target& d = g_targets[g_count++];
        d = {};
        d.t = t;
        DisplayMonitorUsageKind kind{};
        t->get_UsageKind(&kind);
        d.special = kind == kUsageSpecial;
        DisplayMonitorPhysicalConnectorKind conn{};
        mon->get_PhysicalConnector(&conn);
        d.hdmi = conn == kConnectorHdmi;
        UINT32 len = 0;
        BYTE* edid = nullptr;                    // CoTaskMem block, freed by process exit
        if (SUCCEEDED(mon->GetDescriptor(kDescriptorEdid, &len, &edid)) && edid) ParseEdid(d, edid, len);
    }
}

static bool SameMonitor(const Target& a, const Target& b)
{
    if (!a.vendor || a.vendor != b.vendor) return false;
    if (a.serial[0] && b.serial[0]) return lstrcmpA(a.serial, b.serial) == 0;
    return a.serialNum && a.serialNum == b.serialNum;
}

// HDMI target, still on the desktop, whose EDID matches a display on another connector.
static bool IsGhost(UINT i)
{
    const Target& g = g_targets[i];
    if (!g.hdmi || g.special) return false;
    for (UINT j = 0; j < g_count; ++j)
        if (j != i && !g_targets[j].hdmi && SameMonitor(g, g_targets[j])) return true;
    return false;
}

static void LogTarget(const char* tag, UINT i)
{
    const Target& d = g_targets[i];
    HSTRING id = nullptr;
    d.t->get_StableMonitorId(&id);
    UINT32 len = 0;
    const wchar_t* s = WindowsGetStringRawBuffer(id, &len);
    Line().s(tag).s(" ").w(s, len).s(d.hdmi ? " [HDMI]" : " [other]").s(d.special ? " hidden" : " on-desktop")
          .s(" serial=").s(d.serial[0] ? d.serial : "-").out();
}

// ================================================================ 2. hide (RE'd Settings toggle)
#pragma pack(push, 4)
struct SpecializePacket {                       // DisplayConfigSetDeviceInfo, undocumented type -23
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;    // +0x00 type, size, adapter LUID, target id
    GUID   overrideType;                        // +0x14 GUID_MONITOR_OVERRIDE_PSEUDO_SPECIALIZED
    INT32  enable;                              // +0x24
    UINT64 hash;                                // +0x28
};
#pragma pack(pop)
static_assert(sizeof(SpecializePacket) == 48, "layout from SystemSettingsAdminFlows.exe");
static_assert(offsetof(SpecializePacket, overrideType) == 0x14 && offsetof(SpecializePacket, enable) == 0x24 && offsetof(SpecializePacket, hash) == 0x28, "layout");

static constexpr GUID kPseudoSpecialized = { 0xf196c02f, 0xf86f, 0x4f9a, { 0xaa, 0x15, 0xe9, 0xce, 0xbd, 0xfe, 0x3b, 0x96 } };

// RtlHashUnicodeString(X65599, case-insensitive) over StableMonitorId + "{GUID}": first half of
// the string -> low dword, second half -> high dword. Mirrors SetMonitorSpecialized exactly.
static UINT64 SpecializationHash(HSTRING stableId)
{
    static constexpr wchar_t kGuid[] = L"{F196C02F-F86F-4F9A-AA15-E9CEBDFE3B96}";
    wchar_t buf[0x288 / sizeof(wchar_t)];
    UINT32 n = 0, len = 0;
    const wchar_t* s = WindowsGetStringRawBuffer(stableId, &len);
    if (len + ARRAYSIZE(kGuid) <= ARRAYSIZE(buf)) while (n < len) { buf[n] = s[n]; ++n; }
    for (UINT32 i = 0; i + 1 < ARRAYSIZE(kGuid); ++i) buf[n++] = kGuid[i];

    UNICODE_STRING u{ static_cast<USHORT>(n * sizeof(wchar_t)), static_cast<USHORT>(sizeof(buf)), buf };
    const USHORT full = u.Length;
    ULONG h[2] = {};
    u.Length = (full >> 1) & 0x7FFE;
    RtlHashUnicodeString(&u, TRUE, 0, &h[0]);
    if (full > 2) {
        u.Buffer += u.Length / sizeof(wchar_t);
        u.MaximumLength = static_cast<USHORT>(u.MaximumLength - u.Length);
        u.Length = static_cast<USHORT>(full - u.Length);
        RtlHashUnicodeString(&u, TRUE, 0, &h[1]);
    }
    return h[0] | static_cast<UINT64>(h[1]) << 32;
}

static LONG SetSpecialized(IDisplayTarget* t, bool enable)
{
    Com<IDisplayAdapter> adapter;
    ABI::Windows::Graphics::DisplayAdapterId luid{};
    UINT32 targetId = 0;
    HSTRING stableId = nullptr;
    if (FAILED(t->get_Adapter(adapter.put())) || FAILED(adapter->get_Id(&luid)) ||
        FAILED(t->get_AdapterRelativeId(&targetId)) || FAILED(t->get_StableMonitorId(&stableId)) || !stableId)
        return ERROR_INVALID_PARAMETER;

    SpecializePacket p{};
    p.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(-23);
    p.header.size = sizeof(p);
    p.header.adapterId.LowPart = luid.LowPart;
    p.header.adapterId.HighPart = luid.HighPart;
    p.header.id = targetId;
    p.overrideType = kPseudoSpecialized;
    p.enable = enable;
    p.hash = SpecializationHash(stableId);

    auto set = reinterpret_cast<decltype(&DisplayConfigSetDeviceInfo)>(GetProcAddress(LoadLibraryW(L"user32.dll"), "DisplayConfigSetDeviceInfo"));
    return set ? set(&p.header) : ERROR_PROC_NOT_FOUND;
}

// Re-run this exe elevated with the same args (one UAC prompt), wait, return its exit code.
static UINT RelaunchElevated(const wchar_t* args)
{
    wchar_t exe[MAX_PATH], params[512];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    UINT n = 0;
    for (; args[n] && n < 480; ++n) params[n] = args[n];
    for (const wchar_t* p = L" --elevated"; *p; ++p) params[n++] = *p;
    params[n] = 0;

    auto shellExec = reinterpret_cast<decltype(&ShellExecuteExW)>(GetProcAddress(LoadLibraryW(L"shell32.dll"), "ShellExecuteExW"));
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = params;
    sei.nShow = SW_HIDE;
    if (!shellExec || !shellExec(&sei) || !sei.hProcess) { Line().s("elevation declined/failed").out(); return kUacDeclined; }
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = kFailed;
    GetExitCodeProcess(sei.hProcess, &code);
    Line().s("elevated instance finished, exit ").u(code).out();
    return code;
}

// ================================================================ 3. wake
struct Pick { IDisplayModeInfo* mode = nullptr; UINT64 px = ~0ull; Rational hz{ 0, 1 }; SizeInt32 res{}; };

// Lowest pixel count, then lowest vsync rate. Returned mode is AddRef'd.
static Pick PickLowestMode(IVectorView<DisplayModeInfo*>* modes)
{
    Pick best;
    UINT n = 0;
    modes->get_Size(&n);
    for (UINT i = 0; i < n; ++i) {
        IDisplayModeInfo* m = nullptr;
        if (FAILED(modes->GetAt(i, &m))) continue;
        SizeInt32 r{};
        DisplayPresentationRate pr{};
        if (FAILED(m->get_TargetResolution(&r)) || FAILED(m->get_PresentationRate(&pr)) || !pr.VerticalSyncRate.Denominator) { m->Release(); continue; }

        const UINT64 px = static_cast<UINT64>(static_cast<UINT32>(r.Width)) * static_cast<UINT32>(r.Height);
        const Rational hz = pr.VerticalSyncRate;   // a < b  <=>  a.N*b.D < b.N*a.D
        const bool lower = !best.mode || px < best.px ||
            (px == best.px && static_cast<UINT64>(hz.Numerator) * best.hz.Denominator < static_cast<UINT64>(best.hz.Numerator) * hz.Denominator);
        if (lower) { if (best.mode) best.mode->Release(); best = { m, px, hz, r }; }
        else m->Release();
    }
    return best;
}

static HRESULT WakeTarget(IDisplayManager* mgr, IDisplayTarget* target, OneTargetIterable* one, bool preferredRes)
{
    one->it->target = target;

    Com<IDisplayManagerResultWithState> acq;
    HRESULT hr = mgr->TryAcquireTargetsAndCreateEmptyState(one, acq.put());
    if (FAILED(hr)) return Fail("TryAcquireTargetsAndCreateEmptyState", hr);

    DisplayManagerResult err = DisplayManagerResult_Success;
    acq->get_ErrorCode(&err);
    if (err != DisplayManagerResult_Success) {
        HRESULT ext = S_OK;
        acq->get_ExtendedErrorCode(&ext);
        Line().s("acquire target failed: DisplayManagerResult ").u(static_cast<UINT32>(err)).s(", ").x(static_cast<UINT32>(ext)).out();
        return FAILED(ext) ? ext : E_FAIL;
    }

    Com<IDisplayState> state;
    if (FAILED(hr = acq->get_State(state.put()))) return Fail("get_State", hr);

    Com<IDisplayPath> path;
    if (FAILED(hr = state->ConnectTarget(target, path.put()))) return Fail("ConnectTarget", hr);

    Com<IVectorView<DisplayModeInfo*>> modes;
    hr = path->FindModes(preferredRes ? DisplayModeQueryOptions_OnlyPreferredResolution : DisplayModeQueryOptions_None, modes.put());
    if (FAILED(hr)) return Fail("FindModes", hr);

    Pick pick = PickLowestMode(modes);
    if (!pick.mode) return Fail("FindModes (no usable mode)", E_FAIL);
    Com<IDisplayModeInfo> mode;
    mode.p = pick.mode;

    if (FAILED(hr = path->ApplyPropertiesFromMode(mode))) return Fail("ApplyPropertiesFromMode", hr);

    Com<IDisplayStateOperationResult> op;
    if (FAILED(hr = state->TryApply(DisplayStateApplyOptions_None, op.put()))) return Fail("TryApply", hr);

    DisplayStateOperationStatus st = DisplayStateOperationStatus_Success;
    op->get_Status(&st);
    if (st != DisplayStateOperationStatus_Success) {
        HRESULT ext = S_OK;
        op->get_ExtendedErrorCode(&ext);
        Line().s("TryApply failed: DisplayStateOperationStatus ").u(static_cast<UINT32>(st)).s(", ").x(static_cast<UINT32>(ext)).out();
        return FAILED(ext) ? ext : E_FAIL;
    }

    const UINT64 mhz = static_cast<UINT64>(pick.hz.Numerator) * 1000 / pick.hz.Denominator;
    Line().s("woke display: ").u(static_cast<UINT32>(pick.res.Width)).s("x").u(static_cast<UINT32>(pick.res.Height))
          .s(" @ ").u(mhz / 1000).s(".").u(mhz % 1000 / 100).u(mhz % 100 / 10).u(mhz % 10).s(" Hz").out();
    return S_OK;
}

// ---------------------------------------------------------------- args
struct Args { bool preferredRes = true, verbose = false, dryRun = false, restore = false, noHide = false, elevated = false; const wchar_t* tail = L""; };

static bool TokenIs(const wchar_t* t, size_t n, const char* lit)
{
    size_t i = 0;
    for (; i < n && lit[i]; ++i) {
        wchar_t c = t[i];
        if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
        if (c != static_cast<wchar_t>(lit[i])) return false;
    }
    return i == n && !lit[i];
}

static void ParseArgs(Args& a)
{
    const wchar_t* p = GetCommandLineW();
    if (*p == L'"') { ++p; while (*p && *p != L'"') ++p; if (*p) ++p; }   // skip argv[0]
    else while (*p > L' ') ++p;
    while (*p && *p <= L' ') ++p;
    a.tail = p;

    for (;;) {
        while (*p && *p <= L' ') ++p;
        if (!*p) break;
        const wchar_t* t = p;
        while (*p > L' ') ++p;
        const size_t n = static_cast<size_t>(p - t);
        if (TokenIs(t, n, "--min")) a.preferredRes = false;
        else if (TokenIs(t, n, "--verbose") || TokenIs(t, n, "-v")) a.verbose = true;
        else if (TokenIs(t, n, "--dry-run")) a.dryRun = a.verbose = true;
        else if (TokenIs(t, n, "--restore")) a.restore = true;
        else if (TokenIs(t, n, "--no-hide")) a.noHide = true;
        else if (TokenIs(t, n, "--elevated")) a.elevated = true;
    }
}

// ---------------------------------------------------------------- entry
extern "C" void WINAPI Entry()
{
    Args a;
    ParseArgs(a);

    if (a.verbose) {
        g_out = GetStdHandle(STD_OUTPUT_HANDLE);                   // redirected: pipe / file
        if ((!g_out || g_out == INVALID_HANDLE_VALUE) && AttachConsole(ATTACH_PARENT_PROCESS)) {
            g_out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            Line().out();                                          // step off the shell prompt
        }
        if (g_out == INVALID_HANDLE_VALUE) g_out = nullptr;
    }

    HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(hr)) { Fail("RoInitialize", hr); ExitProcess(kInitFailed); }

    IDisplayManager* mgr = CreateManager();
    if (!mgr) ExitProcess(kInitFailed);

    // ---- 1+2: detect ghost(s) and hide them / or restore
    UINT changed = 0, hideFailed = 0;
    if (!a.noHide || a.restore) {
        CollectTargets(mgr);
        for (UINT i = 0; i < g_count; ++i) {
            const bool act = a.restore ? g_targets[i].special : IsGhost(i);
            if (a.verbose) LogTarget(act ? (a.restore ? "restore:" : "ghost:  ") : "display:", i);
            if (!act || a.dryRun) continue;

            const LONG err = SetSpecialized(g_targets[i].t, !a.restore);
            if (err == ERROR_ACCESS_DENIED && !a.elevated) ExitProcess(RelaunchElevated(a.tail));
            if (err) { Line().s(a.restore ? "restore" : "hide").s(" failed: win32 ").u(static_cast<UINT32>(err)).out(); ++hideFailed; }
            else { Line().s(a.restore ? "  -> back on desktop" : "  -> removed from desktop").out(); ++changed; }
        }
        if (a.restore || a.dryRun) ExitProcess(hideFailed ? kFailed : kOk);
    }

    // ---- 3: wake every SpecialPurpose display (retry while a fresh hide settles)
    OneTargetIterator it;
    OneTargetIterable one;
    one.it = &it;
    UINT found = 0, failed = 0;
    for (int attempt = 0;; ++attempt) {
        if (changed) {                                             // fresh view after a topology change
            mgr->Release();
            mgr = CreateManager();
            if (!mgr) ExitProcess(kInitFailed);
        }
        found = failed = 0;
        IVectorView<DisplayTarget*>* targets = nullptr;
        if (FAILED(hr = mgr->GetCurrentTargets(&targets))) { Fail("GetCurrentTargets", hr); ExitProcess(kInitFailed); }
        UINT n = 0;
        targets->get_Size(&n);
        for (UINT i = 0; i < n; ++i) {
            Com<IDisplayTarget> t;
            if (FAILED(targets->GetAt(i, t.put()))) continue;
            DisplayMonitorUsageKind kind{};
            if (FAILED(t->get_UsageKind(&kind)) || kind != kUsageSpecial) continue;
            ++found;
            if (FAILED(WakeTarget(mgr, t, &one, a.preferredRes))) ++failed;
        }
        targets->Release();
        if ((found && !failed) || !changed || attempt >= 30) break;
        Sleep(100);
    }

    // mgr is deliberately not released: target ownership ends with the process, like the original.
    if (!found) Line().s("no SpecialPurpose display found").out();
    ExitProcess(!found ? kNoDisplay : (failed || hideFailed) ? kFailed : kOk);
}
