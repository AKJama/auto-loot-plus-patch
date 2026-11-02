// Auto Loot Plus – native part (dinput8.dll proxy)
//
// Patches the player loot magnet of Enshrouded 1.0 (the code behind "hold E"):
//   * radius: the four distance constants of the magnet point to our own floats
//   * corpses: the entity query runs a second time for looted-enemy corpses
//     (component DisableColliderModeChanges) and targets without PickupItem jump
//     straight to the game's own "loot all" event (LootInteractionEvent)
//   * the interaction of such a target is only accepted when it carries AreaLootOverride (set by
//     mod.lua on every target the settings allow). The entity query is coarse: it also returns other
//     entities near the wanted ones (bushes, heaps ...), which must not be harvested.
//   * autoLoot: the "is hold-E pressed" gate is skipped
// autoLoot, radius, instantLoot and line of sight can be switched while the game runs.
// Every site is located by byte pattern. If any pattern is missing or ambiguous,
// nothing is patched (all-or-nothing) and the reason is written to the log.

#include <windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cwchar>
#include <algorithm>
#include <initializer_list>
#include <string>
#include <vector>

// Mod version: log line and the title of the F8 menu (overlay.cpp). Keep in sync with mod.lua/mod.json/package.py.
extern const char* const kModVersion = "1.1.8";

// ---------------------------------------------------------------------------
// dinput8 proxy
// ---------------------------------------------------------------------------
// Other mods use the file name dinput8.dll as well (XHL Vein Mining, Embervale Atlas Map ...), and
// only one file can carry it. dinput8-chain.ini in the game folder (the same file XHL's loader
// reads) lists the renamed DLLs of the other mods; LoadChain() loads them right after this one.
// The first of them that exports DirectInput8Create receives the game's DirectInput calls, the
// system's dinput8.dll otherwise.

static HMODULE g_real = nullptr;   // target of the forwarded exports
static HMODULE g_self = nullptr;

static HMODULE RealDinput8()
{
    if (!g_real) {
        wchar_t path[MAX_PATH];
        GetSystemDirectoryW(path, MAX_PATH);
        wcscat_s(path, L"\\dinput8.dll");
        g_real = LoadLibraryW(path);
    }
    return g_real;
}

template <typename T>
static T RealProc(const char* name)
{
    HMODULE m = RealDinput8();
    return m ? reinterpret_cast<T>(GetProcAddress(m, name)) : nullptr;
}

extern "C" HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE inst, DWORD version, REFIID riid, LPVOID* out, LPUNKNOWN outer)
{
    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    auto fn = RealProc<Fn>("DirectInput8Create");
    return fn ? fn(inst, version, riid, out, outer) : E_FAIL;
}
extern "C" HRESULT WINAPI Proxy_DllCanUnloadNow() { return S_FALSE; }
extern "C" HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out)
{
    using Fn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
    auto fn = RealProc<Fn>("DllGetClassObject");
    return fn ? fn(clsid, riid, out) : CLASS_E_CLASSNOTAVAILABLE;
}
extern "C" HRESULT WINAPI Proxy_DllRegisterServer() { return E_NOTIMPL; }
extern "C" HRESULT WINAPI Proxy_DllUnregisterServer() { return E_NOTIMPL; }
extern "C" LPCVOID WINAPI Proxy_GetdfDIJoystick()
{
    using Fn = LPCVOID(WINAPI*)();
    auto fn = RealProc<Fn>("GetdfDIJoystick");
    return fn ? fn() : nullptr;
}

// ---------------------------------------------------------------------------
// Logging and config
// ---------------------------------------------------------------------------

static std::wstring g_gameDir;

// Time spent inside our hooks (QueryPerformanceCounter), logged every 5 s with debugLog: shows whether a
// frame-rate drop comes from our work, the game's query for our radius, or something else entirely.
struct HookTime { volatile LONG64 ticks; volatile LONG64 calls; };
static LONG64 Qpc() { LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart; }
struct Timed {
    HookTime& h; LONG64 t0;
    explicit Timed(HookTime& x) : h(x), t0(Qpc()) {}
    ~Timed() { InterlockedAdd64(&h.ticks, Qpc() - t0); InterlockedIncrement64(&h.calls); }
};
static double TakeMs(HookTime& h, LONG64* calls)
{
    static LONG64 freq = 0;
    if (!freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart; }
    LONG64 t = InterlockedExchange64(&h.ticks, 0);
    *calls = InterlockedExchange64(&h.calls, 0);
    return freq ? t * 1000.0 / (double)freq : 0.0;
}
static HookTime g_tLog = {};

void Log(const char* fmt, ...)
{
    Timed timed(g_tLog);
    std::wstring path = g_gameDir + L"\\auto-loot-plus.log";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"a") != 0 || !f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fputc('\n', f);
    fclose(f);
}

struct Config {
    bool enabled = true;
    float radius = 15.0f;
    bool autoLoot = true;
    bool corpses = true;
    bool debugLog = false;
    bool ignoreLineOfSight = true;
    bool instantLoot = true;
    std::string ignoreItems;   // parts of internal item names, comma separated ("=" prefix = exact name)
    bool ignoreNotice = true;   // short in-game notice when an ignored item is left behind
    // group switches (live since stage 2: the DLL decides per target, mod.lua releases every group)
    bool groundLoot = true, stones = true, collectibles = true, plants = true, farmPlants = true, chests = true;
    bool fish = false;   // fish in the water are pickups: own switch (off until tested at a lake, 20.10.2026)
    bool bosses = true;   // corpses of bosses (keen::ecs::Boss, also mini bosses with a health bar); false = loot them by hand
    float lootDelay = 1.0f;   // seconds a fresh corpse/chest/plant waits before it is accepted (special loot race, see TargetPending)
    bool keepQuestItems = true;   // items from quest_items.txt stay in chests and corpses; false = taken too
    // radius per group in meters, 0 = the general radius
    float radiusGround = 0, radiusCollectibles = 0, radiusPlants = 0, radiusFarm = 0, radiusCorpses = 0, radiusChests = 0;
    bool pauseNearEnemies = false;   // no automatic looting while an enemy is closer than enemyRadius
    float enemyRadius = 10.0f;
    // live through the data patches below (mod.lua sets the same at load)
    bool autoSmash = true, smashShared = true;   // smashShared = smashBuildableAwayFromBase
    int stackSize = 9999;                        // 0 = game default
    bool liveData = true;                        // false = no memory scan/patch (smash and stack size then need a restart)
    bool liveWrite = true;                       // liveData=scan: search only, never write (diagnosis)
};
static Config g_cfg;

static std::string Trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// File names from the [Chain] section of dinput8-chain.ini, one per line; ';' and '#' start a
// comment, other sections ([Options] of XHL) are skipped. A file without any section header is
// read as a plain list.
static std::vector<std::string> ParseChainIni(const std::string& text)
{
    std::vector<std::string> names;
    bool inChain = false, anySection = false;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = Trim(text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
        pos = nl == std::string::npos ? text.size() : nl + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            anySection = true;
            inChain = _stricmp(line.c_str(), "[chain]") == 0;
            continue;
        }
        if (inChain || !anySection) names.push_back(line);
    }
    return names;
}

static std::wstring Widen(const std::string& s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

// Loads the DLLs listed in <game folder>\dinput8-chain.ini. Called from DllMain like the loader
// itself would load them as imports; this file and dinput8.dll itself are skipped.
static void LoadChain()
{
    std::wstring path = g_gameDir + L"\\dinput8-chain.ini";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return;
    std::string text;
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    fclose(f);
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(g_self, self, MAX_PATH);
    for (const std::string& name : ParseChainIni(text)) {
        std::wstring w = Widen(name);
        bool relative = w.find_first_of(L"\\/:") == std::wstring::npos;
        std::wstring full = relative ? g_gameDir + L"\\" + w : w;
        if (_wcsicmp(w.c_str(), L"dinput8.dll") == 0 || _wcsicmp(full.c_str(), self) == 0) {
            Log("Kette: %s uebersprungen (diese Datei selbst)", name.c_str());
            continue;
        }
        HMODULE m = LoadLibraryW(full.c_str());
        if (!m) { Log("Kette: %s nicht geladen (Fehler %lu)", name.c_str(), GetLastError()); continue; }
        bool forwards = !g_real && GetProcAddress(m, "DirectInput8Create") != nullptr;
        if (forwards) g_real = m;
        Log("Kette: %s geladen%s", name.c_str(), forwards ? " - erhaelt die DirectInput-Aufrufe" : "");
    }
}

static bool ParseBool(const std::string& v, bool fallback)
{
    std::string s;
    for (char c : v) s += (char)tolower((unsigned char)c);
    if (s == "true" || s == "1" || s == "yes" || s == "on") return true;
    if (s == "false" || s == "0" || s == "no" || s == "off") return false;
    return fallback;
}

static float ClampRadius(const std::string& val, float lo, float hi)
{
    float r = (float)atof(val.c_str());
    return r < lo ? lo : (r > hi ? hi : r);
}

// One config key into g_cfg (config.txt at start, the menu at run time). Returns false for unknown keys.
static bool ApplyConfigKey(const std::string& key, const std::string& val)
{
    Config& c = g_cfg;
    if (key == "enabled") c.enabled = ParseBool(val, c.enabled);
    else if (key == "radiusMeters") c.radius = ClampRadius(val, 1.0f, 50.0f);
    else if (key == "autoLoot") c.autoLoot = ParseBool(val, c.autoLoot);
    else if (key == "lootEnemyCorpses") c.corpses = ParseBool(val, c.corpses);
    else if (key == "debugLog") c.debugLog = ParseBool(val, c.debugLog);
    else if (key == "requireLineOfSight") c.ignoreLineOfSight = !ParseBool(val, !c.ignoreLineOfSight);
    else if (key == "instantLoot") c.instantLoot = ParseBool(val, c.instantLoot);
    else if (key == "ignoreItems") c.ignoreItems = val;
    else if (key == "collectGroundLoot") c.groundLoot = ParseBool(val, c.groundLoot);
    else if (key == "collectStones") c.stones = ParseBool(val, c.stones);
    else if (key == "collectCollectibles") c.collectibles = ParseBool(val, c.collectibles);
    else if (key == "collectPlants") c.plants = ParseBool(val, c.plants);
    else if (key == "collectFarmPlants") c.farmPlants = ParseBool(val, c.farmPlants);
    else if (key == "collectFish") c.fish = ParseBool(val, c.fish);
    else if (key == "lootChests") c.chests = ParseBool(val, c.chests);
    else if (key == "lootBosses") c.bosses = ParseBool(val, c.bosses);
    else if (key == "lootDelay") c.lootDelay = ClampRadius(val, 0.0f, 5.0f);
    else if (key == "keepQuestItems") c.keepQuestItems = ParseBool(val, c.keepQuestItems);
    else if (key == "ignoreNotice") c.ignoreNotice = ParseBool(val, c.ignoreNotice);
    else if (key == "radiusGround") c.radiusGround = ClampRadius(val, 0.0f, 50.0f);
    else if (key == "radiusCollectibles") c.radiusCollectibles = ClampRadius(val, 0.0f, 50.0f);
    else if (key == "radiusPlants") c.radiusPlants = ClampRadius(val, 0.0f, 50.0f);
    else if (key == "radiusFarm") c.radiusFarm = ClampRadius(val, 0.0f, 50.0f);
    else if (key == "radiusCorpses") c.radiusCorpses = ClampRadius(val, 0.0f, 50.0f);
    else if (key == "radiusChests") c.radiusChests = ClampRadius(val, 0.0f, 50.0f);
    else if (key == "pauseNearEnemies") c.pauseNearEnemies = ParseBool(val, c.pauseNearEnemies);
    else if (key == "enemyRadius") c.enemyRadius = ClampRadius(val, 1.0f, 50.0f);
    else if (key == "autoSmash") c.autoSmash = ParseBool(val, c.autoSmash);
    else if (key == "smashBuildableAwayFromBase") c.smashShared = ParseBool(val, c.smashShared);
    else if (key == "stackSize") c.stackSize = (int)ClampRadius(val, 0.0f, 65535.0f);
    else if (key == "liveData") {
        c.liveWrite = !(val == "scan" || val == "Scan");
        c.liveData = !c.liveWrite || ParseBool(val, c.liveData);
    }
    else return false;
    return true;
}

// config.txt text: "key=value" lines of any length (up to 1.1.4 a 512-byte line buffer cut long ignoreItems
// lists after a restart). '#' or ';' start a comment, also at the end of a value - except in ignoreItems,
// whose "#Group" entries start with '#'.
static void ParseConfigText(std::string text)
{
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string s = Trim(text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
        pos = nl == std::string::npos ? text.size() : nl + 1;
        if (s.empty() || s[0] == '#' || s[0] == ';') continue;
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Trim(s.substr(0, eq));
        std::string val = Trim(s.substr(eq + 1));
        size_t comment = key == "ignoreItems" ? std::string::npos : val.find_first_of("#;");
        if (comment != std::string::npos) val = Trim(val.substr(0, comment));
        ApplyConfigKey(key, val);
    }
}

static void LoadConfig()
{
    std::wstring path = g_gameDir + L"\\mods\\auto-loot-plus\\config.txt";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) {
        Log("config.txt nicht gefunden – Standardwerte");
        return;
    }
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    fclose(f);
    ParseConfigText(text);
}

// ---------------------------------------------------------------------------
// Pattern scanning
// ---------------------------------------------------------------------------

struct Pattern {
    std::vector<uint8_t> bytes;
    std::vector<bool> mask;
};

static Pattern ParsePattern(const char* text)
{
    Pattern p;
    for (const char* c = text; *c;) {
        if (*c == ' ') { ++c; continue; }
        if (*c == '?') {
            p.bytes.push_back(0);
            p.mask.push_back(false);
            c += (c[1] == '?') ? 2 : 1;
        } else {
            p.bytes.push_back((uint8_t)strtoul(std::string(c, 2).c_str(), nullptr, 16));
            p.mask.push_back(true);
            c += 2;
        }
    }
    return p;
}

static uint8_t* g_textStart = nullptr;
static size_t g_textSize = 0;

