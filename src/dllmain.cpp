// version.dll proxy for Ori and the Will of the Wisps (Unity IL2CPP, x64)
//
// Delays the final stage of a *backup save* load until at least 4000 ms have
// passed since the backup load started. No Sleep() on the game thread: the
// game's own per-tick polling loop is used to postpone completion.
//
// Call chain (from dump.cs + disassembly of GameAssembly.dll):
//
//   BackupSaveCleverMenuItem.OnPress -> LoadGameAction.Perform
//   LoadGameAction.LoadGame(isBackupLoad = true, ...)          [START, RVA 0xFB4500]
//     -> InstantLoadScenesController.set_LockFinishingLoading(true)
//     -> SaveGameController.PerformLoad                          (0xC2D950)
//          -> LoadUberState -> RestoreCheckpoint                 (0xC2E870)
//          -> [LateStart] RestoreCheckpointPart1                 (0xC2EA70)
//          -> GoToSceneController.GoToSceneAsync(onComplete = OnFinishedLoading)
//          -> InstantLoadScenesController.LoadScenesAtPosition   (0x62ACF0)
//     <- set_LockFinishingLoading(false)
//   every fixed tick:
//   InstantLoadScenesController.OnScenesManagerFixedUpdate      [GATE,  RVA 0x62B3D0]
//     if (m_isLoading && !IsLoadingScene && !m_lockFinishLoading)
//        -> [LateStart] InstantLoadScenesController.CompleteLoading (0x62B8A0)
//           -> GoToSceneController.OnInstantLoadScenesControllerCompletedLoading
//           -> SaveGameController.OnFinishedLoading              [FINAL, RVA 0xC2F1C0]
//              -> GameController.RestoreCheckpointImmediate (control back to player)
//
// Strategy:
//   * LoadGame hook: if isBackupLoad -> remember steady_clock start.
//   * OnScenesManagerFixedUpdate hook: while < 4000 ms, force the game's own
//     m_lockFinishLoading flag (+0x58) to true for the duration of the call, so
//     scene streaming continues and the loading screen keeps rendering, but the
//     completion is not scheduled. The game simply retries next tick.
//   * OnFinishedLoading hook (safety net for any other path): if reached before
//     4000 ms, it is deferred and re-invoked from the fixed-tick hook later.

#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include "MinHook.h"

// ---------------------------------------------------------------------------
// version.dll forwarding (stubs live in exports.asm)
// ---------------------------------------------------------------------------
extern "C" FARPROC g_origVersion[17] = {};

static const char* const kVersionExports[17] = {
    "GetFileVersionInfoA",      "GetFileVersionInfoByHandle", "GetFileVersionInfoExA",
    "GetFileVersionInfoExW",    "GetFileVersionInfoSizeA",    "GetFileVersionInfoSizeExA",
    "GetFileVersionInfoSizeExW","GetFileVersionInfoSizeW",    "GetFileVersionInfoW",
    "VerFindFileA",             "VerFindFileW",               "VerInstallFileA",
    "VerInstallFileW",          "VerLanguageNameA",           "VerLanguageNameW",
    "VerQueryValueA",           "VerQueryValueW",
};

static HMODULE g_realVersion = nullptr;

static bool LoadRealVersionDll()
{
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n > MAX_PATH - 20) return false;
    wcscat_s(path, L"\\version.dll");
    g_realVersion = LoadLibraryW(path);
    if (!g_realVersion) return false;
    for (int i = 0; i < 17; ++i)
        g_origVersion[i] = GetProcAddress(g_realVersion, kVersionExports[i]);
    return true;
}

// ---------------------------------------------------------------------------
// Game offsets (dump.cs, GameAssembly.dll)
// ---------------------------------------------------------------------------
namespace rva {
constexpr uintptr_t LoadGameAction_LoadGame                     = 0xFB4500;
constexpr uintptr_t InstantLoadScenesController_FixedUpdate     = 0x62B3D0; // OnScenesManagerFixedUpdate
constexpr uintptr_t SaveGameController_OnFinishedLoading        = 0xC2F1C0;
} // namespace rva

namespace field {
constexpr size_t ILSC_m_lockFinishLoading = 0x58; // InstantLoadScenesController.m_lockFinishLoading
} // namespace field