static bool FindText()
{
    auto base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (memcmp(sec->Name, ".text", 5) == 0) {
            g_textStart = base + sec->VirtualAddress;
            g_textSize = sec->Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

// Returns the unique match in [start, start+size), or nullptr (count of matches in *count)
static uint8_t* FindUniqueIn(uint8_t* start, size_t size, const char* text, int* count)
{
    Pattern p = ParsePattern(text);
    uint8_t* found = nullptr;
    *count = 0;
    const size_t n = p.bytes.size();
    for (size_t i = 0; i + n <= size; ++i) {
        uint8_t* at = start + i;
        if (at[0] != p.bytes[0]) continue;
        bool ok = true;
        for (size_t k = 1; k < n; ++k) {
            if (p.mask[k] && at[k] != p.bytes[k]) { ok = false; break; }
        }
        if (ok) {
            if (++*count == 1) found = at;
        }
    }
    return *count == 1 ? found : nullptr;
}

static uint8_t* FindUnique(const char* text, int* count)
{
    return FindUniqueIn(g_textStart, g_textSize, text, count);
}

// First match (for patterns that legitimately occur more than once)
static uint8_t* FindFirst(const char* text, int* count)
{
    Pattern p = ParsePattern(text);
    *count = 0;
    const size_t n = p.bytes.size();
    for (size_t i = 0; i + n <= g_textSize; ++i) {
        bool ok = true;
        for (size_t k = 0; k < n; ++k)
            if (p.mask[k] && g_textStart[i + k] != p.bytes[k]) { ok = false; break; }
        if (ok) { *count = 1; return g_textStart + i; }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Patch helpers
// ---------------------------------------------------------------------------

static void WriteCode(void* at, const void* data, size_t size)
{
    DWORD old;
    VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &old);
    memcpy(at, data, size);
    VirtualProtect(at, size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, size);
}

static int32_t Rel32(const uint8_t* from, const void* to)
{
    return (int32_t)(reinterpret_cast<const uint8_t*>(to) - from);
}

static bool InRel32(const uint8_t* from, const void* to)
{
    int64_t d = reinterpret_cast<const uint8_t*>(to) - from;
    return d > INT32_MIN && d < INT32_MAX;
}

// Memory within +-2 GB of the game code for floats and the jump trampoline
static uint8_t* AllocNear(uint8_t* target, size_t size)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity;
    uintptr_t start = (reinterpret_cast<uintptr_t>(target) & ~(gran - 1));
    for (uintptr_t delta = gran; delta < 0x70000000; delta += gran) {
        for (int dir = -1; dir <= 1; dir += 2) {
            uintptr_t addr = dir < 0 ? start - delta : start + delta;
            void* p = VirtualAlloc(reinterpret_cast<void*>(addr), size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p) return static_cast<uint8_t*>(p);
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Game knowledge (Enshrouded 1.0, build 1269579)
// ---------------------------------------------------------------------------

static const uint32_t kHashPickupItem = 0x08775A90;            // keen::ecs::PickupItem (qualifiedHash in .cache\types.json)
static const uint32_t kHashLootedCorpse = 0x761CCA43;          // keen::ecs::DisableColliderModeChanges
static const uint32_t kHashEnemySettings = 0x67FE5508;         // keen::ecs::EnemySettings (every enemy)

// Magnet function: box half-extent load "movss xmm5,[rip+X]" (7.0)
static const char* kSigBox =
    "88 54 24 10 48 89 4C 24 08 55 53 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B 39 48 8B D9 48 8B CF "
    "48 89 7C 24 30 E8 ?? ?? ?? ?? 84 C0 0F 85 ?? ?? ?? ?? 48 89 B4 24 ?? ?? ?? ?? 48 8D 54 24 48 48 8B CF "
    "44 0F 29 94 24 ?? ?? ?? ?? E8 ?? ?? ?? ?? F3 0F 10 2D";
static const size_t kSigBoxDisp = 0x58;

// Pickup radius (1.75 m) and skill radius (3.5 m): movss xmm12 / xmm13,[rip+X]
static const char* kSigRadii =
    "44 0F 29 9C 24 ?? ?? ?? ?? 45 0F 57 DB 44 0F 29 A4 24 ?? ?? ?? ?? "
    "F3 44 0F 10 25 ?? ?? ?? ?? 44 0F 29 AC 24 ?? ?? ?? ?? F3 44 0F 10 2D ?? ?? ?? ?? 44 0F 29 B4 24 ?? ?? ?? ?? "
    "F3 44 0F 10 35 ?? ?? ?? ?? 44 0F 29 BC 24";
static const size_t kSigRadiiA = 0x1B;
static const size_t kSigRadiiB = 0x2D;
static const size_t kSigRadiiNear = 0x3F;   // xmm14 = 0.5625 (0.75 m squared): closer = loot at once, else pull

// Max squared distance (12.25): comiss xmm7,[rip+X]
static const char* kSigMaxDist =
    "F3 41 0F 59 FA F3 41 0F 59 C2 F3 0F 59 FF F3 0F 59 C0 F3 0F 58 F9 F3 0F 58 F8 0F 2F 3D ?? ?? ?? ?? 0F 87";
static const size_t kSigMaxDistDisp = 0x1D;

// Entity query: "mov eax,[PickupItem hash]; ... call Query"
static const char* kSigQuery =
    "48 89 85 ?? ?? ?? ?? 8B 05 ?? ?? ?? ?? 89 44 24 20 48 89 B5 ?? ?? ?? ?? 48 C7 85 ?? ?? ?? ?? 20 00 00 00 E8";
static const size_t kSigQueryCall = 0x23;

// First check of the magnet for every entity: "mov r8,[rbx+X]; lea rcx,[rbp+..]; mov rdx,rax;
// call GetComponent; mov rax,[rax]; mov [rsp+38h],rax; test rax,rax; jne; ...; cmp byte [rax],0;
// jne <skip>" = AreaLootOverride (exclude = true skips the entity). X is the context offset of its
// accessor; OnLootAllTarget reads the same component.
static const char* kSigAreaLoot =
    "4C 8B 83 ?? ?? ?? ?? 48 8D 8D ?? ?? ?? ?? 48 8B D0 E8 ?? ?? ?? ?? 48 8B 00 48 89 44 24 38 48 85 C0 "
    "75 07 48 89 44 24 38 EB ?? 80 38 00 0F 85";
static size_t g_ctxAreaLoot = 0;   // 0 = not found: accepts are not restricted (as in 1.0.0)

// "no PickupItem -> skip": je rel32 that we retarget to the loot-all event block
static const char* kSigNoPickup =
    "48 8B 08 48 85 C9 0F 84 ?? ?? ?? ?? 48 8D 95 ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? "
    "44 0F 2F F7 0F 86 ?? ?? ?? ?? 48 8B 83";
static const size_t kSigNoPickupJe = 0x06;
static const size_t kSigNoPickupCheck = 0x13;   // call Check(PickupItem, ctx) – item filter (pickup path)
static const size_t kSigNoPickupLootAll = 0x2A;

// Line of sight: "mov rdx,[rbx+458h]; mov rcx,[rsp+50h]; call HasAreaLoot; test al,al; jne <skip raycast>"
// Turning the jne into jmp skips the visibility raycast for every target.
// (preceded by "ja" of the max-distance check; the same sequence after a "jbe" must stay untouched)
static const char* kSigSight = "0F 87 ?? ?? ?? ?? 48 8B 93 58 04 00 00 48 8B 4C 24 50 E8 ?? ?? ?? ?? 84 C0 0F 85";
static const size_t kSigSightJne = 0x19;

// System area_loot (pulls marked items to the player): drops them again beyond 1.75 / 3.5 m
static const char* kSigPull =
    "66 41 85 84 24 B0 05 00 00 74 ?? B2 08 49 8B CF E8 ?? ?? ?? ?? 48 8B 55 ?? 84 C0 75 ?? "
    "F3 44 0F 10 05 ?? ?? ?? ?? EB ?? F3 44 0F 10 05 ?? ?? ?? ?? F3 0F 10 05";
static const size_t kSigPullA = 0x22;
static const size_t kSigPullB = 0x2D;

// Hold-E gate: "test byte [rdx+34h],80h; jne +3; xor al,al; ret"
static const char* kSigGate = "F6 42 34 80 75 03 32 C0 C3 B8 00 10 00 00 66 85";
static const size_t kSigGateJne = 0x04;

// Context offsets of player_control_action_sequence (component accessors / event outputs)
static const size_t kCtxOfferRuntime = 0x488;   // InteractionOfferRuntime (random-access read)
static const size_t kCtxAcceptEvent = 0x530;    // AcceptOfferedInteractionEvent output
static const size_t kOfferIdOffset = 40;        // InteractionOfferRuntime.offerId
static const size_t kOfferDisabledOffset = 46;  // InteractionOfferRuntime.isDisabled

// Header site in the vanilla AcceptOffered emitter: "lea rdx,[rbp+67h]; mov rcx,r12; call EventHeader"
static const char* kSigHeader = "48 8D 55 67 49 8B CC E8 ?? ?? ?? ?? 48 8D 55 67 48 8B 08 48 89 0B 49 8B CC E8";
// Vanilla helper allocating an AcceptOfferedInteractionEvent from an output accessor
static const char* kSigAllocAccept =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B F9 E8 ?? ?? ?? ?? 48 8B CF 0F B7 D8 E8 ?? ?? ?? ?? 44 0F B7 CB BA B2 04 E4 82";

using GetComponentFn = void**(__fastcall*)(void* out, void* entity, void* accessor);
using GetIdFn = uint32_t*(__fastcall*)(void* world, void* tmp);
using GetHeaderFn = uint64_t*(__fastcall*)(void* world, void* tmp);
using AllocEventFn = uint8_t*(__fastcall*)(void* accessor);
static GetComponentFn g_getComponent = nullptr;
static GetIdFn g_getGuest = nullptr;
static GetHeaderFn g_getHeader = nullptr;
static AllocEventFn g_allocAccept = nullptr;

static volatile LONG64 g_statLootAll = 0, g_statAccepts = 0;

// "Interaction accepted" once per target (corpse/bush/chest) and visit. Up to 1.0.2 a target was
// accepted up to 3 times per 20 s (every 1.5 s) so that a refused attempt got new chances. But many
// targets stay in range after the accept (corpses, chests that keep a quest item, plants that are
// still being harvested), and every further accept did harm: a plant was harvested once more per
// accept (3 fibers instead of 1), and while a crafting or chest window was open the window was bound
// to the newly accepted target (window closed "twice in a row, as on a timer", or showed the plant's
// empty single slot).
// Since 21.10.2026 (special loot race, diagnosis and fix by faytleingod84): ectoplasm in the Hollow
// Halls, jewels and legendary runes of elites, boss and quest items are generated by the game only
// when the interaction is accepted. Up to 1.0.3 the accept and the vanilla "loot all" ran in the same
// magnet pass, so the corpse was emptied (and vanished) before that loot existed. Now a fresh target
// (1) waits lootDelay seconds after it was first seen (a corpse accepted in the moment of death has
// no loot yet), (2) is accepted, with loot-all blocked in that pass, and (3) is looted in a later
// pass once the game has processed the accept (new offer id). While the offer id stays the same
// nothing happened (refused): the accept is repeated after kRetryMs up to kMaxAccepts times, then
// loot-all runs anyway (as in 1.0.3; a target that needs no accept). Once the game reacted the
// target is done for this visit: no further accept. The window guard in GateHook keeps all of this
// away from open game windows. A new visit starts when the target was out of range for kReArmMs;
// the magnet reports every target in range each frame, so lastSeen stays fresh.
struct SeenTarget { uint32_t host; uint32_t offerId; uint32_t accepts; uint64_t lastSeen; uint64_t acceptTick; uint64_t firstSeen; uint32_t loots; uint64_t lastLoot; };
static const size_t kSeenTargets = 1024;
static SeenTarget g_seen[kSeenTargets];
static SRWLOCK g_seenLock = SRWLOCK_INIT;
static const uint64_t kReArmMs = 10000;
static const uint64_t kRetryMs = 3000;          // nothing happened: try again
static const uint32_t kMaxAccepts = 3;
static const uint64_t kPickupLootGapMs = 1500;   // pickup path: one loot-all per pickup and gap (it lies there a few frames)
static uint64_t g_clock = 0;   // tests only: fixed clock instead of GetTickCount64

static uint64_t Now() { return g_clock ? g_clock : GetTickCount64(); }

static HookTime g_tQueryPickups = {}, g_tQueryCorpses = {}, g_tGate = {}, g_tStub = {}, g_tLootAll = {}, g_tFilter = {};

// What one magnet pass does with a released target: wait (nothing), send the accept (no loot-all in
// this pass) or let the game's loot-all run (no accept).
enum class TargetAction : uint8_t { Wait, Accept, Loot };

// Called for every released target the magnet reports (each frame). offerId = its current offer,
// nullptr = no usable offer right now.
static TargetAction TargetPending(uint32_t host, const uint32_t* offerId)
{
    uint64_t now = Now();
    uint64_t delay = (uint64_t)(g_cfg.lootDelay * 1000.0f);
    AcquireSRWLockExclusive(&g_seenLock);
    SeenTarget* s = nullptr;
    SeenTarget* oldest = &g_seen[0];
    for (auto& t : g_seen) {
        if (t.host == host && t.lastSeen != 0) { s = &t; break; }
        if (t.lastSeen < oldest->lastSeen) oldest = &t;
    }
    if (!s) { s = oldest; *s = { host, 0, 0, 0, 0, 0 }; }   // table full: the longest unseen is forgotten
    if (now - s->lastSeen > kReArmMs) { s->accepts = 0; s->offerId = 0; s->acceptTick = 0; s->firstSeen = 0; s->loots = 0; s->lastLoot = 0; }   // new visit (also a new entry)
    s->lastSeen = now;
    if (s->firstSeen == 0) s->firstSeen = now;
    TargetAction action = TargetAction::Wait;
    if (now - s->firstSeen < delay) action = TargetAction::Wait;                 // fresh: the game is still building its loot
    else if (!offerId) action = TargetAction::Loot;                               // nothing to accept: the old direct path
    else if (s->accepts == 0) action = TargetAction::Accept;
    else if (*offerId != s->offerId) action = TargetAction::Loot;                 // the game reacted: done
    else if (now - s->acceptTick > kRetryMs)
        action = s->accepts < kMaxAccepts ? TargetAction::Accept : TargetAction::Loot;   // refused: again, finally loot anyway
    ReleaseSRWLockExclusive(&g_seenLock);
    return action;
}

// Pickup path: true for the first sighting of a visit (accepts == 0), as in 1.0.3; no delay.
static bool FirstAcceptDue(uint32_t host)
{
    uint64_t now = Now();
    AcquireSRWLockExclusive(&g_seenLock);
    SeenTarget* s = nullptr;
    SeenTarget* oldest = &g_seen[0];
    for (auto& t : g_seen) {
        if (t.host == host && t.lastSeen != 0) { s = &t; break; }
        if (t.lastSeen < oldest->lastSeen) oldest = &t;
    }
    if (!s) { s = oldest; *s = { host, 0, 0, 0, 0, 0 }; }
    if (now - s->lastSeen > kReArmMs) { s->accepts = 0; s->offerId = 0; s->acceptTick = 0; s->firstSeen = 0; s->loots = 0; s->lastLoot = 0; }
    s->lastSeen = now;
    if (s->firstSeen == 0) s->firstSeen = now;
    bool due = s->accepts == 0;
    ReleaseSRWLockExclusive(&g_seenLock);
    return due;
}

// Plants and beds after the game reacted to our accept (offer id changed, or the offer is gone or
// disabled): true for the first loot-all of this visit only - that one harvests. Every further
// loot-all while the plant still stands harvests again. Not after a refused accept (fallback loots).
static bool HarvestLootDue(uint32_t host, const uint32_t* offerId, bool* reacted)
{
    bool due = false;
    *reacted = false;
    AcquireSRWLockExclusive(&g_seenLock);
    for (auto& t : g_seen)
        if (t.host == host && t.lastSeen != 0) {
            *reacted = t.accepts > 0 && (!offerId || *offerId != t.offerId);
            if (*reacted && t.loots == 0) { t.loots = 1; due = true; }
            break;
        }
    ReleaseSRWLockExclusive(&g_seenLock);
    return due;
}

// Pickup path: the game's loot-all may run once per kPickupLootGapMs for the same pickup. It still
// lies there for a few frames after it was taken, and every loot-all meanwhile took it again
// (3x mushrooms, stones, stone piles; 21.10.2026). Still there after the gap = declined: again.
static bool PickupLootDue(uint32_t host)
{
    uint64_t now = Now();
    AcquireSRWLockExclusive(&g_seenLock);
    SeenTarget* s = nullptr;
    SeenTarget* oldest = &g_seen[0];
    for (auto& t : g_seen) {
        if (t.host == host && t.lastSeen != 0) { s = &t; break; }
        if (t.lastSeen < oldest->lastSeen) oldest = &t;
    }
    if (!s) { s = oldest; *s = { host, 0, 0, 0, 0, 0 }; }
    if (now - s->lastSeen > kReArmMs) { s->accepts = 0; s->offerId = 0; s->acceptTick = 0; s->firstSeen = 0; s->loots = 0; s->lastLoot = 0; }
    s->lastSeen = now;
    if (s->firstSeen == 0) s->firstSeen = now;
    bool due = s->lastLoot == 0 || now - s->lastLoot >= kPickupLootGapMs;
    if (due) s->lastLoot = now;
    ReleaseSRWLockExclusive(&g_seenLock);
    return due;
}

// Called right after the accept event for this target was sent.
static void TargetAccepted(uint32_t host, uint32_t offerId)
{
    AcquireSRWLockExclusive(&g_seenLock);
    for (auto& t : g_seen) {
        if (t.host == host && t.lastSeen != 0) { t.offerId = offerId; ++t.accepts; t.acceptTick = Now(); break; }
    }
    ReleaseSRWLockExclusive(&g_seenLock);
}

// Quest items stay in chests when we loot automatically (list: mods\auto-loot-plus\quest_items.txt)
static std::vector<uint32_t> g_questItems;
static volatile LONG64 g_statQuestKept = 0;

static void LoadQuestItems()
{
    std::wstring path = g_gameDir + L"\\mods\\auto-loot-plus\\quest_items.txt";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r") != 0 || !f) { Log("quest_items.txt fehlt"); return; }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] < '0' || line[0] > '9') continue;
        g_questItems.push_back((uint32_t)strtoul(line, nullptr, 10));
    }
    fclose(f);
    std::sort(g_questItems.begin(), g_questItems.end());
    Log("Questgegenstaende: %zu", g_questItems.size());
}

// Diagnostics (debugLog): which items our automatic "loot all" moves, by item id
struct ItemCount { uint32_t id; uint32_t count; };
static ItemCount g_lootedItems[64];
static SRWLOCK g_lootedLock = SRWLOCK_INIT;

static void CountLootedItem(uint32_t id)
{
    AcquireSRWLockExclusive(&g_lootedLock);
    for (auto& c : g_lootedItems) {
        if (c.id == id) { ++c.count; break; }
        if (c.count == 0) { c = { id, 1 }; break; }
    }
    ReleaseSRWLockExclusive(&g_lootedLock);
}

// Item filter (config ignoreItems): item names come from mods\auto-loot-plus\items.txt
// ("id<TAB>internal name<TAB>English name<TAB>German name" per line, generated from the game data).
// A pattern matches as a case-insensitive substring of the internal, English or German name; with a
// leading "=" only the whole name. Matching items stay on the ground (pickup path, see
// BuildPickupStub) and in chests, corpses and bushes (slot loop, KeepInContainer).
struct ItemName {
    uint32_t id; std::string name, en, de; std::string lname, len, lde; bool lootable = true; std::string category;   // l* = lower case
    uint8_t guid[16] = {};   // objectId of the keen::ItemInfo record (column 8)
    uint16_t stack = 0;      // original maxStackSize (column 9), 0 = unknown
};
static std::vector<ItemName> g_itemNames;
static bool ParseGuid(const std::string& text, uint8_t out[16]);
static std::vector<uint32_t> g_ignoredItems;   // sorted
static std::vector<std::string> g_unknownIgnoreEntries;   // exact entries only; retained in config
static SRWLOCK g_ignoreLock = SRWLOCK_INIT;
static volatile LONG64 g_statFiltered = 0, g_statPickupFiltered = 0;

// Lower case for matching: ASCII letters and the Latin-1 capitals in UTF-8 (Ä Ö Ü É ... = C3 80..C3 9E
// except the multiplication sign C3 97 become C3 A0..C3 BE). Independent of the C locale on purpose:
// the game may change it at run time, and the item names are lowered once at start.
static std::string Lower(const std::string& s)
{
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'A' && c <= 'Z') r += (char)(c + 32);
        else if (c == 0xC3 && i + 1 < s.size()) {
            unsigned char d = (unsigned char)s[i + 1];
            r += (char)c;
            r += (char)((d >= 0x80 && d <= 0x9E && d != 0x97) ? d + 0x20 : d);
            ++i;
        } else r += (char)c;
    }
    return r;
}

static void LoadItemNames()
{
    std::wstring path = g_gameDir + L"\\mods\\auto-loot-plus\\items.txt";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r") != 0 || !f) { Log("items.txt fehlt – Gegenstandsfilter aus"); return; }
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        if (line[0] < '0' || line[0] > '9') continue;
        std::string s = Trim(line);
        std::string cols[9];
        size_t start = 0;
        for (int i = 0; i < 9 && start <= s.size(); ++i) {
            size_t tab = s.find('\t', start);
            cols[i] = s.substr(start, tab == std::string::npos ? std::string::npos : tab - start);
            start = tab == std::string::npos ? s.size() + 1 : tab + 1;
        }
        ItemName it{ (uint32_t)strtoul(cols[0].c_str(), nullptr, 10), Trim(cols[1]), Trim(cols[2]), Trim(cols[3]) };
        it.lname = Lower(it.name); it.len = Lower(it.en); it.lde = Lower(it.de);
        it.lootable = Trim(cols[4]) != "0";   // column 5: 1 = can be looted (missing = yes)
        // column 7: group in the menu (Materials, Terrain, Keys, Food ...); older files: column 6 = game category
        it.category = Trim(cols[6]).empty() ? Trim(cols[5]) : Trim(cols[6]);
        // columns 8 and 9 (1.1.0): record GUID and original stack size for the live stack size
        if (!ParseGuid(Trim(cols[7]), it.guid)) memset(it.guid, 0, 16);
        it.stack = (uint16_t)strtoul(cols[8].c_str(), nullptr, 10);
        g_itemNames.push_back(it);
    }
    fclose(f);
}

// Sort key for display names: lower case, umlauts as their base letter (ä a, ö o, ü u, ß ss), so
// "Äste" sorts under A and not behind Z.
static std::string SortKey(const char* name)
{
    std::string l = Lower(name), r;
    for (size_t i = 0; i < l.size(); ++i) {
        unsigned char c = (unsigned char)l[i];
        if (c == 0xC3 && i + 1 < l.size()) {
            unsigned char d = (unsigned char)l[i + 1];
            ++i;
            if (d == 0xA4) r += 'a'; else if (d == 0xB6) r += 'o'; else if (d == 0xBC) r += 'u'; else if (d == 0x9F) r += "ss";
            else { r += (char)c; r += (char)d; }
        } else r += (char)c;
    }
    return r;
}

// Suggestions for the menu: lootable items whose internal, English or German name contains the
// query (case-insensitive); names that start with it come first. displayName = German or English
// name, or the internal name when the game has no text for it.
struct ItemSuggestion { uint32_t id; const char* internalName; const char* displayName; };

static const char* DisplayOf(const ItemName& it, bool german)
{
    const std::string& disp = german && !it.de.empty() ? it.de : (!it.en.empty() ? it.en : it.name);
    return disp.c_str();
}

// Display name of an internal item name (case-insensitive), nullptr when unknown
const char* ItemDisplayName(const char* internalName, bool german)
{
    std::string l = Lower(Trim(internalName));
    for (auto& it : g_itemNames) if (it.lname == l) return DisplayOf(it, german);
    return nullptr;
}

const char* ItemDisplayNameById(uint32_t id, bool german)
{
    for (auto& it : g_itemNames) if (it.id == id) return DisplayOf(it, german);
    return nullptr;
}

// Notice for the in-game hint "left behind: <item>": the newest filtered item and when. Shown at
// most kNoticeMax times per item and session, so the hint teaches the list without nagging.
static SRWLOCK g_noticeLock = SRWLOCK_INIT;
static uint32_t g_noticeId = 0;
static uint64_t g_noticeTick = 0;
struct NoticeCount { uint32_t id; uint32_t n; };
static std::vector<NoticeCount> g_noticeCounts;
static const uint32_t kNoticeMax = 3;

static void NoteFiltered(uint32_t id)
{
    if (!g_cfg.ignoreNotice) return;
    AcquireSRWLockExclusive(&g_noticeLock);
    NoticeCount* c = nullptr;
    for (auto& x : g_noticeCounts) if (x.id == id) { c = &x; break; }
    if (!c) { g_noticeCounts.push_back({ id, 0 }); c = &g_noticeCounts.back(); }
    if (c->n < kNoticeMax) { ++c->n; g_noticeId = id; g_noticeTick = Now(); }
    ReleaseSRWLockExclusive(&g_noticeLock);
}

// overlay.cpp: the newest notice; false when none
bool FilterNotice(uint32_t* id, uint64_t* tick)
{
    AcquireSRWLockShared(&g_noticeLock);
    *id = g_noticeId; *tick = g_noticeTick;
    ReleaseSRWLockShared(&g_noticeLock);
    return *tick != 0;
}

// category: "" = all; with a category the query may be empty (browse the whole category, sorted by
// display name).
size_t SuggestItems(const char* query, bool german, const char* category, ItemSuggestion* out, size_t max)
{
    std::string q = Lower(Trim(query));
    std::string cat = category ? Trim(category) : "";
    if ((q.empty() && cat.empty()) || !max) return 0;
    size_t n = 0;
    for (int pass = 0; pass < 2 && n < max; ++pass) {        // pass 0: starts with, pass 1: contains
        for (auto& it : g_itemNames) {
            if (n >= max) break;
            if (!it.lootable || (!cat.empty() && it.category != cat)) continue;
            const std::string& ldisp = german && !it.lde.empty() ? it.lde : (!it.len.empty() ? it.len : it.lname);
            bool starts = ldisp.compare(0, q.size(), q) == 0 || it.lname.compare(0, q.size(), q) == 0;
            bool contains = ldisp.find(q) != std::string::npos || it.lname.find(q) != std::string::npos ||
                            it.len.find(q) != std::string::npos || it.lde.find(q) != std::string::npos;
            if (pass == 0 ? !starts : (!contains || starts)) continue;
            out[n++] = { it.id, it.name.c_str(), DisplayOf(it, german) };
        }
    }
    if (q.empty())
        std::sort(out, out + n, [](const ItemSuggestion& a, const ItemSuggestion& b) { return SortKey(a.displayName) < SortKey(b.displayName); });
    return n;
}

// Keep the existing exact-match semantics, including English/German names and non-lootable items.
static bool ExactItemMatches(const ItemName& it, const std::string& name)
{
    return !name.empty() && (it.lname == name || it.len == name || it.lde == name);
}

// overlay.cpp: classify the current menu value, including edits not committed to config yet.
// Substrings and #groups cannot be declared obsolete just because they currently match nothing.
bool IsUnknownIgnoreEntry(const char* entry)
{
    std::string p = Lower(Trim(entry));
    if (p.empty() || p[0] != '=') return false;
    p = Trim(p.substr(1));
    for (const auto& it : g_itemNames) if (ExactItemMatches(it, p)) return false;
    return true;
}

static void BuildIgnoreSet(const std::string& patterns)
{
    std::vector<uint32_t> ids;
    std::vector<std::string> unknown;
    std::string first;
    size_t start = 0;
    while (start <= patterns.size()) {
        size_t comma = patterns.find(',', start);
        std::string entry = Trim(patterns.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        std::string p = Lower(entry);
        start = comma == std::string::npos ? patterns.size() + 1 : comma + 1;
        if (p.empty()) continue;
        bool exact = p[0] == '=', group = p[0] == '#';   // "#Materials" = the whole game category
        if (exact || group) p = Trim(p.substr(1));
        bool matched = false;
        if (!p.empty()) for (auto& it : g_itemNames) {
            bool hit = group ? Lower(it.category) == p
                     : exact ? ExactItemMatches(it, p)
                             : (it.lname.find(p) != std::string::npos || it.len.find(p) != std::string::npos ||
                                it.lde.find(p) != std::string::npos);
            if (hit) { matched = true; ids.push_back(it.id); if (first.empty()) first = it.name; }
        }
        if (exact && !matched) unknown.push_back(entry);
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    AcquireSRWLockExclusive(&g_ignoreLock);
    g_ignoredItems.swap(ids);
    g_unknownIgnoreEntries = unknown;
    size_t n = g_ignoredItems.size();
    ReleaseSRWLockExclusive(&g_ignoreLock);
    if (!patterns.empty() || n) Log("Gegenstandsfilter: '%s' = %zu Gegenstaende%s%s", patterns.c_str(), n, first.empty() ? "" : ", z. B. ", first.c_str());
    if (!unknown.empty()) {
        std::string sample;
        for (const auto& entry : unknown) {
            std::string next = (sample.empty() ? "" : ", ") + entry;
            if (sample.size() + next.size() > 240) { sample += "..."; break; }   // whole names, no split UTF-8
            sample += next;
        }
        Log("Gegenstandsfilter: %zu unbekannte Einträge: %s", unknown.size(), sample.c_str());
    }
}

static bool IsIgnoredItem(uint32_t id)
{
    AcquireSRWLockShared(&g_ignoreLock);
    bool hit = std::binary_search(g_ignoredItems.begin(), g_ignoredItems.end(), id);
    ReleaseSRWLockShared(&g_ignoreLock);
    return hit;
}

// ---------------------------------------------------------------------------
// Templates (stage 2): what a target is, decided at run time
// ---------------------------------------------------------------------------
// mods\auto-loot-plus\templates.txt (generated from the game data, see werkzeuge\build_templates.py):
// "guid<TAB>group<TAB>flags<TAB>name<TAB>yield item ids". An entity record of the game has at
// +0x28 a pointer to its template record, whose first 16 bytes are the template guid (uuid in
// little-endian field order). Found 19.10.2026 with the live game: LootChest_Random_gold/silver and
// 1_Player. With the template known, the DLL applies the group switches, the radius per group, the
// yield of bushes against the ignore list and the "enemy nearby" pause itself, all without restart.
enum Group : uint8_t { G_UNKNOWN = 0, G_GROUND, G_COLLECTIBLE, G_PLANT, G_FARM, G_CORPSE, G_CHEST, G_BOSS, G_ENEMY, G_FISH };
enum TemplateFlags : uint8_t { TF_STONE = 1, TF_SWARM = 2, TF_BOSS = 4, TF_ELITE = 8, TF_ANIMAL = 16 };   // animal: wild animal (enemy group, no enemy pause)
struct TemplateInfo { uint8_t guid[16]; uint8_t group; uint8_t flags; std::vector<uint32_t> yield; std::string name; };
static std::vector<TemplateInfo> g_templates;   // sorted by guid
static const size_t kEntityTemplateOffset = 0x28;
static volatile LONG64 g_statGroupOff = 0, g_statRadiusOff = 0, g_statYieldIgnored = 0, g_statEnemyNear = 0, g_statUnknownTemplate = 0;
static uint64_t g_enemyNearTick = 0;

static bool ParseGuid(const std::string& text, uint8_t out[16])
{
    // 8-4-4-4-12 hex; the first three fields are stored little-endian, the last two as written
    std::string hex;
    for (char c : text) if (c != '-' && c != '{' && c != '}') hex += c;
    if (hex.size() != 32) return false;
    uint8_t b[16];
    for (int i = 0; i < 16; ++i) {
        char two[3] = { hex[2 * i], hex[2 * i + 1], 0 };
        char* end = nullptr;
        unsigned long v = strtoul(two, &end, 16);
        if (end != two + 2) return false;
        b[i] = (uint8_t)v;
    }
    out[0] = b[3]; out[1] = b[2]; out[2] = b[1]; out[3] = b[0];
    out[4] = b[5]; out[5] = b[4];
    out[6] = b[7]; out[7] = b[6];
    memcpy(out + 8, b + 8, 8);
    return true;
}

static uint8_t GroupCode(const std::string& g)
{
    if (g == "ground") return G_GROUND;
    if (g == "collectible") return G_COLLECTIBLE;
    if (g == "plant") return G_PLANT;
    if (g == "farm") return G_FARM;
    if (g == "corpse") return G_CORPSE;
    if (g == "chest") return G_CHEST;
    if (g == "boss") return G_BOSS;
    if (g == "enemy") return G_ENEMY;
    if (g == "fish") return G_FISH;
    return G_UNKNOWN;
}

static void SortTemplates()
{
    std::sort(g_templates.begin(), g_templates.end(),
              [](const TemplateInfo& a, const TemplateInfo& b) { return memcmp(a.guid, b.guid, 16) < 0; });
}

static void LoadTemplates()
{
    std::wstring path = g_gameDir + L"\\mods\\auto-loot-plus\\templates.txt";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r") != 0 || !f) { Log("templates.txt fehlt – Gruppen nur ueber die Lua-Freigaben"); return; }
    char line[2048];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        std::string s = Trim(line), cols[5];
        size_t start = 0;
        for (int i = 0; i < 5 && start <= s.size(); ++i) {
            size_t tab = s.find('\t', start);
            cols[i] = s.substr(start, tab == std::string::npos ? std::string::npos : tab - start);
            start = tab == std::string::npos ? s.size() + 1 : tab + 1;
        }
        TemplateInfo t;
        if (!ParseGuid(Trim(cols[0]), t.guid)) continue;
        t.group = GroupCode(Trim(cols[1]));
        std::string flags = Trim(cols[2]);
        t.flags = (flags.find("stone") != std::string::npos ? TF_STONE : 0) | (flags.find("swarm") != std::string::npos ? TF_SWARM : 0) |
                  (flags.find("boss") != std::string::npos ? TF_BOSS : 0) | (flags.find("elite") != std::string::npos ? TF_ELITE : 0) |
                  (flags.find("animal") != std::string::npos ? TF_ANIMAL : 0);
        t.name = Trim(cols[3]);
        std::string y = cols[4];
        size_t p = 0;
        while (p < y.size()) {
            size_t c = y.find(',', p);
            std::string id = Trim(y.substr(p, c == std::string::npos ? std::string::npos : c - p));
            if (!id.empty()) t.yield.push_back((uint32_t)strtoul(id.c_str(), nullptr, 10));
            p = c == std::string::npos ? y.size() : c + 1;
        }
        g_templates.push_back(t);
    }
    fclose(f);
    SortTemplates();
    Log("Vorlagen: %zu", g_templates.size());
}

static const TemplateInfo* FindTemplate(const uint8_t guid[16])
{
    size_t lo = 0, hi = g_templates.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = memcmp(g_templates[mid].guid, guid, 16);
        if (c == 0) return &g_templates[mid];
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return nullptr;
}

static bool Readable(const void* p, size_t n)
{
    if (reinterpret_cast<uintptr_t>(p) < 0x10000) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    return reinterpret_cast<const uint8_t*>(p) + n <= static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
}

// Template of a game entity (nullptr when unknown or unreadable)
// Guarded read without a system call: VirtualQuery (Readable) took 2-5 ms per call in the magnet
// hooks on 20.10.2026 (stutter; address-space lock of a process with 10+ GB of mapped memory and
// many allocating threads). SEH only, no C++ objects in here.
static bool SafeRead(const void* p, void* out, size_t n)
{
    if (reinterpret_cast<uintptr_t>(p) < 0x10000) return false;
    __try { memcpy(out, p, n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static HookTime g_tTemplate = {}, g_tReleased = {};

static const TemplateInfo* TemplateOf(const void* entity)
{
    Timed timed(g_tTemplate);
    static volatile LONG s_logs = 0;
    bool log = g_cfg.debugLog && s_logs < 40;
    const void* record = nullptr;
    if (g_templates.empty() || !SafeRead(static_cast<const uint8_t*>(entity) + kEntityTemplateOffset, &record, 8)) {
        if (log) { InterlockedIncrement(&s_logs); Log("Vorlage: Entity=%p nicht lesbar", entity); }
        return nullptr;
    }
    uint8_t g[16];
    if (!SafeRead(record, g, 16)) {
        if (log) { InterlockedIncrement(&s_logs); Log("Vorlage: Entity=%p Record=%p nicht lesbar", entity, record); }
        return nullptr;
    }
    const TemplateInfo* t = FindTemplate(g);
    if (!t) {
        InterlockedIncrement64(&g_statUnknownTemplate);
        static volatile LONG s_unknownLogs = 0;
        if (g_cfg.debugLog && s_unknownLogs < 60 && InterlockedIncrement(&s_unknownLogs) <= 60)
            Log("Vorlage unbekannt: GUID=%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x Entity=%p",
                g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6], g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15], entity);
    }
    if (log) {
        InterlockedIncrement(&s_logs);
        Log("Vorlage: Entity=%p Record=%p GUID=%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x -> %s (Gruppe %u)",
            entity, record, g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6], g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15],
            t ? t->name.c_str() : "unbekannt", t ? t->group : 0);
    }
    return t;
}

static float GroupRadius(uint8_t group)
{
    float r = 0;
    switch (group) {
    case G_GROUND: r = g_cfg.radiusGround; break;
    case G_COLLECTIBLE: r = g_cfg.radiusCollectibles; break;
    case G_PLANT: r = g_cfg.radiusPlants; break;
    case G_FARM: r = g_cfg.radiusFarm; break;
    case G_CORPSE: r = g_cfg.radiusCorpses; break;
    case G_CHEST: r = g_cfg.radiusChests; break;
    default: break;
    }
    return r > 0 ? r : g_cfg.radius;
}

// The group switches, the radius per group and the ignore list against a bush's yield.
// dist2 = squared distance from the player (from the magnet), < 0 = unknown (no radius check).
// released = the target carries AreaLootOverride exclude=false. A dead enemy keeps its enemy
// template (seen 19.10.: Enemy_Wildbeast_Wolf_hook_AG2, released) – released enemy = corpse. Corpses
// of bosses (keen::ecs::Boss, or a corpse template only bosses turn into, flag boss) and of elite
// enemies (flag elite) follow the "loot bosses and elites" switch: their bonus loot is rolled when the
// corpse is opened by hand (Cyclops, jewels of elites: report faytleingod84 19.10.2026).
static bool GroupAllowed(const TemplateInfo* t, float dist2, bool released)
{
    // Unknown or unreadable template: nothing happens automatically (hazelnut bushes planted by the
    // player were "harvested" into twigs while still growing: ldxiaoxiao 20.10.2026 - their growth
    // stages are not in templates.txt). Without templates.txt the Lua releases decide, as before.
    if (!t) return g_templates.empty();
    uint8_t group = t->group;
    bool bossCorpse = false;
    if (group == G_ENEMY && released) group = G_CORPSE;
    if (group == G_CORPSE) bossCorpse = (t->flags & (TF_BOSS | TF_ELITE)) != 0;
    switch (group) {
    case G_GROUND: if (!g_cfg.groundLoot || ((t->flags & TF_STONE) && !g_cfg.stones)) { InterlockedIncrement64(&g_statGroupOff); return false; } break;
    case G_COLLECTIBLE: if (!g_cfg.collectibles) { InterlockedIncrement64(&g_statGroupOff); return false; } break;
    case G_PLANT: if (!g_cfg.plants) { InterlockedIncrement64(&g_statGroupOff); return false; } break;
    case G_FARM: if (!g_cfg.farmPlants) { InterlockedIncrement64(&g_statGroupOff); return false; } break;
    case G_CORPSE: if (!g_cfg.corpses || (bossCorpse && !g_cfg.bosses)) { InterlockedIncrement64(&g_statGroupOff); return false; } break;
    case G_CHEST: if (!g_cfg.chests) { InterlockedIncrement64(&g_statGroupOff); return false; } break;
    case G_BOSS: case G_ENEMY: InterlockedIncrement64(&g_statGroupOff); return false;
    case G_FISH: if (!g_cfg.fish) { InterlockedIncrement64(&g_statGroupOff); return false; } break;
    default: break;
    }
    if (dist2 >= 0) {
        float r = GroupRadius(group);
        if (dist2 > r * r) { InterlockedIncrement64(&g_statRadiusOff); return false; }
    }
    // Ignore list against the yield. Bushes, beds and heaps are harvested by the accept, which puts the
    // whole yield straight into the inventory, past the container filter: one ignored item = the target
    // stays (21.10.2026: twig bushes give twigs + fiber). Chests and corpses are emptied by loot-all,
    // where the slot filter keeps ignored items inside: skipped only when nothing else is in them.
    if (!t->yield.empty()) {
        bool harvested = group == G_PLANT || group == G_FARM || group == G_GROUND;
        uint32_t ignored = 0;
        bool allIgnored = true;
        for (uint32_t id : t->yield) {
            if (IsIgnoredItem(id)) { if (!ignored) ignored = id; }
            else allIgnored = false;
        }
        if (ignored && (allIgnored || harvested)) { InterlockedIncrement64(&g_statYieldIgnored); NoteFiltered(ignored); return false; }
    }
    return true;
}

// "Inventory full" hint: our loot-all keeps hitting the same item every frame when it cannot be
// moved (seen 19.10.: one weapon 134 times in 5 s). More than kFullRepeats hits of one item within
// kFullWindowMs set the notice, at most once per kFullNoticeGapMs.
static const uint32_t kFullRepeats = 100;
static const uint64_t kFullWindowMs = 5000, kFullNoticeGapMs = 60000;
static uint64_t g_fullTick = 0;
static void NoteContainerAttempt(uint32_t id)
{
    static uint32_t lastId = 0, repeats = 0;
    static uint64_t windowStart = 0, lastNotice = 0;
    uint64_t now = Now();
    if (id != lastId || now - windowStart > kFullWindowMs) { lastId = id; repeats = 0; windowStart = now; }
    if (++repeats > kFullRepeats && now - lastNotice > kFullNoticeGapMs) { lastNotice = now; g_fullTick = now; }
}
bool InventoryFullNotice(uint64_t* tick) { *tick = g_fullTick; return g_fullTick != 0; }   // overlay.cpp

// Pickups the game cannot take (inventory full, too heavy ...) are offered again every frame; the
// game runs its whole pickup attempt for each of them each time, and with a dozen of them lying
// around the frame rate collapses (20.10.2026: 18 such pickups per frame, ~3 fps, fine with auto
// loot off). A pickup that is still there kPickupStuckMs after we first allowed it is therefore
// skipped for kPickupBackoffMs (the game then passes it cheaply), and the "inventory full" notice
// is shown. Keyed by entity id; an unseen gap of kPickupGapMs starts a new episode.
struct StuckPickup { uint32_t key; uint64_t firstSeen, lastSeen, blockedUntil; };
static const size_t kStuckPickups = 256;
static StuckPickup g_stuck[kStuckPickups];
static SRWLOCK g_stuckLock = SRWLOCK_INIT;
static const uint64_t kPickupStuckMs = 3000, kPickupBackoffMs = 10000, kPickupGapMs = 1000;
static volatile LONG64 g_statPickupStuck = 0;

// A pickup the game just evaluated (our FilterPickup ran for it within kPickupRecentMs): when the
// game then declines it (inventory full, too heavy, our pause ...) it continues into the branch for
// targets WITHOUT PickupItem – the gate stub. Vanilla stones and the like are released by the
// game's own data, so until 20.10.2026 every declined pickup in range got "accept + loot all" each
// frame (180 loot-all events per second, ~3 fps with a full inventory). The gate skips them now;
// mushrooms, stone piles and swarms (loot-all by design) never pass the pickup check, so they are
// never seen here.
static const uint64_t kPickupRecentMs = 250;
static volatile LONG64 g_statPickupDeclined = 0;

static bool PickupSeenRecently(uint32_t key)
{
    uint64_t now = Now();
    bool seen = false;
    AcquireSRWLockShared(&g_stuckLock);
    for (const auto& t : g_stuck)
        if (t.key == key && t.lastSeen != 0 && now - t.lastSeen <= kPickupRecentMs) { seen = true; break; }
    ReleaseSRWLockShared(&g_stuckLock);
    return seen;
}

static uint32_t PickupKey(const void* entity, const void* component)
{
    uint32_t id = 0;
    return SafeRead(static_cast<const uint8_t*>(entity) + 0x10, &id, 4) ? id   // entity id (+0x10)
                                                                        : (uint32_t)reinterpret_cast<uintptr_t>(component);
}

// wanted = false: we leave the pickup on purpose (ignore list, group off) - it is only marked as
// seen for the gate; lying there is then no sign of a full inventory (21.10.2026: false notice
// with twigs on the ignore list).
static bool PickupStuck(uint32_t key, bool wanted)
{
    uint64_t now = Now();
    AcquireSRWLockExclusive(&g_stuckLock);
    StuckPickup* s = nullptr;
    StuckPickup* oldest = &g_stuck[0];
    for (auto& t : g_stuck) {
        if (t.key == key && t.lastSeen != 0) { s = &t; break; }
        if (t.lastSeen < oldest->lastSeen) oldest = &t;
    }
    if (!s || now - s->lastSeen > kPickupGapMs) { if (!s) s = oldest; *s = { key, now, now, 0 }; }
    s->lastSeen = now;
    if (!wanted) {
        s->firstSeen = now;   // the episode starts when we want it
        ReleaseSRWLockExclusive(&g_stuckLock);
        return false;
    }
    bool stuck = s->blockedUntil > now;
    if (!stuck && now - s->firstSeen > kPickupStuckMs) {
        s->blockedUntil = now + kPickupBackoffMs;
        s->firstSeen = s->blockedUntil;   // after the pause the pickup gets a fresh try
        stuck = true;
        static uint64_t lastNotice = 0;
        if (now - lastNotice > kFullNoticeGapMs) { lastNotice = now; g_fullTick = now; }
    }
    ReleaseSRWLockExclusive(&g_stuckLock);
    if (stuck) InterlockedIncrement64(&g_statPickupStuck);
    return stuck;
}

// Slot loop (our automatic loot-all events only): true = the item stays in the container
static bool __fastcall KeepInContainer(uint32_t id)
{
    bool quest = g_cfg.keepQuestItems && std::binary_search(g_questItems.begin(), g_questItems.end(), id);
    if (quest) { InterlockedIncrement64(&g_statQuestKept); return true; }
    if (IsIgnoredItem(id)) { InterlockedIncrement64(&g_statFiltered); NoteFiltered(id); return true; }
    NoteContainerAttempt(id);
    if (g_cfg.debugLog) CountLootedItem(id);
    return false;
}

// A game window or our F8 menu is open (GateHook, every frame). The inner paths check it again right
// before they let an accept or a loot-all through: an accepted interaction rebinds an open window to
// another target (neosquall777 23.10.2026: chest shows a foreign row, menus close).
static volatile bool g_windowOpen = false;
static volatile LONG64 g_statWindowBlocked = 0;

// Pickup path of the magnet: called in place of the game's own "can this item be picked up" check
// (component = PickupItem, item id in its first 4 bytes; entity = the pickup; dist2 = squared
// distance from the player). The game's answer comes first.
static bool __fastcall FilterPickup(uint32_t* component, void* entity, float dist2)
{
    Timed timed(g_tFilter);
    static volatile LONG s_pickupLogs = 0;
    if (g_cfg.debugLog && s_pickupLogs < 10 && InterlockedIncrement(&s_pickupLogs) <= 10)
        Log("Zeiger: PickupItem=%p Entity=%p dist2=%.2f Gegenstand=%u", (void*)component, entity, dist2, component[0]);
    if (g_windowOpen) { InterlockedIncrement64(&g_statWindowBlocked); return false; }
    uint32_t key = PickupKey(entity, component);
    bool wanted = GroupAllowed(TemplateOf(entity), dist2, false);
    if (wanted && IsIgnoredItem(component[0])) {
        InterlockedIncrement64(&g_statPickupFiltered);
        NoteFiltered(component[0]);
        wanted = false;
    }
    bool stuck = PickupStuck(key, wanted);   // also marks the entity as "a real pickup" for the gate
    return wanted && !stuck;
}

// immediate_loot_action, per inventory slot: "xor r13d,r13d; cmp [rdi],r13d; je <next slot>"
static const char* kSigSlotLoop = "45 33 ED 44 39 2F 0F 84 ?? ?? ?? ?? 48 8D 8D ?? ?? ?? ?? E8";
static const size_t kSlotLoopSite = 0x03;          // cmp [rdi],r13d (3 bytes) + je rel32 (6 bytes)
static const size_t kEventBypassFromLootAll = 0x64; // r14 = &event.lootAll, +0x64 = &event.bypassInteraction

static volatile LONG64 g_statNotReleased = 0;
static volatile LONG64 g_statNoOffer = 0, g_statOfferDisabled = 0, g_statThrottled = 0, g_statNoHelpers = 0;
static volatile LONG64 g_statLooted = 0;   // loot-all passes after a processed accept (or the fallback)
static volatile LONG64 g_statHarvested = 0;   // plants/beds already harvested this visit: no further loot-all
static volatile LONG64 g_statPickupPath = 0;   // loot-all block reached by an ordinary pickup (never delayed)
static volatile LONG64 g_statPickupRepeat = 0;   // ... the same pickup again within kPickupLootGapMs: skipped
static volatile LONG g_acceptLogs = 0, g_corpseLogs = 0;   // debugLog: the first accepts are logged one by one
static volatile LONG64 g_statSkipped = 0;

// Released = carries AreaLootOverride with exclude=false (mod.lua sets it on everything the settings allow)
static bool IsReleased(uint8_t* ctx, void* entity)
{
    Timed timed(g_tReleased);
    uint8_t tmpArea[64] = {};
    void** area = g_getComponent(tmpArea, entity, *reinterpret_cast<void**>(ctx + g_ctxAreaLoot));
    return area && *area && !*static_cast<uint8_t*>(*area);
}

// Gate in front of the loot-all block for targets without PickupItem (corpses, bushes, chests and the
// neighbours the coarse query returns). Only released targets go on; all others take the game's own
// "next entity" jump as in the original. Up to 1.0.1 they went on into "loot all" without the accepted
// interaction, so the game emptied objects mod.lua never released – boss loot lost its key that way.
// The loot-all block has two entries: the gate stub below (corpses, chests, bushes: no PickupItem)
// and the game's own fall-through behind the pickup check and the near-distance compare (ordinary
// pickups in instant range). The gate notes its target here; OnLootAllTarget applies the delay and
// the "no loot-all in the accept pass" rule only to that target. Same thread, same magnet pass.
static thread_local void* g_gateEntity = nullptr;

static bool GateDecision(uint8_t* ctx, uint8_t* gameRsp, float dist2);

static bool __fastcall OnNoPickupTarget(uint8_t* ctx, uint8_t* gameRsp, float dist2)
{
    bool go = GateDecision(ctx, gameRsp, dist2);
    g_gateEntity = go ? *reinterpret_cast<void**>(gameRsp + 0x50) : nullptr;
    return go;
}

static bool GateDecision(uint8_t* ctx, uint8_t* gameRsp, float dist2)
{
    Timed timed(g_tStub);
    if (g_windowOpen) { InterlockedIncrement64(&g_statWindowBlocked); return false; }
    void* entity = *reinterpret_cast<void**>(gameRsp + 0x50);
    if (PickupSeenRecently(PickupKey(entity, entity))) {   // a pickup the game declined: next object, never loot-all
        InterlockedIncrement64(&g_statPickupDeclined);
        static volatile LONG s_logs = 0;
        if (g_cfg.debugLog && s_logs < 30 && InterlockedIncrement(&s_logs) <= 30) {
            const TemplateInfo* pt = TemplateOf(entity);
            Log("Aufheben abgelehnt, uebersprungen: Vorlage=%s", pt ? pt->name.c_str() : "unbekannt");
        }
        return false;
    }
    const TemplateInfo* t = TemplateOf(entity);
    bool checkable = g_ctxAreaLoot && g_getComponent;
    bool released = checkable && IsReleased(ctx, entity);
    if (t && t->group == G_ENEMY && !released) {   // a living enemy (own query for pauseNearEnemies, or a neighbour): never loot, remember
        if (!(t->flags & TF_ANIMAL) && dist2 >= 0 && dist2 <= g_cfg.enemyRadius * g_cfg.enemyRadius) { g_enemyNearTick = Now(); InterlockedIncrement64(&g_statEnemyNear); }
        return false;
    }
    if (!checkable) return GroupAllowed(t, dist2, false);   // release check not available: as in 1.0.0
    if (!released) { InterlockedIncrement64(&g_statSkipped); return false; }
    return GroupAllowed(t, dist2, true);
}

// Called from the magnet for a target without PickupItem (corpse, bush) right before the game
// sends its "loot all" event. Corpses and bushes roll their loot only when an interaction is
// accepted, so we first send the same AcceptOfferedInteractionEvent that pressing E sends.
// Only for targets that carry AreaLootOverride: mod.lua sets it on everything the settings allow.
// Returns whether the game's loot-all may run in this pass (false = the stub takes the "next
// entity" path). Never true in the pass that sent the accept (see TargetPending).
static bool __fastcall OnLootAllTarget(uint8_t* ctx, uint8_t* gameRsp, uint32_t host, float dist2)
{
    Timed timed(g_tLootAll);
    InterlockedIncrement64(&g_statLootAll);
    void* world = *reinterpret_cast<void**>(gameRsp + 0x30);
    void* entity = *reinterpret_cast<void**>(gameRsp + 0x50);
    bool viaGate = g_gateEntity == entity;   // corpse/chest/bush; otherwise an ordinary pickup in instant range
    g_gateEntity = nullptr;
    if (g_windowOpen) { InterlockedIncrement64(&g_statWindowBlocked); return false; }   // also before the helper fallback
    if (!g_getComponent || !g_getGuest || !g_allocAccept) { InterlockedIncrement64(&g_statNoHelpers); return true; }   // as in 1.0.3

    // Pickup path: never blocked or delayed (the game already decided to take it); the first accept
    // per visit as in 1.0.3 for pickups with an offer (fireflies and other pickups with an inventory).
    if (!viaGate) {
        InterlockedIncrement64(&g_statPickupPath);
        if (!PickupLootDue(host)) { InterlockedIncrement64(&g_statPickupRepeat); return false; }   // taken a moment ago: not again
        if (g_ctxAreaLoot && !IsReleased(ctx, entity)) return true;   // not released by mod.lua: vanilla take, no accept
        uint8_t tmpP[64] = {};
        void** slotP = g_getComponent(tmpP, entity, *reinterpret_cast<void**>(ctx + kCtxOfferRuntime));
        uint8_t* offerP = slotP ? static_cast<uint8_t*>(*slotP) : nullptr;
        if (!offerP || offerP[kOfferDisabledOffset] || !FirstAcceptDue(host)) return true;
        uint8_t* evP = g_allocAccept(*reinterpret_cast<void**>(ctx + kCtxAcceptEvent));
        if (!evP) return true;
        uint32_t idP = *reinterpret_cast<uint32_t*>(offerP + kOfferIdOffset);
        TargetAccepted(host, idP);
        uint8_t h[64] = {}, gst[64] = {};
        if (g_getHeader) *reinterpret_cast<uint64_t*>(evP) = *g_getHeader(world, h);
        *reinterpret_cast<uint32_t*>(evP + 0x08) = *g_getGuest(world, gst);
        *reinterpret_cast<uint32_t*>(evP + 0x0C) = host;
        *reinterpret_cast<uint32_t*>(evP + 0x10) = idP;
        InterlockedIncrement64(&g_statAccepts);
        return true;
    }

    if (g_ctxAreaLoot && !IsReleased(ctx, entity)) {
        InterlockedIncrement64(&g_statNotReleased);
        return false;
    }
    if (!GroupAllowed(TemplateOf(entity), dist2, true)) return false;   // group off, out of its radius, or yield ignored
    // debugLog: pointers of the first released targets, to read their memory from outside
    static volatile LONG s_ptrLogs = 0;
    if (g_cfg.debugLog && s_ptrLogs < 30) {
        static uint32_t s_seen[30]; static LONG s_n = 0;
        bool seen = false;
        for (LONG i = 0; i < s_n && i < 30; ++i) if (s_seen[i] == host) seen = true;
        if (!seen && s_n < 30) { s_seen[s_n++] = host; InterlockedIncrement(&s_ptrLogs); Log("Zeiger: Ziel=%u Entity=%p World=%p Ctx=%p", host, entity, world, (void*)ctx); }
    }
    uint8_t tmp[64] = {};
    void** slot = g_getComponent(tmp, entity, *reinterpret_cast<void**>(ctx + kCtxOfferRuntime));
    uint8_t* offer = slot ? static_cast<uint8_t*>(*slot) : nullptr;
    bool usable = offer && !offer[kOfferDisabledOffset];
    uint32_t offerId = usable ? *reinterpret_cast<uint32_t*>(offer + kOfferIdOffset) : 0;
    TargetAction action = TargetPending(host, usable ? &offerId : nullptr);
    if (action == TargetAction::Wait) { InterlockedIncrement64(&g_statThrottled); return false; }
    if (action == TargetAction::Loot) {
        // Plants and beds: one loot-all after the accept harvests; more while the plant still stands
        // harvested again - 3 plant fiber from a dried-up plant instead of 1 (UtopiA, 21.10.2026).
        const TemplateInfo* pt = TemplateOf(entity);
        bool reacted = false;
        if (pt && (pt->group == G_PLANT || pt->group == G_FARM) && !HarvestLootDue(host, usable ? &offerId : nullptr, &reacted) && reacted) {
            InterlockedIncrement64(&g_statHarvested);
            return false;
        }
        if (!offer) InterlockedIncrement64(&g_statNoOffer);
        else if (!usable) InterlockedIncrement64(&g_statOfferDisabled);
        else InterlockedIncrement64(&g_statLooted);
        return true;
    }

    uint8_t* ev = g_allocAccept(*reinterpret_cast<void**>(ctx + kCtxAcceptEvent));
    if (!ev) return false;
    TargetAccepted(host, offerId);
    uint8_t tmp2[64] = {};
    if (g_getHeader) *reinterpret_cast<uint64_t*>(ev) = *g_getHeader(world, tmp2);
    uint8_t tmp3[64] = {};
    *reinterpret_cast<uint32_t*>(ev + 0x08) = *g_getGuest(world, tmp3);   // guest = player
    *reinterpret_cast<uint32_t*>(ev + 0x0C) = host;                         // host = corpse/bush
    *reinterpret_cast<uint32_t*>(ev + 0x10) = *reinterpret_cast<uint32_t*>(offer + kOfferIdOffset);
    InterlockedIncrement64(&g_statAccepts);
    if (g_cfg.debugLog) {
        const TemplateInfo* t = TemplateOf(entity);
        // corpses, enemies, bosses and unknown templates are logged beyond the general limit (boss diagnosis)
        bool corpseLike = !t || t->group == G_ENEMY || t->group == G_CORPSE || t->group == G_BOSS;
        if (InterlockedIncrement(&g_acceptLogs) <= 200 || (corpseLike && InterlockedIncrement(&g_corpseLogs) <= 500))
            Log("Angenommen: Ziel=%u Angebot=%u offered=%u Gast=%u Vorlage=%s Gruppe=%u Flags=%u", host,
                *reinterpret_cast<uint32_t*>(offer + kOfferIdOffset), offer[45], *reinterpret_cast<uint32_t*>(ev + 0x08),
                t ? t->name.c_str() : "unbekannt", t ? t->group : 0, t ? t->flags : 0);
    }
    return false;   // the loot-all follows in a later pass
}

// ---------------------------------------------------------------------------
// Query hook
// ---------------------------------------------------------------------------

using QueryFn = void*(__fastcall*)(void*, void*, void*, void*, uint32_t, uint8_t);
static QueryFn g_origQuery = nullptr;

// Diagnostics, written to the log every few seconds
static volatile LONG64 g_statQueries = 0, g_statPickups = 0, g_statCorpses = 0;
static volatile LONG64 g_statGate = 0, g_statGateHeld = 0, g_statGateVanilla = 0;

static void* __fastcall QueryHook(void* world, void* result, void* grid, void* box, uint32_t hash, uint8_t flag)
{
    // result list: { entries*, count, capacity, grow-callback } – the query appends
    auto count = reinterpret_cast<volatile uint64_t*>(static_cast<uint8_t*>(result) + 8);
    uint64_t before = *count;
    void* r;
    { Timed timed(g_tQueryPickups); r = g_origQuery(world, result, grid, box, hash, flag); }
    if (hash != kHashPickupItem) return r;
    uint64_t mid = *count;
    // corpses, chests, bushes, crops (mod.lua decides with AreaLootOverride what may be accepted)
    { Timed timed(g_tQueryCorpses); g_origQuery(world, result, grid, box, kHashLootedCorpse, flag); }
    // enemies for the "enemy nearby" pause: they have no PickupItem and no release, so the gate stub
    // (OnNoPickupTarget) sees them, notes the distance and skips them
    if (g_cfg.pauseNearEnemies) g_origQuery(world, result, grid, box, kHashEnemySettings, flag);
    InterlockedIncrement64(&g_statQueries);
    InterlockedAdd64(&g_statPickups, (LONG64)(mid - before));
    InterlockedAdd64(&g_statCorpses, (LONG64)(*count - mid));
    return r;
}

// Gate "is the magnet active for this player": vanilla = hold-E bit && (flag 0x1000 || check(r8, 8))
using GateCheckFn = bool(__fastcall*)(void*, uint8_t);
static GateCheckFn g_gateCheck = nullptr;

// A game window is open (chest, crafting station, dialogue): the game shows the mouse cursor, during
// play it is hidden. While a window is open the magnet stays as in the vanilla game (E cannot be held
// in a window, so: off). An accepted interaction of ours would otherwise rebind or close that window
// (reports after 1.0.2: "crafting window closes on its own", "chest shows a row of empty slots").
// The pointer is replaceable for the test.
static bool CursorShown()
{
    CURSORINFO ci = { sizeof ci };
    return GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) != 0;
}
static bool (*g_cursorShown)() = &CursorShown;
bool Overlay_MenuOpen();   // overlay.cpp: our own F8 menu (draws its own cursor)
static volatile LONG64 g_statGatePaused = 0, g_statGateBlink = 0, g_statEnemyPaused = 0;
static const uint64_t kEnemyPauseMs = 2000;   // after the last enemy seen within enemyRadius
// Every kGateBlinkMs the magnet is switched off for one frame, like releasing and pressing E again:
// the game remembers the targets of one magnet run, and a corpse that was still empty in the moment
// of death stays "done" until the run restarts (seen 19.10.: corpses were looted as soon as the
// pause menu was opened and closed, i.e. after one frame without the magnet).
static const uint64_t kGateBlinkMs = 2000;
extern uint64_t g_lastCursorRecenter;   // overlay.cpp: last SetCursorPos of the game (diagnostics)

static bool __fastcall GateHook(uint8_t* state, uint8_t* input, void* actor)
{
    Timed timed(g_tGate);
    InterlockedIncrement64(&g_statGate);
    if (g_cfg.debugLog) {   // pointers of the player state, every 30 s (they may change on reload)
        static uint64_t s_lastPtrLog = 0;
        uint64_t now = GetTickCount64();
        if (now - s_lastPtrLog > 30000) { s_lastPtrLog = now; Log("Zeiger: Spielerzustand=%p Eingabe=%p Akteur=%p", (void*)state, (void*)input, actor); }
    }
    bool held = (input[0x34] & 0x80) != 0;
    bool vanilla = held && ((*reinterpret_cast<uint16_t*>(state + 0x5B0) & 0x1000) || g_gateCheck(actor, 8));
    if (held) InterlockedIncrement64(&g_statGateHeld);
    if (vanilla) InterlockedIncrement64(&g_statGateVanilla);
    bool window = g_cursorShown() || Overlay_MenuOpen();
    g_windowOpen = window;
    if (!g_cfg.autoLoot) return vanilla;
    static bool lastWindow = false;
    static LONG windowLogs = 0;
    if (window != lastWindow) {
        lastWindow = window;
        if (g_cfg.debugLog && InterlockedIncrement(&windowLogs) <= 60)
            Log("Mauszeiger %s – Magnet %s (letzte Neuzentrierung vor %llu ms)", window ? "sichtbar" : "versteckt",
                window ? "aus" : "automatisch", (unsigned long long)(GetTickCount64() - g_lastCursorRecenter));
    }
    // off, not "vanilla": the E bit stays set in windows (log 23.10.2026: E-gehalten rose with the cursor shown)
    if (window) { InterlockedIncrement64(&g_statGatePaused); return false; }
    static uint64_t s_lastBlink = 0;
    uint64_t t = Now();
    if (g_cfg.pauseNearEnemies && g_enemyNearTick && t - g_enemyNearTick < kEnemyPauseMs) { InterlockedIncrement64(&g_statEnemyPaused); return vanilla; }
    if (t - s_lastBlink > kGateBlinkMs) { s_lastBlink = t; InterlockedIncrement64(&g_statGateBlink); return vanilla; }
    return true;
}