// Expected prologue bytes, to refuse hooking a different game build.
struct Signature { uintptr_t rva; uint8_t bytes[12]; };
static const Signature kSignatures[] = {
    { rva::LoadGameAction_LoadGame,
      {0x48,0x89,0x54,0x24,0x10,0x55,0x56,0x57,0x48,0x83,0xEC,0x70} },
    { rva::InstantLoadScenesController_FixedUpdate,
      {0x4C,0x8B,0xDC,0x57,0x48,0x81,0xEC,0x90,0x00,0x00,0x00,0x49} },
    { rva::SaveGameController_OnFinishedLoading,
      {0x40,0x57,0x48,0x83,0xEC,0x50,0x48,0xC7,0x44,0x24,0x30,0xFE} },
};

constexpr std::chrono::milliseconds kMinBackupLoadTime{4000};

// ---------------------------------------------------------------------------
// State (all hooks run on the Unity main thread; atomics just for safety)
// ---------------------------------------------------------------------------
using Clock = std::chrono::steady_clock;

static std::atomic<bool>   g_backupActive{false};
static Clock::time_point   g_backupStart{};
static std::atomic<bool>   g_hasDeferred{false};
static void*               g_deferredSelf   = nullptr;
static const void*         g_deferredMethod = nullptr;

static bool StillBlocking()
{
    return g_backupActive.load() && (Clock::now() - g_backupStart) < kMinBackupLoadTime;
}

// IL2CPP calling convention: managed args, then hidden const MethodInfo*.
using LoadGame_t          = void (*)(bool isBackupLoad, void* failedMsgProvider, const void* method);
using FixedUpdate_t       = void (*)(uint8_t* self, const void* method);
using OnFinishedLoading_t = void (*)(void* self, const void* method);

static LoadGame_t          o_LoadGame          = nullptr;
static FixedUpdate_t       o_FixedUpdate       = nullptr;
static OnFinishedLoading_t o_OnFinishedLoading = nullptr;

// --- START: LoadGameAction.LoadGame(bool isBackupLoad, MessageProvider) ----
static void hk_LoadGame(bool isBackupLoad, void* failedMsgProvider, const void* method)
{
    if (isBackupLoad) {
        g_backupStart  = Clock::now();
        g_hasDeferred  = false;
        g_deferredSelf = nullptr;
        g_backupActive = true;
    } else {
        g_backupActive = false;
        g_hasDeferred  = false;
    }
    o_LoadGame(isBackupLoad, failedMsgProvider, method);
}

// --- GATE: InstantLoadScenesController.OnScenesManagerFixedUpdate ----------
static void hk_FixedUpdate(uint8_t* self, const void* method)
{
    if (self && g_backupActive.load()) {
        if (StillBlocking()) {
            // Keep the game's own "lock finishing loading" flag raised while the
            // original runs: scenes keep streaming, the loading screen keeps
            // rendering, but CompleteLoading is not scheduled this tick.
            uint8_t& lock  = self[field::ILSC_m_lockFinishLoading];
            const uint8_t saved = lock;
            lock = 1;
            o_FixedUpdate(self, method);
            if (lock == 1) lock = saved;
            return;
        }

        // Time is up. If OnFinishedLoading arrived early by another path, run it now.
        if (g_hasDeferred.exchange(false)) {
            void* s = g_deferredSelf;
            const void* m = g_deferredMethod;
            g_deferredSelf = nullptr;
            g_backupActive = false;
            o_OnFinishedLoading(s, m);
        }
    }
    o_FixedUpdate(self, method);
}

// --- FINAL: SaveGameController.OnFinishedLoading ---------------------------
static void hk_OnFinishedLoading(void* self, const void* method)
{
    if (g_backupActive.load()) {
        if (StillBlocking()) {
            g_deferredSelf   = self;
            g_deferredMethod = method;
            g_hasDeferred    = true;
            return;
        }
        g_backupActive = false;
        g_hasDeferred  = false;
    }
    o_OnFinishedLoading(self, method);
}

// ---------------------------------------------------------------------------
// Hook installation
// ---------------------------------------------------------------------------
static HANDLE g_gameAssemblyEvent = nullptr;
static void*  g_dllNotifyCookie   = nullptr;