// ---------------------------------------------------------------------------
// Patching
// ---------------------------------------------------------------------------

struct NearData {
    float boxHalf;
    float maxDist2;
    float radiusA;
    float radiusB;
    float nearDist2;        // closer than this = loot at once, else pull (instantLoot: whole radius)
    uint8_t ignoreSight;    // read by sightStub: 1 = skip the visibility raycast
    uint8_t trampoline[16];
    uint8_t lootAllStub[64];
    uint8_t gateStub[64];
    uint8_t questStub[128];
    uint8_t sightStub[48];
    uint8_t pickupStub[80];
};

static NearData* g_near = nullptr;
static float g_vanillaNearDist2 = 0.5625f;   // read from the game before patching

// Small code emitter for the stubs
struct Emit {
    uint8_t* p;
    void bytes(std::initializer_list<uint8_t> b) { for (uint8_t x : b) *p++ = x; }
    void imm64(uint64_t v) { memcpy(p, &v, 8); p += 8; }
    void rel32To(const void* target) { int32_t r = Rel32(p + 4, target); memcpy(p, &r, 4); p += 4; }
};

// Slot loop hook: skip empty slots (vanilla) and, for our automatic loot events only
// (bypassInteraction set), quest items and ignored items (KeepInContainer).
static bool PatchQuestFilter()
{
    int count = 0;
    uint8_t* m = FindUnique(kSigSlotLoop, &count);
    if (!m) { Log("Muster 'slotLoop' gefunden: %d mal – Questfilter aus", count); return false; }
    uint8_t* site = m + kSlotLoopSite;
    uint8_t* skip = site + 9 + *reinterpret_cast<int32_t*>(site + 5);   // je target (next slot)
    uint8_t* cont = site + 9;

    Emit e{ g_near->questStub };
    e.bytes({ 0x8B, 0x07 });                         // mov eax,[rdi]
    e.bytes({ 0x85, 0xC0 });                         // test eax,eax
    e.bytes({ 0x0F, 0x84 }); e.rel32To(skip);        // jz skip
    e.bytes({ 0x41, 0x80, 0x7E, (uint8_t)kEventBypassFromLootAll, 0x00 });  // cmp byte [r14+64h],0
    e.bytes({ 0x0F, 0x84 }); e.rel32To(cont);        // je cont (vanilla event: no filter)
    e.bytes({ 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, 0x50 }); // push rcx..r11,rax
    e.bytes({ 0x8B, 0xC8 });                         // mov ecx,eax
    e.bytes({ 0x48, 0x83, 0xEC, 0x28 });             // sub rsp,28h
    e.bytes({ 0x48, 0xB8 }); e.imm64(reinterpret_cast<uint64_t>(&KeepInContainer));
    e.bytes({ 0xFF, 0xD0 });                         // call rax
    e.bytes({ 0x48, 0x83, 0xC4, 0x28 });             // add rsp,28h
    e.bytes({ 0x84, 0xC0 });                         // test al,al
    e.bytes({ 0x58, 0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59 }); // pop (flags kept)
    e.bytes({ 0x0F, 0x85 }); e.rel32To(skip);        // jnz skip (quest item stays)
    e.bytes({ 0xE9 }); e.rel32To(cont);              // jmp cont
    FlushInstructionCache(GetCurrentProcess(), g_near->questStub, e.p - g_near->questStub);

    uint8_t jmp[9] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90 };
    int32_t rel = Rel32(site + 5, g_near->questStub);
    memcpy(jmp + 1, &rel, 4);
    WriteCode(site, jmp, sizeof jmp);
    return true;
}

// The magnet's "can this pickup be looted" check ("lea rdx,[rbp+100h]; call Check" right behind the
// PickupItem lookup, rcx = PickupItem component) is redirected to a stub that runs the game's check
// and, when it says yes, FilterPickup on the component. Both calls keep the x64 stack alignment
// (entry rsp = 8 mod 16, minus 28h = 0 mod 16).
static void BuildPickupStub(uint8_t* call)
{
    uint8_t* original = call + 5 + *reinterpret_cast<int32_t*>(call + 1);
    Emit e{ g_near->pickupStub };
    e.bytes({ 0x48, 0x83, 0xEC, 0x28 });                 // sub rsp,28h
    e.bytes({ 0x48, 0x89, 0x4C, 0x24, 0x20 });           // mov [rsp+20h],rcx (component)
    e.bytes({ 0x48, 0xB8 }); e.imm64(reinterpret_cast<uint64_t>(original));
    e.bytes({ 0xFF, 0xD0 });                             // call original (rcx, rdx unchanged)
    e.bytes({ 0x84, 0xC0 });                             // test al,al
    e.bytes({ 0x74, 0x1D });                             // jz done (29 bytes)
    e.bytes({ 0x48, 0x8B, 0x4C, 0x24, 0x20 });           // mov rcx,[rsp+20h]            (component)
    e.bytes({ 0x48, 0x8B, 0x94, 0x24, 0x80, 0x00, 0x00, 0x00 });   // mov rdx,[rsp+80h]  (entity = game rsp+50h)
    e.bytes({ 0xF3, 0x0F, 0x10, 0xD7 });                 // movss xmm2,xmm7              (squared distance)
    e.bytes({ 0x48, 0xB8 }); e.imm64(reinterpret_cast<uint64_t>(&FilterPickup));
    e.bytes({ 0xFF, 0xD0 });                             // call FilterPickup
    e.bytes({ 0x48, 0x83, 0xC4, 0x28 });                 // done: add rsp,28h
    e.bytes({ 0xC3 });                                   // ret
    FlushInstructionCache(GetCurrentProcess(), g_near->pickupStub, e.p - g_near->pickupStub);
    int32_t rel = Rel32(call + 5, g_near->pickupStub);
    WriteCode(call + 1, &rel, 4);
}