static bool CheckSignatures(uint8_t* base)
{
    for (const auto& s : kSignatures) {
        if (memcmp(base + s.rva, s.bytes, sizeof(s.bytes)) != 0)
            return false;
    }
    return true;
}

static void InstallHooks(HMODULE gameAssembly)
{
    auto* base = reinterpret_cast<uint8_t*>(gameAssembly);
    if (!CheckSignatures(base)) return;
    if (MH_Initialize() != MH_OK) return;

    bool ok = true;
    ok &= MH_CreateHook(base + rva::LoadGameAction_LoadGame, (void*)&hk_LoadGame,
                        (void**)&o_LoadGame) == MH_OK;
    ok &= MH_CreateHook(base + rva::InstantLoadScenesController_FixedUpdate, (void*)&hk_FixedUpdate,
                        (void**)&o_FixedUpdate) == MH_OK;
    ok &= MH_CreateHook(base + rva::SaveGameController_OnFinishedLoading, (void*)&hk_OnFinishedLoading,
                        (void**)&o_OnFinishedLoading) == MH_OK;
    if (!ok) {
        MH_RemoveHook(MH_ALL_HOOKS);
        return;
    }

    MH_EnableHook(MH_ALL_HOOKS);
}

// LdrRegisterDllNotification: get told when GameAssembly.dll is mapped, no polling.
struct UNICODE_STRING_ { USHORT Length, MaximumLength; PWSTR Buffer; };
struct LDR_DLL_LOADED_NOTIFICATION_DATA_ {
    ULONG Flags; const UNICODE_STRING_* FullDllName; const UNICODE_STRING_* BaseDllName;
    PVOID DllBase; ULONG SizeOfImage;
};
using LdrDllNotification_t = VOID(CALLBACK*)(ULONG, const LDR_DLL_LOADED_NOTIFICATION_DATA_*, PVOID);
using LdrRegisterDllNotification_t   = LONG(NTAPI*)(ULONG, LdrDllNotification_t, PVOID, PVOID*);
using LdrUnregisterDllNotification_t = LONG(NTAPI*)(PVOID);
constexpr ULONG LDR_DLL_NOTIFICATION_REASON_LOADED = 1;

static VOID CALLBACK OnDllNotification(ULONG reason, const LDR_DLL_LOADED_NOTIFICATION_DATA_* data, PVOID)
{
    if (reason != LDR_DLL_NOTIFICATION_REASON_LOADED || !data || !data->BaseDllName) return;
    const auto* n = data->BaseDllName;
    if (n->Length == 16 * sizeof(wchar_t) &&
        _wcsnicmp(n->Buffer, L"GameAssembly.dll", 16) == 0)
        SetEvent(g_gameAssemblyEvent);
}

static DWORD WINAPI InitThread(LPVOID)
{
    HMODULE ga = GetModuleHandleW(L"GameAssembly.dll");
    if (!ga) {
        WaitForSingleObject(g_gameAssemblyEvent, INFINITE); // blocks only this helper thread
        ga = GetModuleHandleW(L"GameAssembly.dll");
    }
    if (g_dllNotifyCookie) {
        auto unreg = (LdrUnregisterDllNotification_t)GetProcAddress(
            GetModuleHandleW(L"ntdll.dll"), "LdrUnregisterDllNotification");
        if (unreg) unreg(g_dllNotifyCookie);
        g_dllNotifyCookie = nullptr;
    }
    if (ga) InstallHooks(ga);
    return 0;
}

static void Startup()
{
    g_gameAssemblyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    auto reg = (LdrRegisterDllNotification_t)GetProcAddress(
        GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification");
    if (reg) reg(0, &OnDllNotification, nullptr, &g_dllNotifyCookie);

    HANDLE t = CreateThread(nullptr, 0, &InitThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        if (!LoadRealVersionDll()) return FALSE;
        Startup();
        break;
    case DLL_PROCESS_DETACH:
        if (reserved == nullptr) { // FreeLibrary, not process exit
            MH_DisableHook(MH_ALL_HOOKS);
            MH_Uninitialize();
            if (g_realVersion) FreeLibrary(g_realVersion);
        }
        break;
    }
    return TRUE;
}