// "je <skip>" (0F 84 rel32) of the no-PickupItem check -> gate stub: OnNoPickupTarget(ctx = rbx,
// gameRsp = rsp); released -> loot-all block, else the original skip target
static void BuildGateStub(uint8_t* je, uint8_t* lootAll)
{
    uint8_t* skip = je + 6 + *reinterpret_cast<int32_t*>(je + 2);
    Emit g{ g_near->gateStub };
    g.bytes({ 0x48, 0x8B, 0xCB,                       // mov rcx, rbx
              0x48, 0x8B, 0xD4,                       // mov rdx, rsp
              0xF3, 0x0F, 0x10, 0xD7,                 // movss xmm2, xmm7 (squared distance of the target)
              0x48, 0x83, 0xEC, 0x20,                 // sub rsp, 20h
              0x48, 0xB8 });                          // mov rax, imm64
    g.imm64(reinterpret_cast<uint64_t>(&OnNoPickupTarget));
    g.bytes({ 0xFF, 0xD0,                             // call rax
              0x48, 0x83, 0xC4, 0x20,                 // add rsp, 20h
              0x84, 0xC0,                             // test al, al
              0x0F, 0x84 });                          // jz skip
    g.rel32To(skip);
    g.bytes({ 0xE9 });                                // jmp loot-all block
    g.rel32To(lootAll);
    FlushInstructionCache(GetCurrentProcess(), g_near->gateStub, g.p - g_near->gateStub);
    int32_t jeRel = Rel32(je + 6, g_near->gateStub);
    WriteCode(je + 2, &jeRel, 4);
}

// Line of sight: the "jne <skip raycast>" (0F 85 rel32) jumps to sightStub, which keeps the
// vanilla jump and additionally skips the raycast while g_near->ignoreSight is set.
static void BuildSightStub(uint8_t* jne)
{
    uint8_t* skip = jne + 6 + *reinterpret_cast<int32_t*>(jne + 2);
    Emit e{ g_near->sightStub };
    e.bytes({ 0x0F, 0x85 }); e.rel32To(skip);        // jne skip (vanilla)
    e.bytes({ 0x9C });                               // pushfq
    e.bytes({ 0x80, 0x3D });                         // cmp byte [rip+disp32],0
    int32_t flagRel = Rel32(e.p + 5, &g_near->ignoreSight);   // relative to the end (disp32 + imm8)
    memcpy(e.p, &flagRel, 4); e.p += 4;
    e.bytes({ 0x00 });
    e.bytes({ 0x74, 0x06 });                         // je vanilla
    e.bytes({ 0x9D });                               // popfq
    e.bytes({ 0xE9 }); e.rel32To(skip);              // jmp skip
    e.bytes({ 0x9D });                               // vanilla: popfq
    e.bytes({ 0xE9 }); e.rel32To(jne + 6);           // jmp back (raycast)
    FlushInstructionCache(GetCurrentProcess(), g_near->sightStub, e.p - g_near->sightStub);

    uint8_t jmp[6] = { 0xE9, 0, 0, 0, 0, 0x90 };
    int32_t rel = Rel32(jne + 5, g_near->sightStub);
    memcpy(jmp + 1, &rel, 4);
    WriteCode(jne, jmp, sizeof jmp);
}

static void WriteJumpAbs(uint8_t* at, const void* target)   // mov rax, imm64; jmp rax (12 bytes)
{
    uint8_t code[12] = { 0x48, 0xB8 };
    uint64_t t = reinterpret_cast<uint64_t>(target);
    memcpy(code + 2, &t, 8);
    code[10] = 0xFF; code[11] = 0xE0;
    WriteCode(at, code, sizeof code);
}

// Search radius of the magnet: the general radius or a larger group radius (1.1.6, faytleingod84: the general
// radius is the default for groups at 0, no longer their maximum). GroupAllowed then checks every target
// against its own group radius. Without templates.txt there is no group check: the general radius stays the limit.
static float SearchRadius()
{
    float r = g_cfg.radius;
    if (g_templates.empty()) return r;
    const float groups[] = { g_cfg.radiusGround, g_cfg.radiusCollectibles, g_cfg.radiusPlants, g_cfg.radiusFarm,
                             g_cfg.radiusCorpses, g_cfg.radiusChests };
    for (float g : groups) if (g > r) r = g;
    return r;
}

static void ApplyRadius()
{
    float r = SearchRadius();
    g_near->boxHalf = r > 7.0f ? r : 7.0f;
    g_near->maxDist2 = r * r;
    g_near->radiusA = r;
    g_near->radiusB = r;
    g_near->nearDist2 = g_cfg.instantLoot ? r * r : g_vanillaNearDist2;
    g_near->ignoreSight = g_cfg.ignoreLineOfSight ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Data patches while the game runs: auto-smash program and stack sizes
// ---------------------------------------------------------------------------
// mod.lua writes both into the game data when it loads. To change them without a restart, the DLL
// finds the loaded copies in memory and rewrites the values in place:
//  * the auto-smash program keeps its parameters in a data block (impact/autosmash.data.json):
//    Range::value (+128) = smash radius, 0 = off; SharedTag::value (+224) and StoneTag::value (+528)
//    are marker GUIDs the program checks per object – the "off" GUID is carried by nothing. A block
//    is recognised by SmashTag::value (+208) and AltarTag::value (+256).
//  * a keen::ItemInfo record starts with itemId (u32), objectId (GUID) and maxStackSize (u16 at +20);
//    items.txt lists the GUID and the original stack size of every item.
// The scan runs in the worker thread once the player is in the world (the data is loaded by then)
// and again on request when a block has moved. Reading memory the game may free meanwhile is guarded.

static const uint8_t kSmashTag[16]  = { 0x5A, 0xDE, 0xC0, 0xA5, 0x00, 0x1E, 0x0D, 0x4D, 0x9A, 0x11, 0x00, 0x00, 0x00, 0x0A, 0x50, 0x01 };
static const uint8_t kSharedTag[16] = { 0x5A, 0xDE, 0xC0, 0xA5, 0x00, 0x1E, 0x0D, 0x4D, 0x9A, 0x11, 0x00, 0x00, 0x00, 0x0A, 0x50, 0x03 };
static const uint8_t kAltarTag[16]  = { 0x5A, 0xDE, 0xC0, 0xA5, 0x00, 0x1E, 0x0D, 0x4D, 0x9A, 0x11, 0x00, 0x00, 0x00, 0x0A, 0x50, 0x04 };
static const uint8_t kStoneTag[16]  = { 0x5A, 0xDE, 0xC0, 0xA5, 0x00, 0x1E, 0x0D, 0x4D, 0x9A, 0x11, 0x00, 0x00, 0x00, 0x0A, 0x50, 0x05 };
static const uint8_t kOffTag[16]    = { 0x5A, 0xDE, 0xC0, 0xA5, 0x00, 0x1E, 0x0D, 0x4D, 0x9A, 0x11, 0x00, 0x00, 0x00, 0x0A, 0x50, 0x09 };
static const size_t kSmashBaseRangeOff = 272;   // BaseRange::value = 160 m + radius (base protection)
static const size_t kSmashRangeOff = 128, kSmashTagOff = 208, kSmashSharedOff = 224, kSmashAltarOff = 256, kSmashStoneOff = 528,
                    kSmashBlockSize = 544;
static const size_t kItemStackOff = 20, kItemRecordMin = 24;

struct ItemSite { uint8_t* at; const ItemName* item; uint16_t expectedStack; };
static std::vector<uint8_t*> g_smashBlocks;
static std::vector<ItemSite> g_itemSites;
static SRWLOCK g_dataLock = SRWLOCK_INIT;
static bool g_dataScanned = false;
static HANDLE g_wake = nullptr;                                   // wakes the worker for a pending apply
static volatile LONG g_pending = 0;
enum { kPendingSmash = 1, kPendingStacks = 2, kPendingRescan = 4 };

static void RequestApply(LONG bits)
{
    InterlockedOr(&g_pending, bits);
    if (g_wake) SetEvent(g_wake);
}

// Reads and writes guarded against pages the game has freed meanwhile (no C++ objects: SEH).
static bool RawEquals(const void* at, const void* expect, size_t n)
{
    __try { return memcmp(at, expect, n) == 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool RawWrite(void* at, const void* data, size_t n)
{
    __try { memcpy(at, data, n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool SmashBlockOk(const uint8_t* b)
{
    // Check the layout as well as the two GUIDs before touching ANY slot (including +272).
    // These are resource parameters, never the running aura's mutable actor/target values.
    __try {
        float range = 0, baseRange = 0;
        uint64_t wait = 0;
        memcpy(&range, b + kSmashRangeOff, 4);
        memcpy(&baseRange, b + kSmashBaseRangeOff, 4);
        memcpy(&wait, b + 144, 8);
        return range >= 0 && range <= 50 && baseRange >= 160 && baseRange <= 210 &&
            wait == 1000000000ull && b[192] == 1 && b[193] == 0 && b[240] == 14 && b[320] == 54 &&
            memcmp(b + kSmashTagOff, kSmashTag, 16) == 0 && memcmp(b + kSmashAltarOff, kAltarTag, 16) == 0 &&
            (memcmp(b + kSmashSharedOff, kSharedTag, 16) == 0 || memcmp(b + kSmashSharedOff, kOffTag, 16) == 0) &&
            (memcmp(b + kSmashStoneOff, kStoneTag, 16) == 0 || memcmp(b + kSmashStoneOff, kOffTag, 16) == 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The record copies lie in read-only data regions (the loaded resources). Those regions must not
// stay writable: with 2 regions / 419 MB switched to PAGE_READWRITE the game lagged for the rest
// of the session (20.10.2026, unplayable in the forest; reading them is harmless, liveData=scan).
// So the writes of one apply are collected, sorted and grouped into page ranges (gaps up to 16
// pages); each range is unlocked, written and locked again at once. Per-record VirtualProtect
// calls took over a minute for 16 000 records (one TLB flush each), the ranges keep it to a few.
struct DataWrite { uint8_t* at; uint8_t data[16]; uint8_t n; uint32_t tag; bool ok; };
static size_t g_lastRanges = 0;   // page ranges unlocked by the last commit (log)

static void QueueWrite(std::vector<DataWrite>& writes, void* at, const void* data, size_t n, uint32_t tag)
{
    DataWrite w = { static_cast<uint8_t*>(at), {}, (uint8_t)n, tag, false };
    memcpy(w.data, data, n);
    writes.push_back(w);
}

static int CommitWrites(std::vector<DataWrite>& writes)
{
    g_lastRanges = 0;
    if (writes.empty() || !g_cfg.liveWrite) return 0;   // liveData=scan: diagnosis only
    std::sort(writes.begin(), writes.end(), [](const DataWrite& a, const DataWrite& b) { return a.at < b.at; });
    const uintptr_t kPage = 4096, kGap = 16 * kPage;
    auto pageDown = [&](uintptr_t a) { return a & ~(kPage - 1); };
    auto pageUp = [&](uintptr_t a) { return (a + kPage - 1) & ~(kPage - 1); };
    int written = 0;
    for (size_t i = 0; i < writes.size();) {
        uintptr_t start = pageDown(reinterpret_cast<uintptr_t>(writes[i].at));
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void*>(start), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || mbi.Type == MEM_IMAGE) { ++i; continue; }
        uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        uintptr_t end = pageUp(reinterpret_cast<uintptr_t>(writes[i].at) + writes[i].n);
        if (end > regionEnd) { ++i; continue; }   // a write across the region border: never
        size_t j = i + 1;
        while (j < writes.size()) {
            uintptr_t a = reinterpret_cast<uintptr_t>(writes[j].at), e = pageUp(a + writes[j].n);
            if (a >= end + kGap || e > regionEnd) break;
            if (e > end) end = e;
            ++j;
        }
        DWORD prot = mbi.Protect & 0xFF, old = 0;
        bool writable = prot == PAGE_READWRITE || prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
        bool unlocked = writable || VirtualProtect(reinterpret_cast<void*>(start), end - start, PAGE_READWRITE, &old) ||
                        VirtualProtect(reinterpret_cast<void*>(start), end - start, PAGE_WRITECOPY, &old);
        if (unlocked) {
            for (size_t k = i; k < j; ++k) {
                writes[k].ok = RawWrite(writes[k].at, writes[k].data, writes[k].n);
                written += writes[k].ok;
            }
            if (!writable) VirtualProtect(reinterpret_cast<void*>(start), end - start, old, &old);   // locked again at once
            ++g_lastRanges;
        }
        i = j;
    }
    return written;
}

struct ScanState {
    std::vector<uint8_t*>* blocks;
    std::vector<ItemSite>* sites;
    std::vector<std::pair<uint32_t, const ItemName*>>* ids;   // sorted by id
    uint64_t idBits[1024];                                    // bit filter over id & 0xFFFF
};

// As mod.lua at load: stackable items (original > 1) get the wish when it is larger; 0 = game default.
static uint16_t StackTargetFor(uint16_t original, int wish)
{
    return (original > 1 && wish > original) ? (uint16_t)wish : original;
}
static uint16_t StackTarget(uint16_t original) { return StackTargetFor(original, g_cfg.stackSize); }

static int g_stackAtLoad = -1;   // stackSize from config.txt at game start (what mod.lua wrote), -1 = unknown

// Other game data can start with the same id + GUID (21.10.2026: 14206 hits for 4262 items; writing
// there broke the axe - "not effective" on every tree). A hit counts only when +20 holds the original
// stack size or one this mod sets (mod.lua at load, the DLL live).
static bool StackValueOk(uint16_t value, uint16_t original)
{
    return value == original || value == StackTarget(original) ||
           (g_stackAtLoad >= 0 && value == StackTargetFor(original, g_stackAtLoad));
}

static void ScanFoundItem(ScanState* st, uint8_t* p, uint32_t id)
{
    auto& ids = *st->ids;
    size_t lo = 0, hi = ids.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (ids[mid].first < id) lo = mid + 1; else hi = mid;
    }
    for (; lo < ids.size() && ids[lo].first == id; ++lo)
        if (memcmp(p + 4, ids[lo].second->guid, 16) == 0 &&
            StackValueOk(*reinterpret_cast<uint16_t*>(p + kItemStackOff), ids[lo].second->stack) &&
            st->sites->size() < 4 * ids.size())
            st->sites->push_back({ p, ids[lo].second, *reinterpret_cast<uint16_t*>(p + kItemStackOff) });
}

// One memory region. No C++ objects in this function (SEH): the game may free the region meanwhile.
static void ScanRegionRaw(ScanState* st, uint8_t* base, size_t size)
{
    __try {
        uint8_t* end = base + size;
        // smash blocks: any alignment (a blob inside a resource)
        for (uint8_t* p = base; p + 16 <= end;) {
            p = static_cast<uint8_t*>(memchr(p, kSmashTag[0], end - p - 15));
            if (!p) break;
            if (memcmp(p, kSmashTag, 16) == 0 && p - base >= (ptrdiff_t)kSmashTagOff && p - kSmashTagOff + kSmashBlockSize <= end &&
                SmashBlockOk(p - kSmashTagOff) && st->blocks->size() < 64)
                st->blocks->push_back(p - kSmashTagOff);
            ++p;
        }
        // item records: 8-byte aligned (keen::ItemInfo alignment 8), checked every 4 bytes to be safe
        if (!st->ids->empty())
            for (uint8_t* p = base; p + kItemRecordMin <= end; p += 4) {
                uint32_t v = *reinterpret_cast<uint32_t*>(p);
                if ((st->idBits[(v & 0xFFFF) >> 6] >> (v & 63)) & 1) ScanFoundItem(st, p, v);
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static void ScanData()
{
    uint64_t t0 = GetTickCount64();
    std::vector<uint8_t*> blocks;
    std::vector<ItemSite> sites;
    std::vector<std::pair<uint32_t, const ItemName*>> ids;
    ScanState st = { &blocks, &sites, &ids, {} };
    for (const ItemName& it : g_itemNames) {
        bool hasGuid = false;
        for (uint8_t b : it.guid) hasGuid |= b != 0;
        if (!hasGuid || it.stack <= 1) continue;   // not stackable: nothing to change, never written
        ids.push_back({ it.id, &it });
        st.idBits[(it.id & 0xFFFF) >> 6] |= 1ull << (it.id & 63);
    }
    std::sort(ids.begin(), ids.end(), [](const std::pair<uint32_t, const ItemName*>& a, const std::pair<uint32_t, const ItemName*>& b) { return a.first < b.first; });
    MEMORY_BASIC_INFORMATION mbi;
    size_t regions = 0, bytes = 0;
    for (uint8_t* p = nullptr; VirtualQuery(p, &mbi, sizeof mbi);) {
        uint8_t* base = static_cast<uint8_t*>(mbi.BaseAddress);
        // Only private read-only regions (the loaded resources, ~0.5 GB): the item records and the
        // program's resource copy lie there. Reading the read-write regions (~5 GB) once made the game
        // lag for the rest of the session (19.10.2026, 64 GB RAM; cause unknown, probably memory the
        // graphics driver owns), reading write-combined memory (~10 GB) took minutes.
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && (mbi.Protect & 0xFF) == PAGE_READONLY) {
            ScanRegionRaw(&st, base, mbi.RegionSize);
            ++regions; bytes += mbi.RegionSize;
        }
        if (base + mbi.RegionSize < p) break;
        p = base + mbi.RegionSize;
    }
    AcquireSRWLockExclusive(&g_dataLock);
    g_smashBlocks.swap(blocks);
    g_itemSites.swap(sites);
    g_dataScanned = true;
    ReleaseSRWLockExclusive(&g_dataLock);
    Log("Datensuche: Zerschlagen-Programm %zu Kopien, Stapelgroesse %zu Datensaetze (%zu Gegenstaende), %zu Bereiche/%zu MB in %llu ms",
        g_smashBlocks.size(), g_itemSites.size(), ids.size(), regions, bytes >> 20, (unsigned long long)(GetTickCount64() - t0));
}

// Writes the smash switches into every program copy; returns the number written (-1 = no scan yet).
static int ApplySmash()
{
    if (!g_dataScanned) return -1;
    float range = g_cfg.autoSmash ? g_cfg.radius : 0.0f;
    float baseRange = 160.0f + g_cfg.radius;   // as mod.lua: an altar's build zone is at most 160 m
    const uint8_t* shared = g_cfg.smashShared ? kSharedTag : kOffTag;
    const uint8_t* stone = g_cfg.stones ? kOffTag : kStoneTag;
    int n = 0;
    bool stale = false;
    std::vector<DataWrite> writes;
    AcquireSRWLockShared(&g_dataLock);
    for (size_t i = 0; i < g_smashBlocks.size(); ++i) {
        uint8_t* b = g_smashBlocks[i];
        if (!SmashBlockOk(b)) {
            stale = true;
            continue;
        }
        QueueWrite(writes, b + kSmashRangeOff, &range, 4, (uint32_t)i);
        QueueWrite(writes, b + kSmashBaseRangeOff, &baseRange, 4, (uint32_t)i);
        QueueWrite(writes, b + kSmashSharedOff, shared, 16, (uint32_t)i);
        QueueWrite(writes, b + kSmashStoneOff, stone, 16, (uint32_t)i);
    }
    CommitWrites(writes);
    std::vector<uint8_t> okPerBlock(g_smashBlocks.size(), 0);
    for (const DataWrite& w : writes) okPerBlock[w.tag] += w.ok;
    for (uint8_t c : okPerBlock) n += c == 4;   // a block counts when all four slots were written
    ReleaseSRWLockShared(&g_dataLock);
    if (stale) RequestApply(kPendingRescan);   // the worker scans again (at most every 30 s) and applies after it
    return n;
}

// Writes the stack size into every item record; returns the number changed (-1 = no scan yet).
static int ApplyStacks()
{
    if (!g_dataScanned) return -1;
    bool stale = false;
    std::vector<DataWrite> writes;
    AcquireSRWLockExclusive(&g_dataLock);
    for (size_t i = 0; i < g_itemSites.size(); ++i) {
        const ItemSite& s = g_itemSites[i];
        // The address may have been reused since ScanData(), even with the same id/GUID.
        // Accept only the value actually observed here or successfully written by us.
        if (!RawEquals(s.at, &s.item->id, 4) || !RawEquals(s.at + 4, s.item->guid, 16) ||
            !RawEquals(s.at + kItemStackOff, &s.expectedStack, 2)) {
            stale = true;
            continue;
        }
        uint16_t want = StackTarget(s.item->stack);
        if (s.expectedStack != want) QueueWrite(writes, s.at + kItemStackOff, &want, 2, (uint32_t)i);
    }
    int n = CommitWrites(writes);
    for (const DataWrite& w : writes)
        if (w.ok) memcpy(&g_itemSites[w.tag].expectedStack, w.data, 2);
    ReleaseSRWLockExclusive(&g_dataLock);
    if (stale) RequestApply(kPendingRescan);
    return n;
}

// Called by the in-game menu (overlay.cpp) for settings that take effect at once.
void ApplyLiveSetting(const std::string& key, const std::string& value)
{
    if (key == "enabled") return;   // restart only (debugLog may be switched live via the config key)
    if (!ApplyConfigKey(key, value)) return;
    if (key == "ignoreItems") { BuildIgnoreSet(value); return; }
    if (key == "autoSmash" || key == "smashBuildableAwayFromBase" || key == "collectStones" || key == "radiusMeters")
        RequestApply(kPendingSmash);   // the worker writes the program data (plus a fresh scan when needed)
    if (key == "stackSize") { RequestApply(kPendingStacks); return; }
    if (key == "lootEnemyCorpses" && !g_templates.empty()) return;   // live through the templates (else restart, as before)
    if (g_near) ApplyRadius();
}

const std::wstring& GameDirectory() { return g_gameDir; }

static void PatchDisp(uint8_t* dispAt, const void* newTarget)
{
    // rip-relative disp32 is relative to the end of the instruction; all our sites end right after the disp
    int32_t disp = Rel32(dispAt + 4, newTarget);
    WriteCode(dispAt, &disp, 4);
}

static bool Patch()
{
    // inMagnet: searched only inside the magnet function (starts at the "box" site)
    struct Site { const char* name; const char* sig; bool inMagnet; uint8_t* at; };
    Site sites[] = {
        { "box", kSigBox, false, nullptr },
        { "radii", kSigRadii, true, nullptr },
        { "maxDist", kSigMaxDist, true, nullptr },
        { "query", kSigQuery, true, nullptr },
        { "noPickup", kSigNoPickup, true, nullptr },
        { "gate", kSigGate, false, nullptr },
        { "pull", kSigPull, false, nullptr },
        { "sight", kSigSight, true, nullptr },
    };
    const size_t kMagnetSize = 0x1000;
    bool ok = true;
    for (auto& s : sites) {
        int count = 0;
        if (s.inMagnet) {
            if (!sites[0].at) { ok = false; continue; }
            s.at = FindUniqueIn(sites[0].at, kMagnetSize, s.sig, &count);
        } else {
            s.at = FindUnique(s.sig, &count);
        }
        if (!s.at) {
            Log("Muster '%s' gefunden: %d mal (erwartet 1)", s.name, count);
            ok = false;
        }
    }
    if (!ok) return false;

    g_near = reinterpret_cast<NearData*>(AllocNear(g_textStart, sizeof(NearData)));
    if (!g_near || !InRel32(g_textStart, g_near) || !InRel32(g_textStart + g_textSize, g_near)) {
        Log("Kein Speicher in Reichweite des Spielcodes");
        return false;
    }
    uint8_t* nearDisp = sites[1].at + kSigRadiiNear;
    g_vanillaNearDist2 = *reinterpret_cast<float*>(nearDisp + 4 + *reinterpret_cast<int32_t*>(nearDisp));
    if (g_cfg.debugLog) Log("Nahbereich Original: %.4f (erwartet 0.5625)", g_vanillaNearDist2);
    ApplyRadius();

    // Radius
    PatchDisp(sites[0].at + kSigBoxDisp, &g_near->boxHalf);
    PatchDisp(sites[1].at + kSigRadiiA, &g_near->radiusA);
    PatchDisp(sites[1].at + kSigRadiiB, &g_near->radiusB);
    PatchDisp(sites[2].at + kSigMaxDistDisp, &g_near->maxDist2);
    // Instant loot: everything inside the radius is looted directly instead of being pulled.
    // Static things (mushrooms, fireflies in bushes) cannot be pulled and would never arrive.
    // Always our own float, so the menu can switch it while the game runs.
    PatchDisp(nearDisp, &g_near->nearDist2);
    PatchDisp(sites[6].at + kSigPullA, &g_near->radiusA);
    PatchDisp(sites[6].at + kSigPullB, &g_near->radiusB);

    BuildSightStub(sites[7].at + kSigSightJne);

    // Loot-all targets (chests, bushes, crops, corpses): extra queries + jump to the loot-all event
    // when there is no PickupItem. Always installed; QueryHook asks for corpses only while enabled.
    {
        uint8_t* call = sites[3].at + kSigQueryCall;
        g_origQuery = reinterpret_cast<QueryFn>(call + 5 + *reinterpret_cast<int32_t*>(call + 1));
        WriteJumpAbs(g_near->trampoline, &QueryHook);
        int32_t rel = Rel32(call + 5, g_near->trampoline);
        WriteCode(call + 1, &rel, 4);

        // Game helpers, taken from call sites next to the verified patterns
        uint8_t* getCall = sites[4].at - 5;               // call GetComponent (PickupItem lookup)
        uint8_t* guestCall = sites[4].at + 0x7A;          // call GetCurrentEntity (loot-all block)
        if (getCall[0] == 0xE8 && guestCall[0] == 0xE8) {
            g_getComponent = reinterpret_cast<GetComponentFn>(getCall + 5 + *reinterpret_cast<int32_t*>(getCall + 1));
            g_getGuest = reinterpret_cast<GetIdFn>(guestCall + 5 + *reinterpret_cast<int32_t*>(guestCall + 1));
        }
        // The header pattern occurs twice in a row in the vanilla emitter; both call the same function.
        int n1 = 0, n2 = 0;
        uint8_t* header = FindFirst(kSigHeader, &n1);
        if (header) g_getHeader = reinterpret_cast<GetHeaderFn>(header + 12 + *reinterpret_cast<int32_t*>(header + 8));
        g_allocAccept = reinterpret_cast<AllocEventFn>(FindUnique(kSigAllocAccept, &n2));
        if (g_cfg.debugLog)
            Log("Helfer: GetComponent=%p Guest=%p Header=%p AllocAccept=%p", (void*)g_getComponent,
                (void*)g_getGuest, (void*)g_getHeader, (void*)g_allocAccept);

        // Every target that reaches the loot-all event block (pickups with an inventory like fireflies,
        // corpses, bushes, chests) first gets OnLootAllTarget(ctx=rbx, gameRsp=rsp, host=edi).
        // The block starts with "mov rax,[rbx+548h]" (7 bytes); it is replaced by a jmp to the stub,
        // which runs the call and then either the displaced instruction and the jump back (loot-all
        // allowed) or the game's "next entity" path, the original target of the no-PickupItem je
        // (same as in BuildGateStub) - so the accept and the loot-all never run in the same pass.
        uint8_t* lootAll = sites[4].at + kSigNoPickupLootAll;
        uint8_t* noPickupJe = sites[4].at + kSigNoPickupJe;
        uint8_t* nextEntity = noPickupJe + 6 + *reinterpret_cast<int32_t*>(noPickupJe + 2);
        static const uint8_t kBlockEntry[] = { 0x48, 0x8B, 0x83, 0x48, 0x05, 0x00, 0x00 };
        if (memcmp(lootAll, kBlockEntry, sizeof kBlockEntry) != 0) {
            Log("Loot-all-Block passt nicht – Leichen/Büsche aus");
            return false;
        }
        uint8_t* s = g_near->lootAllStub;
        size_t k = 0;
        const uint8_t pre[] = { 0x48, 0x8B, 0xCB,             // mov rcx, rbx
                                0x48, 0x8B, 0xD4,             // mov rdx, rsp
                                0x44, 0x8B, 0xC7,             // mov r8d, edi
                                0xF3, 0x0F, 0x10, 0xDF,       // movss xmm3, xmm7 (squared distance of the target)
                                0x48, 0x83, 0xEC, 0x20,       // sub rsp, 20h
                                0x48, 0xB8 };                 // mov rax, imm64
        memcpy(s, pre, sizeof pre); k = sizeof pre;
        uint64_t fn = reinterpret_cast<uint64_t>(&OnLootAllTarget);
        memcpy(s + k, &fn, 8); k += 8;
        const uint8_t post[] = { 0xFF, 0xD0,                  // call rax
                                 0x48, 0x83, 0xC4, 0x20,      // add rsp, 20h
                                 0x84, 0xC0,                  // test al, al
                                 0x0F, 0x84 };                // jz nextEntity
        memcpy(s + k, post, sizeof post); k += sizeof post;
        int32_t skipRel = Rel32(s + k + 4, nextEntity);
        memcpy(s + k, &skipRel, 4); k += 4;
        memcpy(s + k, kBlockEntry, sizeof kBlockEntry); k += sizeof kBlockEntry;   // mov rax,[rbx+548h]
        s[k++] = 0xE9;                                        // jmp back behind the displaced instruction
        int32_t backRel = Rel32(s + k + 4, lootAll + sizeof kBlockEntry);
        memcpy(s + k, &backRel, 4); k += 4;
        FlushInstructionCache(GetCurrentProcess(), s, k);

        uint8_t entryJmp[7] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90 };
        int32_t toStub = Rel32(lootAll + 5, s);
        memcpy(entryJmp + 1, &toStub, 4);
        WriteCode(lootAll, entryJmp, sizeof entryJmp);

        // Targets without PickupItem (corpses, bushes, chests) go through the release check into the
        // loot-all block; not released -> the original jump target ("next entity", only rax and rbp
        // are used there, rbx = context as here)
        BuildGateStub(sites[4].at + kSigNoPickupJe, lootAll);

        // Item filter on the pickup path: the game's check call right behind the PickupItem lookup
        uint8_t* pickupCall = sites[4].at + kSigNoPickupCheck;
        if (pickupCall[0] == 0xE8) BuildPickupStub(pickupCall);
        else Log("Pruefaufruf der Bodenbeute passt nicht – Gegenstandsfilter nur fuer Behaelter");

        // Accessor of AreaLootOverride: the magnet's own first check, right behind the query call
        int areaCount = 0;
        uint8_t* areaSite = FindUnique(kSigAreaLoot, &areaCount);
        if (areaSite && areaSite > call && areaSite < call + 0x200) {
            g_ctxAreaLoot = *reinterpret_cast<uint32_t*>(areaSite + 3);
            Log("Freigabe-Prüfung aktiv (AreaLootOverride, Kontext +%zX)", g_ctxAreaLoot);
        } else {
            Log("Muster 'areaLoot' gefunden: %d mal – Freigabe-Prüfung aus (wie 1.0.0)", areaCount);
        }

        LoadQuestItems();
        PatchQuestFilter();   // quest items and the item filter (ignoreItems, may be set later in the menu)
    }

    // Gate: replaced by GateHook (automatic mode + diagnostics). The vanilla helper is the
    // "jmp rel32" at the end of the gate: "B2 08 49 8B C8 E9 rel32" at +0x1A.
    uint8_t* gate = sites[5].at;
    static const uint8_t tail[] = { 0xB2, 0x08, 0x49, 0x8B, 0xC8, 0xE9 };
    if (memcmp(gate + 0x1A, tail, sizeof tail) != 0) {
        Log("Gate-Ende passt nicht – Auto-Modus nicht aktiv");
    } else {
        uint8_t* jmp = gate + 0x1F;
        g_gateCheck = reinterpret_cast<GateCheckFn>(jmp + 5 + *reinterpret_cast<int32_t*>(jmp + 1));
        WriteJumpAbs(gate, &GateHook);
    }
    (void)kSigGateJne;
    return true;
}

static DWORD WINAPI Worker(LPVOID)
{
    LoadConfig();
    g_stackAtLoad = g_cfg.stackSize;
    Log("Auto Loot Plus native %s – radius=%.1f autoLoot=%d corpses=%d instant=%d ignoreSight=%d", kModVersion, g_cfg.radius,
        g_cfg.autoLoot, g_cfg.corpses, g_cfg.instantLoot, g_cfg.ignoreLineOfSight);
    if (!g_cfg.enabled) { Log("deaktiviert (config.txt)"); return 0; }
    if (!FindText()) { Log(".text nicht gefunden"); return 0; }

    // The executable is wrapped (SteamStub); wait until the real code is readable.
    for (int i = 0; i < 240; ++i) {
        int count = 0;
        if (FindUnique(kSigGate, &count)) break;
        Sleep(250);
    }
    if (!Patch()) { Log("Nichts geändert (Spielversion passt nicht?)"); return 0; }
    Log("Patches angewendet");
    LoadItemNames();
    BuildIgnoreSet(g_cfg.ignoreItems);
    LoadTemplates();

    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    LONG64 lastQueries = -1;
    uint64_t lastScan = 0, lastStats = 0;
    bool rescanWanted = false;
    for (;;) {
        WaitForSingleObject(g_wake, 5000);

        // data patches: the first scan once the player is in the world (gate calls), later on request
        // (a block moved or the setting changed while the scan is older than 30 s); each scan is
        // followed by a fresh apply, so a setting always reaches every copy
        LONG pending = InterlockedExchange(&g_pending, 0);
        uint64_t now = GetTickCount64();
        if (pending & kPendingRescan) rescanWanted = true;   // a block moved (or the game is shutting down)
        bool scanNow = g_cfg.liveData && ((!g_dataScanned && g_statGate > 0) ||
                       (g_dataScanned && ((pending & (kPendingSmash | kPendingStacks)) || rescanWanted) && now - lastScan > 30000));
        if (scanNow) {
            if (pending & kPendingSmash) ApplySmash();     // cached addresses first: no wait for the user
            if (pending & kPendingStacks) ApplyStacks();
            ScanData();
            lastScan = GetTickCount64();
            rescanWanted = false;
            pending = kPendingSmash | kPendingStacks;
        }
        if (g_dataScanned && (pending & (kPendingSmash | kPendingStacks))) {
            uint64_t t0 = GetTickCount64();
            int smash = (pending & kPendingSmash) ? ApplySmash() : 0;
            int stacks = (pending & kPendingStacks) ? ApplyStacks() : 0;
            if (g_cfg.debugLog) Log("Live-Daten: Zerschlagen %d Kopien (Radius %.1f, Baubares %d, Steine %d), Stapelgroesse %d geaendert (Wunsch %d), %llu ms, Seitenbereiche entsperrt %zu (wieder gesperrt)%s",
                                    smash, g_cfg.autoSmash ? g_cfg.radius : 0.0f, g_cfg.smashShared, g_cfg.stones, stacks, g_cfg.stackSize,
                                    (unsigned long long)(GetTickCount64() - t0), g_lastRanges,
                                    g_cfg.liveWrite ? "" : " [nur Suche]");
        }

        LONG64 q = g_statQueries;
        if (!g_cfg.debugLog || q == lastQueries || now - lastStats < 5000) continue;
        lastQueries = q;
        lastStats = now;
        Log("Magnet: Abfragen=%lld Gegenstaende=%lld Ziele=%lld AllesNehmen=%lld Uebersprungen=%lld OhneFreigabe=%lld Gedrosselt=%lld Gepluendert=%lld Geerntet=%lld Sofortpfad=%lld SofortWdh=%lld KeinAngebot=%lld AngebotGesperrt=%lld OhneHelfer=%lld Angenommen=%lld QuestBehalten=%lld Gefiltert=%lld/%lld AufhebenPause=%lld AufhebenAbgelehnt=%lld GruppeAus=%lld RadiusAus=%lld ErtragIgnoriert=%lld VorlageUnbekannt=%lld GegnerNah=%lld | Freigabe: Aufrufe=%lld E-gehalten=%lld Original-aktiv=%lld FensterPause=%lld FensterSperre=%lld Blink=%lld GegnerPause=%lld",
            q, (LONG64)g_statPickups, (LONG64)g_statCorpses, (LONG64)g_statLootAll, (LONG64)g_statSkipped, (LONG64)g_statNotReleased,
            (LONG64)g_statThrottled, (LONG64)g_statLooted, (LONG64)g_statHarvested, (LONG64)g_statPickupPath, (LONG64)g_statPickupRepeat, (LONG64)g_statNoOffer, (LONG64)g_statOfferDisabled, (LONG64)g_statNoHelpers, (LONG64)g_statAccepts,
            (LONG64)g_statQuestKept, (LONG64)g_statFiltered, (LONG64)g_statPickupFiltered, (LONG64)g_statPickupStuck, (LONG64)g_statPickupDeclined, (LONG64)g_statGroupOff, (LONG64)g_statRadiusOff,
            (LONG64)g_statYieldIgnored, (LONG64)g_statUnknownTemplate, (LONG64)g_statEnemyNear,
            (LONG64)g_statGate, (LONG64)g_statGateHeld, (LONG64)g_statGateVanilla, (LONG64)g_statGatePaused, (LONG64)g_statWindowBlocked, (LONG64)g_statGateBlink,
            (LONG64)g_statEnemyPaused);
        {
            LONG64 cq = 0, cc = 0, cg = 0, cs = 0, cl = 0, cf = 0;
            double mq = TakeMs(g_tQueryPickups, &cq), mc = TakeMs(g_tQueryCorpses, &cc), mg = TakeMs(g_tGate, &cg),
                   ms = TakeMs(g_tStub, &cs), ml = TakeMs(g_tLootAll, &cl), mf = TakeMs(g_tFilter, &cf);
            size_t recent = 0; uint64_t longest = 0;
            AcquireSRWLockShared(&g_stuckLock);
            for (const auto& t : g_stuck) {
                if (t.lastSeen == 0 || now - t.lastSeen > 5000) continue;
                ++recent;
                if (t.lastSeen > t.firstSeen && t.lastSeen - t.firstSeen > longest) longest = t.lastSeen - t.firstSeen;
            }
            ReleaseSRWLockShared(&g_stuckLock);
            LONG64 ct = 0, cr = 0, clg = 0;
            double mt = TakeMs(g_tTemplate, &ct), mr = TakeMs(g_tReleased, &cr), mlg = TakeMs(g_tLog, &clg);
            Log("Hook-Zeit je 5 s: SuchePickups=%.1f ms (%lld) SucheZiele=%.1f ms (%lld) Gate=%.1f ms (%lld) Stub=%.1f ms (%lld) LootAll=%.1f ms (%lld) Filter=%.1f ms (%lld) | darin Vorlage=%.1f ms (%lld) Freigabe=%.1f ms (%lld) Log=%.1f ms (%lld) | Pickups zuletzt 5 s: %zu verschiedene, laengstes %llu ms",
                mq, cq, mc, cc, mg, cg, ms, cs, ml, cl, mf, cf, mt, ct, mr, cr, mlg, clg, recent, (unsigned long long)longest);
        }
        char items[64 * 24] = {};
        size_t used = 0;
        AcquireSRWLockExclusive(&g_lootedLock);
        for (auto& c : g_lootedItems) {
            if (c.count == 0) break;
            used += sprintf_s(items + used, sizeof items - used, " %u×%u", c.id, c.count);
            c = { 0, 0 };
        }
        ReleaseSRWLockExclusive(&g_lootedLock);
        if (used) Log("Alles-nehmen-Gegenstände (Nummer×Anzahl):%s", items);
    }
}

void Overlay_Install();   // overlay.cpp

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        g_self = module;
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        g_gameDir = exe;
        g_gameDir = g_gameDir.substr(0, g_gameDir.find_last_of(L"\\/"));
        // only act inside the game client
        std::wstring name = exe;
        name = name.substr(name.find_last_of(L"\\/") + 1);
        if (_wcsicmp(name.c_str(), L"enshrouded.exe") == 0) {
            Overlay_Install();   // must run before the game loads Vulkan
            LoadChain();         // after our hooks, so a chained overlay mod finds and keeps them
            CloseHandle(CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr));
        }
    }
    return TRUE;
}
