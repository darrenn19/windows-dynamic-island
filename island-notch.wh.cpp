// ==WindhawkMod==
// @id              island-notch
// @name            Island (notch)
// @description     A click-only notch at the top of the screen: notifications, music with lyrics, timers, shelf, clipboard and notes
// @version         0.1.0
// @author          Darren Nadarajah
// @include         windhawk.exe
// @compilerOptions -lgdi32 -luser32 -ld2d1 -ldwrite -lwindowscodecs -lole32 -loleaut32 -luuid -lshell32 -lshlwapi -lshcore -lwinhttp -lwbemuuid -ldwmapi -lwindowsapp -lversion
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Island

A notch at the top centre of your main screen. Nothing opens on hover: **click** it.

| Where | Left click | Right click |
|---|---|---|
| The bar | full view | half view |
| Half view | full view on that item | close |
| Full view | the buttons | half view |
| A pop-up | Notifications tab | dismiss the pop-up |

- **Alt+Z** steps closed → half → full → closed. **Alt+Shift+Z** locks it see-through.
- **Hold Alt** to see (and click) through it. **Shake** the pointer over the half view to
  toggle see-through; it comes back a quarter-second after you move away.
- Full view tabs: Notifications, Music (with lyrics), Clock (timer, alarms, stopwatch),
  Shelf (drop files on the island), Clipboard, Notes.

Notes
- Runs in its own process (a Windhawk "tool mod"), so it can't take Explorer down.
- Notifications are read from Windows' own notification database; Windows still shows its
  pop-ups too (turn banners off per app in Windows Settings if you only want the island's).
- Music apps don't give lyrics to Windows. When "Look up lyrics" is on, the song's title,
  artist and length are sent to lrclib.net, which returns timed lyrics, plain lyrics or none.
- The lock screen is off-limits to all apps; the verification animation plays for
  Windows Security prompts on your desktop.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- idleStyle: line
  $name: When nothing is happening
  $options:
  - line: Show the bar
  - hidden: Hide it (click the top centre to open)
- popups: true
  $name: Show new notifications as pop-ups
- levelPopup: true
  $name: Show volume and brightness changes
- autoHalf: true
  $name: Open the half view when a timer starts
- lyrics: true
  $name: Look up lyrics (lrclib.net)
- verifyAnimation: true
  $name: Animate Windows Security verification prompts
- shortcuts: true
  $name: Shortcuts (Alt+key steps the views, Alt+Shift+key locks see-through)
- shortcutKey: Z
  $name: Shortcut letter
- holdKey: alt
  $name: Hold to see through
  $options:
  - alt: Alt
  - ctrl: Ctrl
  - shift: Shift
  - none: Off
- shake: true
  $name: Shake the half view to see through
- cornerRadius: 20
  $name: Corner radius
- animationSpeed: 100
  $name: Animation speed (%)
  $description: 100 is normal; higher is faster; 0 turns animations off
- rules:
  - - app: ""
      $name: App (part of its name)
    - mode: quiet
      $name: Notifications
      $options:
      - popup: Pop up
      - quiet: Quiet (list only)
      - off: Off
  $name: Per-app notification rules
- dndAllow:
  - Clock
  - Alarms
  $name: Apps allowed during Do not disturb (part of their name)
- alarms:
  - - time: "07:00"
      $name: Time (HH:MM, 24-hour)
    - label: Wake up
      $name: Label
    - enabled: true
      $name: On
  $name: Alarms
*/
// ==/WindhawkModSettings==

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wincodec.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <winhttp.h>
#include <wbemidl.h>
#include <oleidl.h>

#undef GetCurrentTime  // clashes with C++/WinRT
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using std::wstring;

// ============================================================================
// Small helpers
// ============================================================================

template <class T>
static void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

static ULONGLONG NowMs() { return GetTickCount64(); }

static double QpcSeconds() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(freq.QuadPart);
}

static wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

static std::string WideToUtf8(const wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static wstring ToLower(wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
    return s;
}

static wstring FmtClock(long long totalSec) {
    if (totalSec < 0) totalSec = 0;
    long long h = totalSec / 3600, m = (totalSec / 60) % 60, s = totalSec % 60;
    wchar_t b[32];
    if (h) swprintf_s(b, L"%lld:%02lld:%02lld", h, m, s);
    else swprintf_s(b, L"%lld:%02lld", m, s);
    return b;
}

static wstring FmtStopwatch(long long ms) {
    long long m = ms / 60000, s = (ms / 1000) % 60, cs = (ms / 10) % 100;
    wchar_t b[32];
    swprintf_s(b, L"%lld:%02lld.%02lld", m, s, cs);
    return b;
}

static wstring FmtTimeOfDay(const SYSTEMTIME& st) {
    wchar_t b[64] = L"";
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, b, 64);
    return b;
}

// Critically damped spring (the prototype's SpringTowards). Returns true while moving.
struct Spring {
    double v = 0, vel = 0;
};
static double g_speedScale = 1.0;  // from the animation speed setting; 0 = no animation

static bool SpringStep(Spring& s, double target, double seconds, double dt, double damping = 1.0, double tol = 0.05) {
    if (g_speedScale <= 0.0) {
        s.v = target;
        s.vel = 0;
        return false;
    }
    seconds /= g_speedScale;
    double w = 8.0 / seconds, step = 1.0 / 480.0;
    for (double rem = dt; rem > 0; rem -= step) {
        double h = std::min(rem, step);
        double a = w * w * (target - s.v) - 2 * damping * w * s.vel;
        s.vel += a * h;
        s.v += s.vel * h;
    }
    if (std::fabs(target - s.v) < tol && std::fabs(s.vel) < tol * 10) {
        s.v = target;
        s.vel = 0;
        return false;
    }
    return true;
}

// ============================================================================
// Settings
// ============================================================================

struct Rule {
    wstring app;   // lower-case substring
    int mode = 0;  // 0 popup, 1 quiet, 2 off
};
struct AlarmCfg {
    int hour = 7, minute = 0;
    wstring label;
    bool enabled = true;
};

struct Settings {
    bool idleHidden = false;
    bool popups = true;
    bool levelPopup = true;
    bool autoHalf = true;
    bool lyrics = true;
    bool verifyAnimation = true;
    bool shortcuts = true;
    wchar_t shortcutKey = L'Z';
    int holdKey = VK_MENU;  // 0 = off
    bool shake = true;
    double radius = 20;
    std::vector<Rule> rules;
    std::vector<wstring> dndAllow;
    std::vector<AlarmCfg> alarms;
} g_set;

static wstring GetStr(PCWSTR name) {
    PCWSTR v = Wh_GetStringSetting(name);
    wstring r = v ? v : L"";
    if (v) Wh_FreeStringSetting(v);
    return r;
}

static void LoadSettings() {
    Settings s;
    s.idleHidden = GetStr(L"idleStyle") == L"hidden";
    s.popups = Wh_GetIntSetting(L"popups") != 0;
    s.levelPopup = Wh_GetIntSetting(L"levelPopup") != 0;
    s.autoHalf = Wh_GetIntSetting(L"autoHalf") != 0;
    s.lyrics = Wh_GetIntSetting(L"lyrics") != 0;
    s.verifyAnimation = Wh_GetIntSetting(L"verifyAnimation") != 0;
    s.shortcuts = Wh_GetIntSetting(L"shortcuts") != 0;
    wstring key = GetStr(L"shortcutKey");
    if (!key.empty() && iswalnum(key[0])) s.shortcutKey = (wchar_t)towupper(key[0]);
    wstring hk = GetStr(L"holdKey");
    s.holdKey = hk == L"ctrl" ? VK_CONTROL : hk == L"shift" ? VK_SHIFT : hk == L"none" ? 0 : VK_MENU;
    s.shake = Wh_GetIntSetting(L"shake") != 0;
    int r = Wh_GetIntSetting(L"cornerRadius");
    s.radius = std::clamp(r, 4, 30);
    int sp = Wh_GetIntSetting(L"animationSpeed");
    g_speedScale = sp <= 0 ? 0.0 : std::clamp(sp, 25, 400) / 100.0;

    for (int i = 0; i < 64; i++) {
        wchar_t n[64];
        swprintf_s(n, L"rules[%d].app", i);
        PCWSTR app = Wh_GetStringSetting(n);
        if (!app) break;
        wstring a = app;
        Wh_FreeStringSetting(app);
        if (a.empty()) continue;
        swprintf_s(n, L"rules[%d].mode", i);
        wstring m = GetStr(n);
        s.rules.push_back({ToLower(a), m == L"off" ? 2 : m == L"quiet" ? 1 : 0});
    }
    for (int i = 0; i < 64; i++) {
        wchar_t n[64];
        swprintf_s(n, L"dndAllow[%d]", i);
        PCWSTR app = Wh_GetStringSetting(n);
        if (!app) break;
        wstring a = app;
        Wh_FreeStringSetting(app);
        if (!a.empty()) s.dndAllow.push_back(ToLower(a));
    }
    for (int i = 0; i < 32; i++) {
        wchar_t n[64];
        swprintf_s(n, L"alarms[%d].time", i);
        PCWSTR t = Wh_GetStringSetting(n);
        if (!t) break;
        wstring ts = t;
        Wh_FreeStringSetting(t);
        int hh = -1, mm = -1;
        if (swscanf_s(ts.c_str(), L"%d:%d", &hh, &mm) != 2 || hh < 0 || hh > 23 || mm < 0 || mm > 59) continue;
        AlarmCfg a;
        a.hour = hh;
        a.minute = mm;
        swprintf_s(n, L"alarms[%d].label", i);
        a.label = GetStr(n);
        swprintf_s(n, L"alarms[%d].enabled", i);
        a.enabled = Wh_GetIntSetting(n) != 0;
        s.alarms.push_back(a);
    }
    g_set = std::move(s);
}

// Per-app rule for a notification's app name: 0 popup, 1 quiet, 2 off.
static int RuleFor(const wstring& app) {
    wstring a = ToLower(app);
    for (auto& r : g_set.rules)
        if (a.find(r.app) != wstring::npos) return r.mode;
    return 0;
}
static bool AllowedDuringDnd(const wstring& app) {
    wstring a = ToLower(app);
    for (auto& d : g_set.dndAllow)
        if (a.find(d) != wstring::npos) return true;
    return false;
}

// Persistent values (Windhawk's per-mod storage).
static wstring LoadValue(PCWSTR key) {
    std::vector<wchar_t> buf(65536);
    size_t n = Wh_GetStringValue(key, buf.data(), buf.size());
    return n ? wstring(buf.data()) : L"";
}
static void SaveValue(PCWSTR key, const wstring& v) { Wh_SetStringValue(key, v.c_str()); }

// ============================================================================
// Shared state between the worker threads and the UI thread
// ============================================================================

#define WM_ISLAND_NOTIF (WM_APP + 1)   // new notifications are waiting in g_incoming
#define WM_ISLAND_MEDIA (WM_APP + 2)   // media state changed
#define WM_ISLAND_LEVEL (WM_APP + 3)   // wParam 0 volume, 1 brightness; lParam level | muted << 16
#define WM_ISLAND_REBIND (WM_APP + 4)  // default audio device changed

static HWND g_hwnd = nullptr;
static std::atomic<bool> g_stop{false};
static std::mutex g_mx;  // guards g_incoming, g_media, g_icons

struct Pixels {  // premultiplied BGRA, top-down
    int w = 0, h = 0;
    std::vector<BYTE> data;
};

struct Notif {
    long long id = 0;
    wstring aumid, app, title, body;
    SYSTEMTIME at{};
};
static std::deque<Notif> g_incoming;

struct LyricLine {
    int ms = -1;  // -1 when not timed
    wstring text;
};

struct Media {
    bool has = false;
    wstring title, artist, album, aumid, app;
    bool playing = false;
    long long posMs = 0, durMs = 0;
    double sampledAt = 0;  // QpcSeconds() when posMs was read
    Pixels art;
    unsigned artGen = 0;
    int lyrState = 0;  // 0 looking, 1 timed, 2 plain, 3 none, 4 lookup off
    std::vector<LyricLine> lyrics;
    unsigned lyrGen = 0;
} g_media;

// App names and icons by AppUserModelID (filled by workers, read by the UI).
struct AppInfo {
    wstring name;
    Pixels icon;
};
static std::map<wstring, AppInfo> g_apps;

static void PostToUi(UINT msg, WPARAM w = 0, LPARAM l = 0) {
    if (g_hwnd) PostMessageW(g_hwnd, msg, w, l);
}

// ---------------------------------------------------------------------------
// App names and icons from an AppUserModelID (works for Store apps and most
// desktop apps; falls back to the exe name).
// ---------------------------------------------------------------------------

static wstring FallbackAppName(const wstring& aumid) {
    wstring s = aumid;
    size_t p = s.find_last_of(L"\\/");
    if (p != wstring::npos) s = s.substr(p + 1);
    p = s.find(L'!');
    if (p != wstring::npos) s = s.substr(0, p);
    p = s.find(L'_');
    if (p != wstring::npos) s = s.substr(0, p);
    if (s.size() > 4 && ToLower(s.substr(s.size() - 4)) == L".exe") s = s.substr(0, s.size() - 4);
    p = s.find_last_of(L'.');
    if (p != wstring::npos && p + 1 < s.size()) s = s.substr(p + 1);  // "Microsoft.WindowsTerminal" -> "WindowsTerminal"
    if (!s.empty()) s[0] = (wchar_t)towupper(s[0]);
    return s.empty() ? L"App" : s;
}

static Pixels PixelsFromHBitmap(HBITMAP hbm) {
    Pixels px;
    BITMAP bm{};
    if (!GetObjectW(hbm, sizeof(bm), &bm)) return px;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = bm.bmWidth;
    bi.bmiHeader.biHeight = -bm.bmHeight;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    px.w = bm.bmWidth;
    px.h = bm.bmHeight;
    px.data.resize(size_t(px.w) * px.h * 4);
    HDC dc = GetDC(nullptr);
    GetDIBits(dc, hbm, 0, px.h, px.data.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    bool anyAlpha = false;
    for (size_t i = 3; i < px.data.size(); i += 4)
        if (px.data[i]) { anyAlpha = true; break; }
    if (!anyAlpha)
        for (size_t i = 3; i < px.data.size(); i += 4) px.data[i] = 255;
    return px;
}

// Resolve name + icon for an AUMID once (COM must be initialised on the calling thread).
static void ResolveApp(const wstring& aumid) {
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (g_apps.count(aumid)) return;
    }
    AppInfo info;
    info.name = FallbackAppName(aumid);
    IShellItem* item = nullptr;
    if (SUCCEEDED(SHCreateItemInKnownFolder(FOLDERID_AppsFolder, 0, aumid.c_str(), IID_PPV_ARGS(&item)))) {
        PWSTR dn = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &dn)) && dn) {
            info.name = dn;
            CoTaskMemFree(dn);
        }
        IShellItemImageFactory* f = nullptr;
        if (SUCCEEDED(item->QueryInterface(IID_PPV_ARGS(&f)))) {
            HBITMAP hbm = nullptr;
            if (SUCCEEDED(f->GetImage({48, 48}, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hbm)) && hbm) {
                info.icon = PixelsFromHBitmap(hbm);
                DeleteObject(hbm);
            }
            f->Release();
        }
        item->Release();
    }
    std::lock_guard<std::mutex> lk(g_mx);
    g_apps[aumid] = std::move(info);
}

static wstring AppNameUnlocked(const wstring& aumid) {  // caller holds g_mx
    auto it = g_apps.find(aumid);
    return it != g_apps.end() ? it->second.name : FallbackAppName(aumid);
}

static wstring AppName(const wstring& aumid) {
    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_apps.find(aumid);
    return it != g_apps.end() ? it->second.name : FallbackAppName(aumid);
}

// ============================================================================
// Notifications: read new toasts from Windows' notification database
// (%LOCALAPPDATA%\Microsoft\Windows\Notifications\wpndatabase.db) with the
// system copy of SQLite (winsqlite3.dll). No special permission needed.
// ============================================================================

namespace sql {
typedef int(__stdcall* open_v2_t)(const char*, void**, int, const char*);
typedef int(__stdcall* prepare_v2_t)(void*, const char*, int, void**, const char**);
typedef int(__stdcall* step_t)(void*);
typedef long long(__stdcall* col_int64_t)(void*, int);
typedef const void*(__stdcall* col_blob_t)(void*, int);
typedef int(__stdcall* col_bytes_t)(void*, int);
typedef const unsigned char*(__stdcall* col_text_t)(void*, int);
typedef int(__stdcall* bind_int64_t)(void*, int, long long);
typedef int(__stdcall* reset_t)(void*);
typedef int(__stdcall* finalize_t)(void*);
typedef int(__stdcall* close_t)(void*);
typedef int(__stdcall* busy_timeout_t)(void*, int);
open_v2_t open_v2;
prepare_v2_t prepare_v2;
step_t step;
col_int64_t col_int64;
col_blob_t col_blob;
col_bytes_t col_bytes;
col_text_t col_text;
bind_int64_t bind_int64;
reset_t reset;
finalize_t finalize;
close_t close;
busy_timeout_t busy_timeout;
const int ROW = 100;
const int OPEN_READONLY = 0x00000001;

bool Load() {
    HMODULE m = LoadLibraryExW(L"winsqlite3.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) return false;
#define SQLF(var, name) var = (decltype(var))GetProcAddress(m, name)
    SQLF(open_v2, "sqlite3_open_v2");
    SQLF(prepare_v2, "sqlite3_prepare_v2");
    SQLF(step, "sqlite3_step");
    SQLF(col_int64, "sqlite3_column_int64");
    SQLF(col_blob, "sqlite3_column_blob");
    SQLF(col_bytes, "sqlite3_column_bytes");
    SQLF(col_text, "sqlite3_column_text");
    SQLF(bind_int64, "sqlite3_bind_int64");
    SQLF(reset, "sqlite3_reset");
    SQLF(finalize, "sqlite3_finalize");
    SQLF(close, "sqlite3_close");
    SQLF(busy_timeout, "sqlite3_busy_timeout");
#undef SQLF
    return open_v2 && prepare_v2 && step && col_int64 && col_blob && col_bytes && col_text && bind_int64 && reset &&
           finalize && close && busy_timeout;
}
}  // namespace sql

static wstring XmlUnescape(const wstring& s) {
    wstring o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != L'&') { o += s[i]; continue; }
        size_t e = s.find(L';', i);
        if (e == wstring::npos || e - i > 10) { o += s[i]; continue; }
        wstring ent = s.substr(i + 1, e - i - 1);
        if (ent == L"amp") o += L'&';
        else if (ent == L"lt") o += L'<';
        else if (ent == L"gt") o += L'>';
        else if (ent == L"quot") o += L'"';
        else if (ent == L"apos") o += L'\'';
        else if (!ent.empty() && ent[0] == L'#') {
            unsigned long cp = ent.size() > 1 && (ent[1] == L'x' || ent[1] == L'X') ? wcstoul(ent.c_str() + 2, nullptr, 16)
                                                                                       : wcstoul(ent.c_str() + 1, nullptr, 10);
            if (cp < 0x10000) o += (wchar_t)cp;
            else if (cp <= 0x10FFFF) {
                cp -= 0x10000;
                o += (wchar_t)(0xD800 + (cp >> 10));
                o += (wchar_t)(0xDC00 + (cp & 0x3FF));
            }
        } else { o += s.substr(i, e - i + 1); }
        i = e;
    }
    return o;
}

// Pull the <text> elements out of a toast's XML payload.
static std::vector<wstring> ToastTexts(const wstring& xml) {
    std::vector<wstring> out;
    size_t p = 0;
    while ((p = xml.find(L"<text", p)) != wstring::npos) {
        size_t gt = xml.find(L'>', p);
        if (gt == wstring::npos) break;
        if (xml[gt - 1] == L'/') { p = gt; continue; }  // <text/>
        size_t end = xml.find(L"</text>", gt);
        if (end == wstring::npos) break;
        wstring t = XmlUnescape(xml.substr(gt + 1, end - gt - 1));
        while (!t.empty() && iswspace(t.back())) t.pop_back();
        if (!t.empty()) out.push_back(t);
        p = end + 7;
    }
    return out;
}

static void NotificationThread() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (!sql::Load()) {
        Wh_Log(L"winsqlite3.dll not available: notifications are off");
        CoUninitialize();
        return;
    }
    wchar_t local[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) { CoUninitialize(); return; }
    std::string path = WideToUtf8(wstring(local) + L"\\Microsoft\\Windows\\Notifications\\wpndatabase.db");

    void* db = nullptr;
    void* qMax = nullptr;
    void* qNew = nullptr;
    long long lastId = -1;
    while (!g_stop) {
        if (!db) {
            if (sql::open_v2(path.c_str(), &db, sql::OPEN_READONLY, nullptr) != 0) {
                if (db) sql::close(db);
                db = nullptr;
                Sleep(3000);
                continue;
            }
            sql::busy_timeout(db, 250);
            sql::prepare_v2(db, "SELECT MAX(Id) FROM Notification", -1, &qMax, nullptr);
            sql::prepare_v2(db,
                            "SELECT n.Id, n.Payload, h.PrimaryId FROM Notification n "
                            "JOIN NotificationHandler h ON n.HandlerId = h.RecordId "
                            "WHERE n.Type = 'toast' AND n.Id > ? ORDER BY n.Id ASC LIMIT 20",
                            -1, &qNew, nullptr);
            if (!qMax || !qNew) {
                Wh_Log(L"Notification database has an unexpected layout");
                break;
            }
        }
        if (lastId < 0) {  // start after whatever is already there
            if (sql::step(qMax) == sql::ROW) lastId = sql::col_int64(qMax, 0);
            sql::reset(qMax);
            if (lastId < 0) lastId = 0;
        }
        sql::bind_int64(qNew, 1, lastId);
        std::vector<Notif> found;
        while (sql::step(qNew) == sql::ROW) {
            Notif n;
            n.id = sql::col_int64(qNew, 0);
            lastId = std::max(lastId, n.id);
            const char* blob = (const char*)sql::col_blob(qNew, 1);
            int len = sql::col_bytes(qNew, 1);
            const unsigned char* aumid = sql::col_text(qNew, 2);
            if (!blob || len <= 0) continue;
            auto texts = ToastTexts(Utf8ToWide(std::string(blob, len)));
            if (texts.empty()) continue;
            n.aumid = aumid ? Utf8ToWide((const char*)aumid) : L"";
            n.title = texts[0];
            for (size_t i = 1; i < texts.size(); i++) n.body += (i > 1 ? L" · " : L"") + texts[i];
            GetLocalTime(&n.at);
            found.push_back(std::move(n));
        }
        sql::reset(qNew);
        if (!found.empty()) {
            for (auto& n : found) {
                ResolveApp(n.aumid);
                n.app = AppName(n.aumid);
            }
            {
                std::lock_guard<std::mutex> lk(g_mx);
                for (auto& n : found) g_incoming.push_back(std::move(n));
            }
            PostToUi(WM_ISLAND_NOTIF);
        }
        for (int i = 0; i < 8 && !g_stop; i++) Sleep(100);
    }
    if (qMax) sql::finalize(qMax);
    if (qNew) sql::finalize(qNew);
    if (db) sql::close(db);
    CoUninitialize();
}

// ============================================================================
// Lyrics: lrclib.net (timed "syncedLyrics", or "plainLyrics", or nothing)
// ============================================================================

static std::string UrlEncode(const wstring& w) {
    std::string s = WideToUtf8(w), o;
    char hex[4];
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else {
            sprintf_s(hex, "%%%02X", c);
            o += hex;
        }
    }
    return o;
}

static bool HttpGet(const wstring& host, const std::string& pathUtf8, std::string& body, DWORD& status) {
    status = 0;
    HINTERNET ses = WinHttpOpen(L"Island-notch/0.1 (Windhawk mod)", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return false;
    WinHttpSetTimeouts(ses, 4000, 4000, 5000, 6000);
    bool ok = false;
    HINTERNET con = WinHttpConnect(ses, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (con) {
        wstring wpath = Utf8ToWide(pathUtf8);
        HINTERNET req = WinHttpOpenRequest(con, L"GET", wpath.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           WINHTTP_FLAG_SECURE);
        if (req) {
            if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(req, nullptr)) {
                DWORD sz = sizeof(status);
                WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                                    &sz, WINHTTP_NO_HEADER_INDEX);
                DWORD avail = 0;
                while (WinHttpQueryDataAvailable(req, &avail) && avail) {
                    std::string chunk(avail, '\0');
                    DWORD read = 0;
                    if (!WinHttpReadData(req, chunk.data(), avail, &read) || !read) break;
                    body.append(chunk.data(), read);
                    if (body.size() > 512 * 1024) break;
                }
                ok = true;
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(con);
    }
    WinHttpCloseHandle(ses);
    return ok;
}

static void AppendUtf8(std::string& s, unsigned cp) {
    if (cp < 0x80) s += (char)cp;
    else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
    else { s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3F)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
}

// Read the JSON string value of "key" (first occurrence). Returns false for null/missing/empty.
static bool JsonString(const std::string& js, const char* key, wstring& out) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = js.find(k);
    if (p == std::string::npos) return false;
    p = js.find(':', p + k.size());
    if (p == std::string::npos) return false;
    p++;
    while (p < js.size() && isspace((unsigned char)js[p])) p++;
    if (p >= js.size() || js[p] != '"') return false;  // null or not a string
    std::string s;
    for (p++; p < js.size() && js[p] != '"'; p++) {
        char c = js[p];
        if (c != '\\') { s += c; continue; }
        if (++p >= js.size()) break;
        char e = js[p];
        if (e == 'n') s += '\n';
        else if (e == 't') s += '\t';
        else if (e == 'r') {}
        else if (e == 'b' || e == 'f') {}
        else if (e == 'u' && p + 4 < js.size()) {
            unsigned cp = strtoul(js.substr(p + 1, 4).c_str(), nullptr, 16);
            p += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && p + 6 < js.size() && js[p + 1] == '\\' && js[p + 2] == 'u') {
                unsigned lo = strtoul(js.substr(p + 3, 4).c_str(), nullptr, 16);
                if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); p += 6; }
            }
            AppendUtf8(s, cp);
        } else s += e;
    }
    out = Utf8ToWide(s);
    return !out.empty();
}

static std::vector<LyricLine> ParseLrc(const wstring& lrc) {
    std::vector<LyricLine> out;
    size_t start = 0;
    while (start <= lrc.size()) {
        size_t end = lrc.find(L'\n', start);
        wstring line = lrc.substr(start, end == wstring::npos ? wstring::npos : end - start);
        std::vector<int> times;
        size_t p = 0;
        while (p < line.size() && line[p] == L'[') {
            size_t c = line.find(L']', p);
            if (c == wstring::npos) break;
            int mm = 0;
            double ss = 0;
            if (swscanf_s(line.substr(p + 1, c - p - 1).c_str(), L"%d:%lf", &mm, &ss) == 2) times.push_back(int(mm * 60000 + ss * 1000));
            p = c + 1;
        }
        wstring text = line.substr(p);
        while (!text.empty() && iswspace(text.front())) text.erase(text.begin());
        for (int t : times) out.push_back({t, text});
        if (end == wstring::npos) break;
        start = end + 1;
    }
    std::sort(out.begin(), out.end(), [](const LyricLine& a, const LyricLine& b) { return a.ms < b.ms; });
    return out;
}

static void FetchLyrics(const wstring& title, const wstring& artist, const wstring& album, long long durMs, unsigned gen) {
    int state = 3;
    std::vector<LyricLine> lines;
    std::string body;
    DWORD status = 0;
    std::string path = "/api/get?track_name=" + UrlEncode(title) + "&artist_name=" + UrlEncode(artist);
    if (!album.empty()) path += "&album_name=" + UrlEncode(album);
    if (durMs > 0) path += "&duration=" + std::to_string((durMs + 500) / 1000);
    bool got = HttpGet(L"lrclib.net", path, body, status) && status == 200;
    if (!got) {  // looser search: first result
        body.clear();
        got = HttpGet(L"lrclib.net", "/api/search?track_name=" + UrlEncode(title) + "&artist_name=" + UrlEncode(artist), body, status) &&
              status == 200 && body.size() > 2;
    }
    if (got) {
        wstring synced, plain;
        if (JsonString(body, "syncedLyrics", synced)) {
            lines = ParseLrc(synced);
            if (!lines.empty()) state = 1;
        }
        if (state != 1 && JsonString(body, "plainLyrics", plain)) {
            size_t s = 0;
            while (s <= plain.size()) {
                size_t e = plain.find(L'\n', s);
                lines.push_back({-1, plain.substr(s, e == wstring::npos ? wstring::npos : e - s)});
                if (e == wstring::npos) break;
                s = e + 1;
            }
            state = lines.empty() ? 3 : 2;
        }
    }
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_media.lyrGen != gen) return;  // song changed meanwhile
    g_media.lyrics = std::move(lines);
    g_media.lyrState = state;
    PostToUi(WM_ISLAND_MEDIA);
}

// ============================================================================
// Media: Windows' media sessions (whatever app is playing)
// ============================================================================

enum class MediaCmd { PlayPause, Next, Prev, Seek };
struct MediaCommand {
    MediaCmd cmd;
    long long ms = 0;
};
static std::mutex g_cmdMx;
static std::vector<MediaCommand> g_cmds;
static HANDLE g_mediaWake = nullptr;

static void SendMediaCommand(MediaCmd c, long long ms = 0) {
    {
        std::lock_guard<std::mutex> lk(g_cmdMx);
        g_cmds.push_back({c, ms});
    }
    if (g_mediaWake) SetEvent(g_mediaWake);
}

static Pixels DecodeArt(IStream* stm) {
    Pixels px;
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return px;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICFormatConverter* conv = nullptr;
    if (SUCCEEDED(wic->CreateDecoderFromStream(stm, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame))) {
        UINT w = 0, h = 0;
        frame->GetSize(&w, &h);
        UINT side = 96;
        if (w && h && SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(frame, side, side, WICBitmapInterpolationModeFant)) && SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(scaler, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
            px.w = px.h = (int)side;
            px.data.resize(side * side * 4);
            if (FAILED(conv->CopyPixels(nullptr, side * 4, (UINT)px.data.size(), px.data.data()))) px = Pixels{};
        }
    }
    SafeRelease(conv);
    SafeRelease(scaler);
    SafeRelease(frame);
    SafeRelease(dec);
    SafeRelease(wic);
    return px;
}

static void MediaThread() {
    using namespace winrt::Windows::Media::Control;
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    GlobalSystemMediaTransportControlsSessionManager mgr{nullptr};
    wstring lastKey;
    unsigned lyrGen = 0;
    while (!g_stop) {
        try {
            if (!mgr) mgr = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
            auto s = mgr.GetCurrentSession();
            std::vector<MediaCommand> cmds;
            {
                std::lock_guard<std::mutex> lk(g_cmdMx);
                cmds.swap(g_cmds);
            }
            if (s) {
                for (auto& c : cmds) {
                    if (c.cmd == MediaCmd::PlayPause) s.TryTogglePlayPauseAsync().get();
                    else if (c.cmd == MediaCmd::Next) s.TrySkipNextAsync().get();
                    else if (c.cmd == MediaCmd::Prev) s.TrySkipPreviousAsync().get();
                    else if (c.cmd == MediaCmd::Seek) s.TryChangePlaybackPositionAsync(c.ms * 10000).get();
                }
            }
            Media m;
            if (s) {
                auto props = s.TryGetMediaPropertiesAsync().get();
                auto pb = s.GetPlaybackInfo();
                auto tl = s.GetTimelineProperties();
                m.has = true;
                m.title = props.Title().c_str();
                m.artist = props.Artist().c_str();
                m.album = props.AlbumTitle().c_str();
                m.aumid = s.SourceAppUserModelId().c_str();
                m.playing = pb.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
                long long pos = tl.Position().count() / 10000;
                long long end = tl.EndTime().count() / 10000;
                if (m.playing) {
                    auto since = winrt::clock::now() - tl.LastUpdatedTime();
                    long long add = std::chrono::duration_cast<std::chrono::milliseconds>(since).count();
                    if (add > 0 && add < 24LL * 3600 * 1000) pos += add;
                }
                m.durMs = std::max(0LL, end);
                m.posMs = std::clamp(pos, 0LL, m.durMs > 0 ? m.durMs : pos);
                m.sampledAt = QpcSeconds();
                if (m.title.empty()) m.has = false;

                wstring key = m.title + L"\x1f" + m.artist + L"\x1f" + m.aumid;
                if (m.has && key != lastKey) {
                    lastKey = key;
                    ResolveApp(m.aumid);
                    Pixels art;
                    try {
                        if (auto th = props.Thumbnail()) {
                            auto stream = th.OpenReadAsync().get();
                            // shcore's CreateStreamOverRandomAccessStream, looked up at run time.
                            using CSORAS = HRESULT(WINAPI*)(::IUnknown*, REFIID, void**);
                            static CSORAS createStream = (CSORAS)GetProcAddress(LoadLibraryW(L"shcore.dll"), "CreateStreamOverRandomAccessStream");
                            IStream* stm = nullptr;
                            if (createStream && SUCCEEDED(createStream(reinterpret_cast<::IUnknown*>(winrt::get_abi(stream)), IID_PPV_ARGS(&stm)))) {
                                art = DecodeArt(stm);
                                stm->Release();
                            }
                        }
                    } catch (...) {
                    }
                    unsigned gen;
                    {
                        std::lock_guard<std::mutex> lk(g_mx);
                        g_media.art = std::move(art);
                        g_media.artGen++;
                        g_media.lyrics.clear();
                        g_media.lyrState = g_set.lyrics ? 0 : 4;
                        gen = ++g_media.lyrGen;
                    }
                    lyrGen = gen;
                    if (g_set.lyrics) {
                        // Off the media loop so controls stay responsive while it looks.
                        wstring t = m.title, a = m.artist, al = m.album;
                        long long d = m.durMs;
                        std::thread([t, a, al, d, gen] { FetchLyrics(t, a, al, d, gen); }).detach();
                    }
                }
            }
            {
                std::lock_guard<std::mutex> lk(g_mx);
                bool changed = g_media.has != m.has || g_media.title != m.title || g_media.playing != m.playing ||
                               std::llabs(g_media.posMs - m.posMs) > 1500 || g_media.durMs != m.durMs;
                Pixels art = std::move(g_media.art);
                auto lyr = std::move(g_media.lyrics);
                int ls = g_media.lyrState;
                unsigned ag = g_media.artGen, lg = g_media.lyrGen;
                g_media = std::move(m);
                g_media.art = std::move(art);
                g_media.lyrics = std::move(lyr);
                g_media.lyrState = ls;
                g_media.artGen = ag;
                g_media.lyrGen = lg;
                if (!g_media.has) lastKey.clear();
                g_media.app = AppNameUnlocked(g_media.aumid);
                if (changed || !cmds.empty()) PostToUi(WM_ISLAND_MEDIA);
            }
        } catch (...) {
            mgr = nullptr;
        }
        WaitForSingleObject(g_mediaWake, 700);
    }
    (void)lyrGen;
    winrt::uninit_apartment();
}

// ============================================================================
// Volume (master endpoint) and brightness (WMI events)
// ============================================================================

class VolumeCallback : public IAudioEndpointVolumeCallback {
    LONG ref = 1;

   public:
    virtual ~VolumeCallback() = default;
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&ref);
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** pp) override {
        if (iid == IID_IUnknown || iid == __uuidof(IAudioEndpointVolumeCallback)) {
            *pp = static_cast<IAudioEndpointVolumeCallback*>(this);
            AddRef();
            return S_OK;
        }
        *pp = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA d) override {
        int lvl = (int)std::lround(d->fMasterVolume * 100);
        PostToUi(WM_ISLAND_LEVEL, 0, lvl | (d->bMuted ? 1 << 16 : 0));
        return S_OK;
    }
};

class DeviceCallback : public IMMNotificationClient {
    LONG ref = 1;

   public:
    virtual ~DeviceCallback() = default;
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&ref);
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** pp) override {
        if (iid == IID_IUnknown || iid == __uuidof(IMMNotificationClient)) {
            *pp = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *pp = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) PostToUi(WM_ISLAND_REBIND);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
};

static IMMDeviceEnumerator* g_devEnum = nullptr;
static IAudioEndpointVolume* g_epVol = nullptr;
static VolumeCallback* g_volCb = nullptr;
static DeviceCallback* g_devCb = nullptr;

static void UnbindVolume() {
    if (g_epVol && g_volCb) g_epVol->UnregisterControlChangeNotify(g_volCb);
    SafeRelease(g_epVol);
}

static void BindVolume() {  // UI thread (COM initialised there)
    UnbindVolume();
    if (!g_devEnum) {
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&g_devEnum)))) return;
        g_devCb = new DeviceCallback();
        g_devEnum->RegisterEndpointNotificationCallback(g_devCb);
    }
    IMMDevice* dev = nullptr;
    if (SUCCEEDED(g_devEnum->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) {
        dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&g_epVol);
        dev->Release();
    }
    if (g_epVol) {
        if (!g_volCb) g_volCb = new VolumeCallback();
        g_epVol->RegisterControlChangeNotify(g_volCb);
    }
}

static void ShutdownVolume() {
    UnbindVolume();
    if (g_devEnum && g_devCb) g_devEnum->UnregisterEndpointNotificationCallback(g_devCb);
    SafeRelease(g_devEnum);
    if (g_volCb) { g_volCb->Release(); g_volCb = nullptr; }
    if (g_devCb) { g_devCb->Release(); g_devCb = nullptr; }
}

static int GetVolume(bool* muted) {
    float f = 0;
    BOOL m = FALSE;
    if (g_epVol) {
        g_epVol->GetMasterVolumeLevelScalar(&f);
        g_epVol->GetMute(&m);
    }
    if (muted) *muted = m;
    return (int)std::lround(f * 100);
}
static void SetVolume(int v) {
    if (g_epVol) g_epVol->SetMasterVolumeLevelScalar(std::clamp(v, 0, 100) / 100.0f, nullptr);
}

static void BrightnessThread() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWbemLocator* loc = nullptr;
    IWbemServices* svc = nullptr;
    IEnumWbemClassObject* en = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&loc)))) {
        BSTR ns = SysAllocString(L"ROOT\\WMI");
        if (SUCCEEDED(loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc))) {
            CoSetProxyBlanket(svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr,
                              EOAC_NONE);
            BSTR lang = SysAllocString(L"WQL");
            BSTR q = SysAllocString(L"SELECT * FROM WmiMonitorBrightnessEvent");
            if (FAILED(svc->ExecNotificationQuery(lang, q, WBEM_FLAG_RETURN_IMMEDIATELY | WBEM_FLAG_FORWARD_ONLY, nullptr, &en))) en = nullptr;
            SysFreeString(lang);
            SysFreeString(q);
        }
        SysFreeString(ns);
    }
    while (en && !g_stop) {
        IWbemClassObject* obj = nullptr;
        ULONG got = 0;
        HRESULT hr = en->Next(500, 1, &obj, &got);
        if (hr == WBEM_S_TIMEDOUT) continue;
        if (FAILED(hr)) break;
        if (got && obj) {
            VARIANT v;
            VariantInit(&v);
            if (SUCCEEDED(obj->Get(L"Brightness", 0, &v, nullptr, nullptr)) && (v.vt == VT_UI1 || v.vt == VT_I4)) {
                int b = v.vt == VT_UI1 ? v.bVal : v.lVal;
                PostToUi(WM_ISLAND_LEVEL, 1, std::clamp(b, 0, 100));
            }
            VariantClear(&v);
            obj->Release();
        }
    }
    SafeRelease(en);
    SafeRelease(svc);
    SafeRelease(loc);
    CoUninitialize();
}

// ============================================================================
// Microphone / camera in use (Windows' privacy "in use" records)
// ============================================================================

static bool CapabilityInUse(const wchar_t* cap) {
    wstring base = wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\") + cap;
    std::function<bool(const wstring&, int)> scan = [&](const wstring& path, int depth) -> bool {
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
        bool inUse = false;
        ULONGLONG start = 0, stop = 0;
        DWORD sz = sizeof(start);
        if (RegQueryValueExW(k, L"LastUsedTimeStart", nullptr, nullptr, (BYTE*)&start, &sz) == ERROR_SUCCESS) {
            sz = sizeof(stop);
            if (RegQueryValueExW(k, L"LastUsedTimeStop", nullptr, nullptr, (BYTE*)&stop, &sz) == ERROR_SUCCESS && start && !stop) inUse = true;
        }
        if (!inUse && depth < 2) {
            wchar_t name[512];
            for (DWORD i = 0;; i++) {
                DWORD n = 512;
                if (RegEnumKeyExW(k, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                if (scan(path + L"\\" + name, depth + 1)) { inUse = true; break; }
            }
        }
        RegCloseKey(k);
        return inUse;
    };
    return scan(base, 0);
}

// ============================================================================
// UI state (UI thread only)
// ============================================================================

enum View { V_HIDDEN, V_LINE, V_NOTIF, V_DIP, V_HALF, V_EXP, V_LEVEL, V_AUTH, V_COUNT };
enum Tab { T_ALERTS, T_MUSIC, T_CLOCK, T_SHELF, T_CLIP, T_NOTES, T_COUNT };
enum ClockMode { CM_TIMER, CM_ALARMS, CM_STOPWATCH };

static const float CW = 480, CH = 340;  // canvas (DIPs); the island is centred in it
static const wchar_t* TAB_NAMES[T_COUNT] = {L"Notifications", L"Music", L"Clock", L"Shelf", L"Clipboard", L"Notes"};

// Click/scroll targets, rebuilt every frame (canvas DIPs).
enum Act {
    A_NONE, A_POPUP, A_TAB, A_PIN, A_CHIP, A_H_TIMER, A_H_SW, A_H_PLAY,
    A_N_OPEN, A_N_REMOVE, A_N_CLEAR,
    A_M_PREV, A_M_PLAY, A_M_NEXT, A_M_LYRICS, A_M_SEEKBAR, A_M_LINE, A_M_SOURCE, A_M_LYRLIST,
    A_C_MODE, A_T_MINUS, A_T_PLUS, A_T_PRESET, A_T_START, A_T_PAUSE, A_T_CANCEL, A_T_DIGITS, A_AL_TOGGLE,
    A_SW_GO, A_SW_LAP,
    A_S_OPEN, A_S_REMOVE, A_S_CLEAR,
    A_CL_COPY, A_CL_REMOVE, A_CL_CLEAR, A_CL_LIST,
    A_NT_AREA, A_NT_LINE, A_NT_COPY, A_NT_CLEAR,
    A_LEVEL, A_N_LIST, A_HALF_ROW,
};
struct Hit {
    D2D1_RECT_F r;
    int act, arg;
};
static std::vector<Hit> g_hits;

struct ClipItem {
    wstring text;
    ULONGLONG at;
};

struct Ui {
    bool expanded = false, halfOn = false, pinned = false;
    Tab tab = T_ALERTS;
    // notifications
    std::vector<Notif> list;  // newest first
    bool popActive = false;
    Notif pop;
    ULONGLONG popUntil = 0, dipUntil = 0;
    bool dnd = false;
    int dndHeld = 0;
    bool fullscreen = false;
    float alertsScroll = 0;
    // levels
    int levelKind = 0, vol = 0, bri = -1;
    bool muted = false;
    ULONGLONG levelUntil = 0, inlineUntil = 0;
    Spring levelV;
    // verification
    int auth = 0;  // 0 none, 1 scanning, 2 done
    ULONGLONG authAt = 0;
    HWND authHwnd = nullptr;
    struct {
        bool valid = false, expanded = false, halfOn = false;
        Tab tab = T_ALERTS;
        float alertsScroll = 0, clipScroll = 0, noteScroll = 0, halfScroll = 0;
    } prev;
    // see-through
    bool holdSee = false, seeLock = false, ducking = false;
    // clock
    ClockMode cmode = CM_TIMER;
    int timerSet = 300;
    bool timerRun = false, timerPaused = false;
    double timerEnd = 0;
    int timerLeft = 0, timerDur = 0;
    bool swRun = false;
    double swStart = 0;
    long long swAcc = 0;
    std::vector<long long> laps;
    std::vector<bool> alarmOn;
    int lastAlarmMinute = -1;
    // shelf
    std::vector<wstring> shelf;
    bool dropping = false, openedByDrop = false;
    // clipboard
    std::vector<ClipItem> clips;
    int ignoreClip = 0;
    float clipScroll = 0;
    // notes
    std::vector<wstring> note{L""};
    int cLine = 0, cCol = 0;
    bool noteFocus = false;
    float noteScroll = 0;
    ULONGLONG noteSaveAt = 0;
    // music
    bool lyricsOpen = false;
    Spring lyrScroll;
    float lyrTarget = 0;
    int lastLyric = -2;
    unsigned lyrGenSeen = 0;
    bool lyrUserScrolled = false;
    // half view
    float halfScroll = 0, halfContentW = 0;
    // privacy dots
    bool mic = false, cam = false;
    // pointer
    bool hover = false;
    D2D1_POINT_2F mouse{-1, -1};
    int pressAct = A_NONE;
    // geometry
    Spring w, h, hov;
    float op[V_COUNT] = {};
    View lastView = V_HIDDEN;
} U;

static void Wake();  // start the frame loop
static void SaveUiValues();

static ULONGLONG g_now = 0;

static bool HalfHasContent() {
    bool media;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        media = g_media.has;
    }
    return U.timerRun || U.swRun || U.swAcc > 0 || media || !U.list.empty() || U.mic || U.cam || U.dnd;
}

static View CurrentView() {
    if (U.auth) return V_AUTH;
    if (U.fullscreen) return g_now < U.levelUntil ? V_LEVEL : V_HIDDEN;
    if (U.expanded) return V_EXP;
    if (g_now < U.levelUntil) return V_LEVEL;
    if (U.popActive) return g_now < U.dipUntil ? V_DIP : V_NOTIF;
    if (U.halfOn && HalfHasContent()) return V_HALF;
    bool active;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        active = g_media.has;
    }
    active = active || U.timerRun || U.swRun || U.mic || U.cam || U.dnd;
    if (!active && g_set.idleHidden) return V_HIDDEN;
    return V_LINE;
}

static bool Ghosted() { return U.holdSee || U.seeLock || U.ducking; }

// ============================================================================
// Direct2D / DirectWrite resources
// ============================================================================

static ID2D1Factory* g_d2d = nullptr;
static IDWriteFactory* g_dw = nullptr;
static ID2D1DCRenderTarget* g_rt = nullptr;
static ID2D1SolidColorBrush* g_br = nullptr;
static ID2D1Layer* g_layers[3] = {};
static IDWriteInlineObject* g_ellipsis = nullptr;
static wstring g_textFont = L"Segoe UI Variable Text", g_iconFont = L"Segoe Fluent Icons";
static std::map<unsigned long long, IDWriteTextFormat*> g_formats;
static std::map<wstring, ID2D1Bitmap*> g_bitmaps;
static ID2D1Bitmap* g_artBmp = nullptr;
static unsigned g_artGenShown = 0;
static HDC g_memDC = nullptr;
static HBITMAP g_dib = nullptr, g_oldBmp = nullptr;
static int g_pxW = 0, g_pxH = 0;
static float g_scale = 1;
static POINT g_winPos{};

static bool FontExists(const wchar_t* family) {
    IDWriteFontCollection* fc = nullptr;
    if (FAILED(g_dw->GetSystemFontCollection(&fc, FALSE))) return false;
    UINT32 i;
    BOOL ok = FALSE;
    fc->FindFamilyName(family, &i, &ok);
    fc->Release();
    return ok;
}

static bool InitGraphics() {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&g_dw)))) return false;
    if (!FontExists(g_textFont.c_str())) g_textFont = L"Segoe UI";
    if (!FontExists(g_iconFont.c_str())) g_iconFont = L"Segoe MDL2 Assets";
    auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                              D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (FAILED(g_d2d->CreateDCRenderTarget(&props, &g_rt))) return false;
    g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &g_br);
    for (auto& l : g_layers) g_rt->CreateLayer(nullptr, &l);
    return true;
}

static void ReleaseBitmaps() {
    for (auto& kv : g_bitmaps) SafeRelease(kv.second);
    g_bitmaps.clear();
    SafeRelease(g_artBmp);
    g_artGenShown = 0;
}

static void ShutdownGraphics() {
    ReleaseBitmaps();
    for (auto& kv : g_formats) kv.second->Release();
    g_formats.clear();
    SafeRelease(g_ellipsis);
    for (auto& l : g_layers) SafeRelease(l);
    SafeRelease(g_br);
    SafeRelease(g_rt);
    SafeRelease(g_dw);
    SafeRelease(g_d2d);
    if (g_memDC) {
        SelectObject(g_memDC, g_oldBmp);
        DeleteObject(g_dib);
        DeleteDC(g_memDC);
        g_memDC = nullptr;
    }
}

static void EnsureSurface() {
    int w = (int)std::ceil(CW * g_scale), h = (int)std::ceil(CH * g_scale);
    if (g_memDC && w == g_pxW && h == g_pxH) return;
    if (g_memDC) {
        SelectObject(g_memDC, g_oldBmp);
        DeleteObject(g_dib);
        DeleteDC(g_memDC);
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    g_memDC = CreateCompatibleDC(screen);
    g_dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    g_oldBmp = (HBITMAP)SelectObject(g_memDC, g_dib);
    g_pxW = w;
    g_pxH = h;
    RECT rc{0, 0, w, h};
    g_rt->BindDC(g_memDC, &rc);
}

static D2D1_COLOR_F Col(UINT32 rgb, float a = 1) { return D2D1::ColorF(rgb, a); }
static D2D1_COLOR_F Wh(float a) { return D2D1::ColorF(1, 1, 1, a); }
static const UINT32 ORANGE = 0xFF9F0A, ORANGE_L = 0xFFC266, BLUE = 0x76B9ED, GREEN = 0x34C759, BG = 0x070709;

static IDWriteTextFormat* Fmt(float size, DWRITE_FONT_WEIGHT wt, int align, bool icon = false, bool wrap = false) {
    unsigned long long key = ((unsigned long long)(size * 10) << 24) | ((unsigned long long)wt << 8) | (align << 2) | (icon ? 2 : 0) | (wrap ? 1 : 0);
    auto it = g_formats.find(key);
    if (it != g_formats.end()) return it->second;
    IDWriteTextFormat* f = nullptr;
    g_dw->CreateTextFormat(icon ? g_iconFont.c_str() : g_textFont.c_str(), nullptr, wt, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                           L"", &f);
    if (!f) return nullptr;
    f->SetTextAlignment(align == 1 ? DWRITE_TEXT_ALIGNMENT_CENTER : align == 2 ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (!wrap) {
        if (!g_ellipsis) g_dw->CreateEllipsisTrimmingSign(f, &g_ellipsis);
        DWRITE_TRIMMING t{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        f->SetTrimming(&t, g_ellipsis);
    }
    g_formats[key] = f;
    return f;
}

static void Text(const wstring& s, float x, float y, float w, float h, float size, D2D1_COLOR_F c, DWRITE_FONT_WEIGHT wt = DWRITE_FONT_WEIGHT_NORMAL,
                 int align = 0) {
    if (s.empty() || w <= 1) return;
    g_br->SetColor(c);
    g_rt->DrawText(s.c_str(), (UINT32)s.size(), Fmt(size, wt, align), D2D1::RectF(x, y, x + w, y + h), g_br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

static float TextW(const wstring& s, float size, DWRITE_FONT_WEIGHT wt = DWRITE_FONT_WEIGHT_NORMAL) {
    IDWriteTextLayout* l = nullptr;
    if (FAILED(g_dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), Fmt(size, wt, 0), 4000, 100, &l))) return 0;
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    l->Release();
    return m.widthIncludingTrailingWhitespace;
}

static void Icon(wchar_t glyph, float cx, float cy, float size, D2D1_COLOR_F c) {
    wchar_t s[2] = {glyph, 0};
    g_br->SetColor(c);
    g_rt->DrawText(s, 1, Fmt(size, DWRITE_FONT_WEIGHT_NORMAL, 1, true), D2D1::RectF(cx - size, cy - size, cx + size, cy + size), g_br);
}

static void FillRR(float x, float y, float w, float h, float r, D2D1_COLOR_F c) {
    g_br->SetColor(c);
    g_rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), r, r), g_br);
}
static void FillR(float x, float y, float w, float h, D2D1_COLOR_F c) {
    g_br->SetColor(c);
    g_rt->FillRectangle(D2D1::RectF(x, y, x + w, y + h), g_br);
}
static void Circle(float cx, float cy, float r, D2D1_COLOR_F c) {
    g_br->SetColor(c);
    g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), g_br);
}

// Progress ring (starts at 12 o'clock).
static void Ring(float cx, float cy, float r, float frac, D2D1_COLOR_F track, D2D1_COLOR_F fill, float stroke = 2) {
    g_br->SetColor(track);
    g_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), g_br, stroke);
    frac = std::clamp(frac, 0.0f, 1.0f);
    if (frac <= 0.001f) return;
    g_br->SetColor(fill);
    if (frac >= 0.999f) {
        g_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), g_br, stroke);
        return;
    }
    ID2D1PathGeometry* pg = nullptr;
    ID2D1GeometrySink* sk = nullptr;
    if (FAILED(g_d2d->CreatePathGeometry(&pg))) return;
    pg->Open(&sk);
    float a = frac * 2 * 3.14159265f;
    sk->BeginFigure(D2D1::Point2F(cx, cy - r), D2D1_FIGURE_BEGIN_HOLLOW);
    sk->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx + r * std::sin(a), cy - r * std::cos(a)), D2D1::SizeF(r, r), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                frac > 0.5f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sk->EndFigure(D2D1_FIGURE_END_OPEN);
    sk->Close();
    g_rt->DrawGeometry(pg, g_br, stroke);
    sk->Release();
    pg->Release();
}

static ID2D1Bitmap* BitmapFromPixels(const Pixels& px) {
    if (!px.w || !px.h || px.data.empty()) return nullptr;
    ID2D1Bitmap* b = nullptr;
    g_rt->CreateBitmap(D2D1::SizeU(px.w, px.h), px.data.data(), px.w * 4,
                       D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &b);
    return b;
}

static ID2D1Bitmap* AppIconBitmap(const wstring& aumid) {
    if (aumid.empty()) return nullptr;
    auto it = g_bitmaps.find(L"app:" + aumid);
    if (it != g_bitmaps.end()) return it->second;
    Pixels px;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        auto a = g_apps.find(aumid);
        if (a == g_apps.end()) return nullptr;  // not resolved yet; try next frame
        px = a->second.icon;
    }
    ID2D1Bitmap* b = BitmapFromPixels(px);
    g_bitmaps[L"app:" + aumid] = b;
    return b;
}

static ID2D1Bitmap* FileIconBitmap(const wstring& path) {
    auto it = g_bitmaps.find(L"file:" + path);
    if (it != g_bitmaps.end()) return it->second;
    ID2D1Bitmap* b = nullptr;
    SHFILEINFOW fi{};
    if (SHGetFileInfoW(path.c_str(), 0, &fi, sizeof(fi), SHGFI_ICON | SHGFI_LARGEICON) && fi.hIcon) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = 32;
        bi.bmiHeader.biHeight = -32;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HDC dc = CreateCompatibleDC(nullptr);
        HBITMAP hb = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ old = SelectObject(dc, hb);
        DrawIconEx(dc, 0, 0, fi.hIcon, 32, 32, 0, nullptr, DI_NORMAL);
        SelectObject(dc, old);
        DeleteDC(dc);
        b = BitmapFromPixels(PixelsFromHBitmap(hb));
        DeleteObject(hb);
        DestroyIcon(fi.hIcon);
    }
    g_bitmaps[L"file:" + path] = b;
    return b;
}

// Draw a bitmap clipped to a rounded rectangle.
static void DrawRounded(ID2D1Bitmap* b, float x, float y, float w, float h, float r, float opacity = 1) {
    if (!b) return;
    ID2D1RoundedRectangleGeometry* g = nullptr;
    if (FAILED(g_d2d->CreateRoundedRectangleGeometry(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), r, r), &g))) return;
    g_rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), g), g_layers[2]);
    g_rt->DrawBitmap(b, D2D1::RectF(x, y, x + w, y + h), opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    g_rt->PopLayer();
    g->Release();
}

// An app's icon, or a coloured tile with a glyph / first letter.
static void AppTile(const wstring& aumid, const wstring& app, float x, float y, float s, wchar_t glyph = 0, UINT32 color = 0x5A6470) {
    if (ID2D1Bitmap* b = AppIconBitmap(aumid)) {
        DrawRounded(b, x, y, s, s, s * 0.22f);
        return;
    }
    FillRR(x, y, s, s, s * 0.28f, Col(color));
    if (glyph) Icon(glyph, x + s / 2, y + s / 2, s * 0.55f, Wh(1));
    else Text(app.substr(0, 1), x, y, s, s, s * 0.55f, Wh(1), DWRITE_FONT_WEIGHT_BOLD, 1);
}

static void AddHit(float x, float y, float w, float h, int act, int arg = 0) { g_hits.push_back({D2D1::RectF(x, y, x + w, y + h), act, arg}); }
static bool Hovered(float x, float y, float w, float h) {
    return U.mouse.x >= x && U.mouse.x < x + w && U.mouse.y >= y && U.mouse.y < y + h;
}
// A small round button: hover highlight, glyph, hit.
static void Btn(float x, float y, float s, wchar_t glyph, int act, int arg = 0, float gs = 11, D2D1_COLOR_F c = Wh(0.9f), bool lit = false) {
    if (lit || Hovered(x, y, s, s)) FillRR(x, y, s, s, 6, Wh(lit ? 0.16f : 0.1f));
    Icon(glyph, x + s / 2, y + s / 2, gs, c);
    AddHit(x, y, s, s, act, arg);
}
// A text link.
static float Link(const wstring& s, float xRight, float y, float h, int act, int arg = 0, float size = 10.5f) {
    float w = TextW(s, size) + 2;
    float x = xRight - w;
    Text(s, x, y, w, h, size, Wh(Hovered(x, y, w, h) ? 0.95f : 0.55f));
    AddHit(x, y, w, h, act, arg);
    return x;
}

// ============================================================================
// Glyphs (Segoe Fluent Icons / MDL2 Assets)
// ============================================================================
namespace G {
const wchar_t Bell = 0xEA8F, Music = 0xE8D6, Clock = 0xE916, Shelf = 0xE7B8, Clip = 0xE77F, Note = 0xE70B, Pin = 0xE718, Pinned = 0xE840,
              Play = 0xE768, Pause = 0xE769, Prev = 0xE892, Next = 0xE893, Lyrics = 0xE8E4, X = 0xE711, Plus = 0xE710, Minus = 0xE738,
              Vol = 0xE767, Vol1 = 0xE993, Mute = 0xE74F, Sun = 0xE706, Mic = 0xE720, Cam = 0xE714, Moon = 0xE708, Lock = 0xE72E,
              Unlock = 0xE785, Check = 0xE73E, Flag = 0xE7C1, Reset = 0xE72C, Globe = 0xE774, Doc = 0xE8A5, Code = 0xE943, Link = 0xE71B;
}

// ============================================================================
// Model helpers used while drawing
// ============================================================================

static Media MediaSnapshot(bool withLyrics) {
    std::lock_guard<std::mutex> lk(g_mx);
    Media m;
    m.has = g_media.has;
    m.title = g_media.title;
    m.artist = g_media.artist;
    m.album = g_media.album;
    m.aumid = g_media.aumid;
    m.app = g_media.app;
    m.playing = g_media.playing;
    m.durMs = g_media.durMs;
    m.posMs = g_media.posMs;
    if (m.playing) m.posMs += (long long)((QpcSeconds() - g_media.sampledAt) * 1000);
    if (m.durMs > 0) m.posMs = std::clamp(m.posMs, 0LL, m.durMs);
    m.lyrState = g_media.lyrState;
    m.lyrGen = g_media.lyrGen;
    if (withLyrics) m.lyrics = g_media.lyrics;
    if (g_media.artGen != g_artGenShown) {
        SafeRelease(g_artBmp);
        g_artBmp = BitmapFromPixels(g_media.art);
        g_artGenShown = g_media.artGen;
    }
    return m;
}

static int CurrentLyric(const Media& m) {
    if (m.lyrState != 1 || m.lyrics.empty()) return -1;
    int idx = -1;
    for (size_t i = 0; i < m.lyrics.size(); i++) {
        if (m.lyrics[i].ms <= m.posMs + 150) idx = (int)i;
        else break;
    }
    return idx;
}

static int TimerRemaining() {
    if (!U.timerRun) return 0;
    if (U.timerPaused) return U.timerLeft;
    return std::max(0, (int)std::ceil(U.timerEnd - QpcSeconds()));
}
static long long StopwatchMs() { return U.swAcc + (U.swRun ? (long long)((QpcSeconds() - U.swStart) * 1000) : 0); }

static wstring Ago(ULONGLONG at) {
    ULONGLONG m = (g_now - at) / 60000;
    if (m < 1) return L"now";
    if (m < 60) return std::to_wstring(m) + L"m";
    return std::to_wstring(m / 60) + L"h";
}

// ============================================================================
// Sizes of each view
// ============================================================================

static const float EXP_W = 356, HALF_MAX = 400;

static float PaneHeight(Tab t) {
    switch (t) {
        case T_ALERTS:
            return U.list.empty() ? 30 + 16 : 16 + std::min<float>(4, (float)U.list.size()) * 24 + 18;
        case T_MUSIC: {
            bool has;
            {
                std::lock_guard<std::mutex> lk(g_mx);
                has = g_media.has;
            }
            return has ? 118 : 40;
        }
        case T_CLOCK:
            return U.cmode == CM_ALARMS ? 26 + std::max(1.0f, std::min<float>(3, (float)g_set.alarms.size())) * 24 : 26 + 40;
        case T_SHELF:
            return 70 + 18;
        case T_CLIP:
            return U.clips.empty() ? 30 + 16 : std::min<float>(4, (float)U.clips.size()) * 28 + 18;
        case T_NOTES:
            return 84 + 18;
        default:
            return 40;
    }
}

static float ExpandedHeight() {
    float h = 28 + PaneHeight(U.tab) + 8;
    if (g_now < U.inlineUntil) h += 32;
    return h;
}

struct HalfChip {
    int kind;  // 0 timer, 1 stopwatch, 2 music, 3 dots, 4 bell
    float w;
};
static std::vector<HalfChip> HalfChips() {
    std::vector<HalfChip> c;
    if (U.timerRun) c.push_back({0, 98});
    if (U.swRun || U.swAcc > 0) c.push_back({1, 108});
    bool media;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        media = g_media.has;
    }
    if (media) c.push_back({2, 176});
    if (U.mic || U.cam || U.dnd) c.push_back({3, 12.0f + (U.dnd ? 18 : 0) + (U.mic ? 10 : 0) + (U.cam ? 10 : 0)});
    if (!U.list.empty()) c.push_back({4, 52});
    return c;
}

static D2D1_SIZE_F ViewSize(View v) {
    switch (v) {
        case V_HIDDEN: return {90, 0};
        case V_LINE: {
            bool ind = U.mic || U.cam || U.dnd;
            return ind ? D2D1::SizeF(92.0f + (U.mic ? 9 : 0) + (U.cam ? 9 : 0) + (U.dnd ? 11 : 0), 11) : D2D1::SizeF(90, 6);
        }
        case V_NOTIF: return {300, 34};
        case V_DIP: return {210, 22};
        case V_HALF: {
            float t = 12;
            for (auto& c : HalfChips()) t += c.w;
            U.halfContentW = t;
            return {std::clamp(t, 150.0f, HALF_MAX), 32};
        }
        case V_EXP: return {EXP_W, ExpandedHeight()};
        case V_LEVEL: return {320, 36};
        case V_AUTH: return {206, 36};
        default: return {90, 6};
    }
}

// ============================================================================
// Drawing each view. Every function draws at (x, 0) with the view's own size
// and records hits in canvas coordinates.
// ============================================================================

static void DrawLine(float x, float w, float h) {
    if (h < 8.5f) return;
    float a = std::min(1.0f, (h - 8.5f) / 2.5f);
    if (U.dnd) Icon(G::Moon, x + 14, h / 2, 7.5f, D2D1::ColorF(0xB6A8FF, a));
    float dx = x + w - 12;
    if (U.mic) { Circle(dx, h / 2, 2.6f, D2D1::ColorF(0xFF9F0A, a)); dx -= 9; }
    if (U.cam) Circle(dx, h / 2, 2.6f, D2D1::ColorF(GREEN, a));
}

static void DrawNotif(float x) {
    const Notif& n = U.pop;
    AppTile(n.aumid, n.app, x + 16, 8, 18, n.aumid.empty() ? G::Clock : 0, n.aumid.empty() ? ORANGE : 0x5A6470);
    float tx = x + 42, maxW = 300 - 42 - 16;
    float tw = std::min(112.0f, TextW(n.title, 12, DWRITE_FONT_WEIGHT_SEMI_BOLD) + 1);
    Text(n.title, tx, 0, tw, 34, 12, Wh(1), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    Text(n.body, tx + tw + 8, 0, maxW - tw - 8, 34, 12, Wh(0.72f));
    AddHit(x, 0, 300, 34, A_POPUP);
}

static void DrawLevel(float x, float w, float h, int kind, float y = 0) {
    wchar_t g = kind == 1 ? G::Sun : (U.muted || U.vol == 0) ? G::Mute : U.vol < 50 ? G::Vol1 : G::Vol;
    int val = kind == 1 ? std::max(0, U.bri) : U.vol;
    Icon(g, x + 18, y + h / 2, 12, Wh(0.95f));
    Text(kind == 1 ? L"Brightness" : L"Sound", x + 32, y, 70, h, 11.5f, Wh(1), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    float bx = x + 106, bw = w - 106 - 40;
    FillRR(bx, y + h / 2 - 2, bw, 4, 2, Wh(0.18f));
    float frac = std::clamp((float)U.levelV.v / 100.0f, 0.0f, 1.0f);
    if (frac > 0) FillRR(bx, y + h / 2 - 2, bw * frac, 4, 2, Wh(1));
    Text(std::to_wstring(val), x + w - 36, y, 24, h, 11, Wh(0.6f), DWRITE_FONT_WEIGHT_NORMAL, 2);
    AddHit(bx - 6, y + 4, bw + 12, h - 8, A_LEVEL, kind);
}

static void DrawAuth(float x) {
    float t = (float)((g_now - U.authAt) / 1000.0);
    if (U.auth == 1) {
        float pulse = 0.55f + 0.45f * std::fabs(std::sin(t * 3.0f));
        Circle(x + 25, 18, 11, D2D1::ColorF(0x8CC8F2, 0.14f));
        Icon(G::Lock, x + 25, 18, 11, D2D1::ColorF(0x8CC8F2, pulse));
        Text(L"Verifying…", x + 44, 3, 150, 17, 12, Wh(1), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        Text(L"Windows Security", x + 44, 18, 150, 14, 10, Wh(0.5f));
    } else {
        float pop = std::min(1.0f, t / 0.3f);
        Circle(x + 25, 18, 11 * (0.6f + 0.4f * pop), D2D1::ColorF(GREEN, 0.16f));
        Icon(t < 0.15f ? G::Lock : G::Unlock, x + 25, 18, 11, D2D1::ColorF(GREEN, 1));
        Text(L"Done", x + 44, 3, 120, 17, 12, Wh(1), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        Text(L"Windows Security", x + 44, 18, 120, 14, 10, Wh(0.5f));
        Icon(G::Check, x + 184, 18, 12 * std::min(1.0f, t / 0.35f), D2D1::ColorF(GREEN, std::min(1.0f, t / 0.25f)));
    }
}

static void DrawEq(float x, float cy, bool playing, D2D1_COLOR_F c) {
    float t = (float)QpcSeconds();
    for (int i = 0; i < 3; i++) {
        float hh = playing ? 4 + 6 * (0.5f + 0.5f * std::sin(t * (7 + i * 2.3f) + i * 1.7f)) : 3;
        FillRR(x + i * 4, cy + 5 - hh, 2.5f, hh, 1, c);
    }
}

static void DrawHalf(float x, float w) {
    auto chips = HalfChips();
    float total = U.halfContentW;
    float maxScroll = std::max(0.0f, total - w);
    U.halfScroll = std::clamp(U.halfScroll, 0.0f, maxScroll);
    float cx = total <= w ? x + (w - total) / 2 + 6 : x + 6 - U.halfScroll;
    Media m = MediaSnapshot(false);
    for (size_t i = 0; i < chips.size(); i++) {
        auto& c = chips[i];
        float cw = c.w;
        bool hov = Hovered(cx, 0, cw, 32) && c.kind != 3;
        if (hov) FillRR(cx + 1, 3, cw - 2, 26, 9, Wh(0.08f));
        if (i > 0 && !hov) FillR(cx, 10, 1, 12, Wh(0.12f));
        float right = cx + cw - 8;
        if (c.kind == 0) {
            int rem = TimerRemaining();
            Ring(cx + 18, 16, 7, U.timerDur ? (float)rem / U.timerDur : 0, Wh(0.15f), Col(ORANGE), 2);
            Text(FmtClock(rem), cx + 30, 0, cw - 58, 32, 12.5f, Col(ORANGE_L, U.timerPaused ? 0.55f : 1), DWRITE_FONT_WEIGHT_SEMI_BOLD);
            AddHit(cx, 0, cw, 32, A_CHIP, T_CLOCK);
            Btn(right - 20, 6, 20, U.timerPaused ? G::Play : G::Pause, A_H_TIMER, 0, 9);
        } else if (c.kind == 1) {
            Icon(G::Clock, cx + 16, 16, 11, Wh(0.85f));
            Text(FmtStopwatch(StopwatchMs()), cx + 28, 0, cw - 54, 32, 12.5f, Wh(U.swRun ? 1 : 0.55f), DWRITE_FONT_WEIGHT_SEMI_BOLD);
            AddHit(cx, 0, cw, 32, A_CHIP, T_CLOCK);
            Btn(right - 20, 6, 20, U.swRun ? G::Pause : G::Play, A_H_SW, 0, 9);
        } else if (c.kind == 2) {
            if (g_artBmp) DrawRounded(g_artBmp, cx + 8, 7, 18, 18, 4);
            else FillRR(cx + 8, 7, 18, 18, 4, Col(0xD93A64));
            DrawEq(cx + 32, 16, m.playing, Col(0xFFB07A));
            Text(m.title, cx + 48, 0, cw - 48 - 30, 32, 12, Wh(1), DWRITE_FONT_WEIGHT_SEMI_BOLD);
            AddHit(cx, 0, cw, 32, A_CHIP, T_MUSIC);
            Btn(right - 20, 6, 20, m.playing ? G::Pause : G::Play, A_H_PLAY, 0, 9);
            if (m.durMs > 0) FillR(cx + 8, 29, (cw - 16) * (float)m.posMs / (float)m.durMs, 1.5f, Wh(0.55f));
        } else if (c.kind == 3) {
            float dx = cx + 8;
            if (U.dnd) { Icon(G::Moon, dx + 6, 16, 10, Col(0xB6A8FF)); dx += 18; }
            if (U.cam) { Circle(dx + 3, 16, 3, Col(GREEN)); dx += 10; }
            if (U.mic) Circle(dx + 3, 16, 3, Col(ORANGE));
        } else if (c.kind == 4) {
            Icon(G::Bell, cx + 17, 16, 11, Wh(0.9f));
            wstring n = std::to_wstring(U.list.size());
            float bw = std::max(16.0f, TextW(n, 10, DWRITE_FONT_WEIGHT_BOLD) + 9);
            FillRR(cx + 28, 8, bw, 16, 8, Col(0xFF6B8A));
            Text(n, cx + 28, 8, bw, 16, 10, Wh(1), DWRITE_FONT_WEIGHT_BOLD, 1);
            AddHit(cx, 0, cw, 32, A_CHIP, T_ALERTS);
        }
        cx += cw;
    }
    AddHit(x, 0, w, 32, A_HALF_ROW);  // wheel target (scroll sideways); lowest priority
}

// ---- Expanded panes ---------------------------------------------------------

static void PaneAlerts(float x, float y, float w) {
    if (U.list.empty()) {
        Text(L"No notifications", x, y, w, 30, 12, Wh(0.5f), DWRITE_FONT_WEIGHT_NORMAL, 1);
        y += 30;
    } else {
        Text(L"EARLIER", x, y, 100, 16, 9.5f, Wh(0.45f), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        Link(L"Clear", x + w, y, 16, A_N_CLEAR);
        y += 16;
        float listH = std::min<float>(4, (float)U.list.size()) * 24;
        float maxS = std::max(0.0f, U.list.size() * 24.0f - listH);
        U.alertsScroll = std::clamp(U.alertsScroll, 0.0f, maxS);
        AddHit(x, y, w, listH, A_N_LIST);
        g_rt->PushAxisAlignedClip(D2D1::RectF(x - 2, y, x + w + 2, y + listH), D2D1_ANTIALIAS_MODE_ALIASED);
        for (size_t i = 0; i < U.list.size(); i++) {
            float ry = y + i * 24 - U.alertsScroll;
            if (ry + 24 < y || ry > y + listH) continue;
            const Notif& n = U.list[i];
            bool hov = Hovered(x, ry, w, 24) && U.mouse.y >= y && U.mouse.y < y + listH;
            if (hov) FillRR(x - 4, ry + 1, w + 8, 22, 6, Wh(0.06f));
            AppTile(n.aumid, n.app, x, ry + 4, 16, n.aumid.empty() ? G::Clock : 0, n.aumid.empty() ? ORANGE : 0x5A6470);
            float aw = std::min(110.0f, TextW(n.app, 11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD) + 1);
            Text(n.app, x + 24, ry, aw, 24, 11.5f, Wh(1), DWRITE_FONT_WEIGHT_SEMI_BOLD);
            wstring rest = L" · " + n.title + (n.body.empty() ? L"" : L" · " + n.body);
            float rightW = hov ? 24 : 56;
            Text(rest, x + 24 + aw, ry, w - 24 - aw - rightW, 24, 11.5f, Wh(0.8f));
            if (hov) {
                if (y <= ry && ry + 24 <= y + listH + 1) AddHit(x, ry, w - 24, 24, A_N_OPEN, (int)i);
                Btn(x + w - 22, ry + 2, 20, G::X, A_N_REMOVE, (int)i, 9);
            } else {
                Text(FmtTimeOfDay(n.at), x + w - 56, ry, 56, 24, 10.5f, Wh(0.45f), DWRITE_FONT_WEIGHT_NORMAL, 2);
            }
        }
        g_rt->PopAxisAlignedClip();
        y += listH;
    }
    y += 2;
    wstring status = U.dnd ? (L"Do not disturb is on" + (U.dndHeld ? L" · " + std::to_wstring(U.dndHeld) + L" held" : wstring())) : L"Pop-ups follow each app's rule";
    Text(status, x, y, w, 16, 10.5f, Wh(0.45f));
}

static void PaneMusic(float x, float y, float w) {
    Media m = MediaSnapshot(U.lyricsOpen);
    if (!m.has) {
        Text(L"Nothing playing", x, y, w, 40, 12, Wh(0.5f), DWRITE_FONT_WEIGHT_NORMAL, 1);
        return;
    }
    // Left: cover, title, controls
    if (g_artBmp) DrawRounded(g_artBmp, x, y + 2, 42, 42, 9);
    else FillRR(x, y + 2, 42, 42, 9, Col(0xD93A64));
    Text(m.title, x + 50, y + 4, 86, 18, 12.5f, Wh(1), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    Text(m.artist, x + 50, y + 22, 86, 16, 11, Wh(0.6f));
    float by = y + 56;
    Btn(x - 2, by, 24, G::Prev, A_M_PREV, 0, 10);
    Btn(x + 24, by, 24, m.playing ? G::Pause : G::Play, A_M_PLAY, 0, 11);
    Btn(x + 50, by, 24, G::Next, A_M_NEXT, 0, 10);
    Btn(x + 80, by, 24, G::Lyrics, A_M_LYRICS, 0, 10, U.lyricsOpen ? Col(0xFF8A65) : Wh(0.9f), U.lyricsOpen);

    // Right: details or lyrics
    float rx = x + 144, rw = w - 144, rh = 84;
    if (!U.lyricsOpen) {
        const wchar_t* labels[3] = {L"Album", L"Artist", L"Playing on"};
        wstring vals[3] = {m.album.empty() ? L"—" : m.album, m.artist.empty() ? L"—" : m.artist, m.app};
        for (int i = 0; i < 3; i++) {
            Text(labels[i], rx, y + 4 + i * 26, 70, 20, 11, Wh(0.45f));
            Text(vals[i], rx + 70, y + 4 + i * 26, rw - 70, 20, 11.5f, Wh(0.95f), DWRITE_FONT_WEIGHT_NORMAL, 2);
        }
    } else if (m.lyrState == 0) {
        Text(L"Looking for lyrics…", rx, y, rw, rh, 11.5f, Wh(0.5f), DWRITE_FONT_WEIGHT_NORMAL, 1);
    } else if (m.lyrState >= 3) {
        Icon(G::Lyrics, rx + rw / 2, y + 24, 11, Wh(0.4f));
        Text(L"No lyrics available", rx, y + 34, rw, 18, 12, Wh(0.85f), DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        Text(m.lyrState == 4 ? L"Lyrics lookup is off in settings" : L"None found for this song", rx, y + 52, rw, 16, 10.5f, Wh(0.5f),
             DWRITE_FONT_WEIGHT_NORMAL, 1);
    } else {
        const float LH = 26;
        bool timed = m.lyrState == 1;
        int cur = CurrentLyric(m);
        if (m.lyrGen != U.lyrGenSeen) { U.lyrGenSeen = m.lyrGen; U.lyrScroll = {}; U.lastLyric = -2; U.lyrTarget = 0; }
        float contentH = m.lyrics.size() * LH;
        float maxS = std::max(0.0f, contentH - rh + LH);
        if (timed && cur != U.lastLyric) {  // follow the song: centre the current line at every change
            U.lastLyric = cur;
            U.lyrUserScrolled = false;
            U.lyrTarget = std::clamp(std::max(0, cur) * LH - rh / 2 + LH / 2 + LH / 2, 0.0f, maxS);
            Wake();
        }
        U.lyrScroll.v = std::clamp((float)U.lyrScroll.v, 0.0f, maxS);
        AddHit(rx, y, rw, rh, A_M_LYRLIST);
        g_rt->PushAxisAlignedClip(D2D1::RectF(rx - 4, y, x + w + 2, y + rh), D2D1_ANTIALIAS_MODE_ALIASED);
        for (size_t i = 0; i < m.lyrics.size(); i++) {
            float ly = y + LH / 2 + i * LH - (float)U.lyrScroll.v;
            if (ly + LH < y || ly > y + rh) continue;
            bool isCur = timed && (int)i == cur;
            float a = !timed ? 0.8f : isCur ? 1.0f : (int)i < cur ? 0.55f : 0.42f;
            bool hov = timed && Hovered(rx - 4, ly, rw + 8, LH) && U.mouse.y >= y && U.mouse.y < y + rh;
            if (hov) FillRR(rx - 4, ly + 2, rw + 8, LH - 4, 6, Wh(0.07f));
            Text(m.lyrics[i].text.empty() ? L"♪" : m.lyrics[i].text, rx, ly, rw, LH, isCur ? 13 : 12.5f, Wh(a),
                 isCur ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_MEDIUM);
            if (timed && ly >= y && ly + LH <= y + rh) AddHit(rx - 4, ly, rw + 8, LH, A_M_LINE, (int)i);
        }
        g_rt->PopAxisAlignedClip();
    }

    // Seek bar and footer
    float sy = y + 92;
    float frac = m.durMs > 0 ? (float)m.posMs / (float)m.durMs : 0;
    bool hovBar = Hovered(x, sy - 5, w, 12);
    FillRR(x, sy, w, hovBar ? 4.0f : 2.5f, 2, Wh(0.15f));
    FillRR(x, sy, w * frac, hovBar ? 4.0f : 2.5f, 2, Wh(0.85f));
    AddHit(x, sy - 5, w, 12, A_M_SEEKBAR);
    float fy = sy + 8;
    Text(FmtClock(m.posMs / 1000), x, fy, 60, 18, 10.5f, Wh(0.55f));
    Text(L"-" + FmtClock((m.durMs - m.posMs) / 1000), x + w - 60, fy, 60, 18, 10.5f, Wh(0.55f), DWRITE_FONT_WEIGHT_NORMAL, 2);
    wstring src = m.app + (U.lyricsOpen && m.lyrState == 2 ? L" · lyrics not timed" : L"");
    float sw = std::min(w - 130, TextW(src, 10.5f) + 26);
    float sx = x + (w - sw) / 2;
    FillRR(sx, fy + 1, sw, 16, 8, Wh(Hovered(sx, fy, sw, 18) ? 0.12f : 0.07f));
    Icon(G::Globe, sx + 10, fy + 9, 8.5f, Wh(0.7f));
    Text(src, sx + 18, fy, sw - 22, 18, 10.5f, Wh(0.7f));
    AddHit(sx, fy, sw, 18, A_M_SOURCE);
}

static void PaneClock(float x, float y, float w) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t tb[64] = L"", db[64] = L"";
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, nullptr, tb, 64);
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"ddd, MMM d", db, 64, nullptr);
    float tw = TextW(tb, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD) + 2;
    Text(tb, x, y, tw, 22, 13, Wh(1), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    Text(db, x + tw + 8, y, 90, 22, 11, Wh(0.55f));
    // mode switch
    const wchar_t* modes[3] = {L"Timer", L"Alarms", L"Stopwatch"};
    float mx = x + w;
    float widths[3];
    for (int i = 2; i >= 0; i--) { widths[i] = TextW(modes[i], 11) + 16; mx -= widths[i]; }
    FillRR(mx - 2, y + 1, x + w - mx + 2, 20, 7, Wh(0.06f));
    for (int i = 0; i < 3; i++) {
        bool on = U.cmode == i;
        if (on) FillRR(mx, y + 3, widths[i], 16, 5, Wh(0.1f));
        Text(modes[i], mx, y + 1, widths[i], 20, 11, on ? Col(ORANGE_L) : Wh(Hovered(mx, y, widths[i], 22) ? 0.9f : 0.55f), DWRITE_FONT_WEIGHT_MEDIUM, 1);
        AddHit(mx, y, widths[i], 22, A_C_MODE, i);
        mx += widths[i];
    }
    y += 26;
    if (U.cmode == CM_TIMER) {
        if (U.timerRun) {
            int rem = TimerRemaining();
            Text(FmtClock(rem), x, y, 80, 40, 22, Col(ORANGE_L), DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
            float bx = x + 90, bw = w - 90 - 62;
            float frac = U.timerDur ? 1.0f - (float)rem / U.timerDur : 0;
            FillRR(bx, y + 14, bw, 3, 1.5f, D2D1::ColorF(ORANGE, 0.18f));
            FillRR(bx, y + 14, bw * frac, 3, 1.5f, Col(ORANGE));
            SYSTEMTIME end;
            GetLocalTime(&end);
            FILETIME ft;
            SystemTimeToFileTime(&end, &ft);
            ULARGE_INTEGER u;
            u.LowPart = ft.dwLowDateTime;
            u.HighPart = ft.dwHighDateTime;
            u.QuadPart += (ULONGLONG)rem * 10000000ULL;
            ft.dwLowDateTime = u.LowPart;
            ft.dwHighDateTime = u.HighPart;
            FileTimeToSystemTime(&ft, &end);
            Text(U.timerPaused ? L"Paused" : L"Ends at " + FmtTimeOfDay(end), bx, y + 20, bw, 16, 10.5f, Wh(0.55f));
            Btn(x + w - 56, y + 8, 24, U.timerPaused ? G::Play : G::Pause, A_T_PAUSE, 0, 10);
            Btn(x + w - 28, y + 8, 24, G::X, A_T_CANCEL, 0, 9);
        } else {
            Btn(x - 2, y + 8, 24, G::Minus, A_T_MINUS, 0, 10);
            wstring t = FmtClock(U.timerSet);
            Text(t, x + 22, y, 64, 40, 22, Wh(1), DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
            AddHit(x + 22, y, 64, 40, A_T_DIGITS);
            Btn(x + 86, y + 8, 24, G::Plus, A_T_PLUS, 0, 10);
            int presets[3] = {5, 10, 25};
            float px = x + 116;
            for (int i = 0; i < 3; i++) {
                wstring l = std::to_wstring(presets[i]) + L" min";
                float pw = TextW(l, 11) + 14;
                bool on = U.timerSet == presets[i] * 60;
                FillRR(px, y + 10, pw, 20, 10, on ? D2D1::ColorF(ORANGE, 0.22f) : Wh(Hovered(px, y + 10, pw, 20) ? 0.14f : 0.08f));
                Text(l, px, y + 10, pw, 20, 11, on ? Col(ORANGE_L) : Wh(0.95f), DWRITE_FONT_WEIGHT_MEDIUM, 1);
                AddHit(px, y + 10, pw, 20, A_T_PRESET, presets[i]);
                px += pw + 4;
            }
            float sw = 48;
            FillRR(x + w - sw, y + 10, sw, 20, 10, Col(Hovered(x + w - sw, y + 10, sw, 20) ? 0xFFB340 : ORANGE));
            Text(L"Start", x + w - sw, y + 10, sw, 20, 11.5f, Col(0x1D1200), DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
            AddHit(x + w - sw, y + 10, sw, 20, A_T_START);
        }
    } else if (U.cmode == CM_ALARMS) {
        if (g_set.alarms.empty()) {
            Text(L"Add alarms in the mod's settings", x, y, w, 24, 11.5f, Wh(0.5f), DWRITE_FONT_WEIGHT_NORMAL, 1);
        }
        for (size_t i = 0; i < g_set.alarms.size() && i < 3; i++) {
            auto& a = g_set.alarms[i];
            float ry = y + i * 24;
            SYSTEMTIME at{};
            at.wHour = (WORD)a.hour;
            at.wMinute = (WORD)a.minute;
            Text(FmtTimeOfDay(at), x, ry, 76, 24, 13, Wh(1), DWRITE_FONT_WEIGHT_SEMI_BOLD);
            Text(a.label, x + 80, ry, w - 80 - 44, 24, 11.5f, Wh(0.6f));
            bool on = i < U.alarmOn.size() && U.alarmOn[i];
            FillRR(x + w - 32, ry + 5, 30, 14, 7, on ? Col(ORANGE) : Wh(0.22f));
            Circle(on ? x + w - 10 : x + w - 25, ry + 12, 5, on ? Col(0x1D1200) : Wh(0.9f));
            AddHit(x + w - 36, ry, 38, 24, A_AL_TOGGLE, (int)i);
        }
    } else {
        long long ms = StopwatchMs();
        Text(FmtStopwatch(ms), x, y, 110, 40, 21, Col(ORANGE_L), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        float lx = x + 116, lw = w - 116 - 118;
        if (U.laps.empty()) Text(L"Laps show here", lx, y + 6, lw, 28, 10.5f, Wh(0.45f));
        for (size_t i = 0; i < U.laps.size() && i < 2; i++)
            Text(L"Lap " + std::to_wstring(U.laps.size() - i) + L" · " + FmtStopwatch(U.laps[i]), lx, y + 6 + i * 14, lw, 14, 10.5f, Wh(0.6f));
        float bx = x + w - 112;
        const wchar_t* go = U.swRun ? L"Pause" : ms ? L"Resume" : L"Start";
        FillRR(bx, y + 10, 64, 20, 10, Col(Hovered(bx, y + 10, 64, 20) ? 0xFFB340 : ORANGE));
        Text(go, bx, y + 10, 64, 20, 11.5f, Col(0x1D1200), DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        AddHit(bx, y + 10, 64, 20, A_SW_GO);
        Btn(x + w - 42, y + 8, 24, U.swRun ? G::Flag : G::Reset, A_SW_LAP, 0, 10);
    }
}

static void PaneShelf(float x, float y, float w) {
    if (U.shelf.empty()) {
        g_br->SetColor(Wh(U.dropping ? 0.5f : 0.18f));
        ID2D1StrokeStyle* dash = nullptr;
        float dashes[] = {4, 3};
        g_d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                             D2D1_LINE_JOIN_ROUND, 10, D2D1_DASH_STYLE_CUSTOM, 0),
                                 dashes, 2, &dash);
        g_rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x + 1, y + 1, x + w - 1, y + 68), 10, 10), g_br, 1, dash);
        SafeRelease(dash);
        Text(U.dropping ? L"Let go to add to the shelf" : L"Drop files here to keep them handy", x, y, w, 70, 11.5f, Wh(0.55f), DWRITE_FONT_WEIGHT_NORMAL, 1);
    } else {
        if (U.dropping) FillRR(x, y, w, 70, 10, Wh(0.06f));
        float tx = x;
        for (size_t i = 0; i < U.shelf.size() && tx + 60 <= x + w; i++) {
            const wstring& p = U.shelf[i];
            bool hov = Hovered(tx, y, 60, 70);
            if (hov) FillRR(tx, y, 60, 70, 8, Wh(0.07f));
            if (ID2D1Bitmap* b = FileIconBitmap(p)) g_rt->DrawBitmap(b, D2D1::RectF(tx + 14, y + 6, tx + 46, y + 38));
            else Icon(G::Doc, tx + 30, y + 22, 18, Wh(0.8f));
            Text(PathFindFileNameW(p.c_str()), tx + 2, y + 42, 56, 20, 10.5f, Wh(0.85f), DWRITE_FONT_WEIGHT_NORMAL, 1);
            AddHit(tx, y, 60, 70, A_S_OPEN, (int)i);
            if (hov) {
                Circle(tx + 52, y + 8, 8, Col(0x2A2C34));
                Icon(G::X, tx + 52, y + 8, 7, Wh(0.9f));
                AddHit(tx + 44, y, 16, 16, A_S_REMOVE, (int)i);
            }
            tx += 64;
        }
    }
    y += 72;
    Text(U.shelf.empty() ? L"Nothing on the shelf" : std::to_wstring(U.shelf.size()) + L" item" + (U.shelf.size() == 1 ? L"" : L"s") + L" · drag out or click to open",
         x, y, w - 60, 16, 10.5f, Wh(0.45f));
    if (!U.shelf.empty()) Link(L"Clear", x + w, y, 16, A_S_CLEAR);
}

static void PaneClip(float x, float y, float w) {
    if (U.clips.empty()) {
        Text(L"Clipboard history is empty", x, y, w, 30, 12, Wh(0.5f), DWRITE_FONT_WEIGHT_NORMAL, 1);
        y += 30;
        Text(L"Copy some text to add it", x, y, w, 16, 10.5f, Wh(0.45f));
        return;
    }
    float listH = std::min<float>(4, (float)U.clips.size()) * 28;
    float maxS = std::max(0.0f, U.clips.size() * 28.0f - listH);
    U.clipScroll = std::clamp(U.clipScroll, 0.0f, maxS);
    AddHit(x, y, w, listH, A_CL_LIST);
    g_rt->PushAxisAlignedClip(D2D1::RectF(x - 4, y, x + w + 4, y + listH), D2D1_ANTIALIAS_MODE_ALIASED);
    for (size_t i = 0; i < U.clips.size(); i++) {
        float ry = y + i * 28 - U.clipScroll;
        if (ry + 28 < y || ry > y + listH) continue;
        auto& c = U.clips[i];
        bool hov = Hovered(x, ry, w, 28) && U.mouse.y >= y && U.mouse.y < y + listH;
        if (hov) FillRR(x - 4, ry + 1, w + 8, 26, 7, Wh(0.07f));
        FillRR(x, ry + 4, 20, 20, 6, Wh(0.08f));
        bool link = c.text.rfind(L"http", 0) == 0;
        Icon(link ? G::Link : G::Doc, x + 10, ry + 14, 9, Wh(0.8f));
        wstring oneLine = c.text;
        std::replace(oneLine.begin(), oneLine.end(), L'\n', L' ');
        std::replace(oneLine.begin(), oneLine.end(), L'\r', L' ');
        Text(oneLine, x + 28, ry, w - 28 - 34, 28, 12, Wh(0.95f));
        if (hov) {
            AddHit(x, ry, w - 26, 28, A_CL_COPY, (int)i);
            Btn(x + w - 22, ry + 4, 20, G::X, A_CL_REMOVE, (int)i, 9);
        } else {
            Text(Ago(c.at), x + w - 32, ry, 32, 28, 10.5f, Wh(0.45f), DWRITE_FONT_WEIGHT_NORMAL, 2);
        }
    }
    g_rt->PopAxisAlignedClip();
    y += listH + 2;
    Text(L"Click one to copy it again", x, y, w - 60, 16, 10.5f, Wh(0.45f));
    Link(L"Clear all", x + w, y, 16, A_CL_CLEAR);
}

// ---- Notes editor (lines; a line of exactly "---" is drawn as a full-width rule) ----

static const float NOTE_PAD = 10, NOTE_H = 84, NOTE_LH = 18;

static IDWriteTextLayout* NoteLayout(const wstring& s, float w) {
    IDWriteTextLayout* l = nullptr;
    g_dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), Fmt(12.5f, DWRITE_FONT_WEIGHT_NORMAL, 0, false, true), w, 1000, &l);
    if (l) l->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    return l;
}
static float NoteLineHeight(const wstring& s, float w) {
    if (s == L"---") return 13;
    IDWriteTextLayout* l = NoteLayout(s.empty() ? L" " : s, w);
    float h = NOTE_LH;
    if (l) {
        DWRITE_TEXT_METRICS m{};
        l->GetMetrics(&m);
        h = std::max(NOTE_LH, m.height);
        l->Release();
    }
    return h;
}

static size_t NoteChars() {
    size_t n = 0;
    for (auto& l : U.note)
        if (l != L"---") n += l.size();
    return n;
}

static void PaneNotes(float x, float y, float w) {
    bool focus = U.noteFocus;
    FillRR(x, y, w, NOTE_H, 9, Wh(0.06f));
    if (focus) {
        g_br->SetColor(D2D1::ColorF(BLUE, 0.6f));
        g_rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x + 0.75f, y + 0.75f, x + w - 0.75f, y + NOTE_H - 0.75f), 9, 9), g_br, 1.5f);
    }
    AddHit(x, y, w, NOTE_H, A_NT_AREA);
    float iw = w - NOTE_PAD * 2;
    bool empty = U.note.size() == 1 && U.note[0].empty();
    // keep the caret in view
    float caretY = 0, total = 0;
    for (size_t i = 0; i < U.note.size(); i++) {
        float lh = NoteLineHeight(U.note[i], iw);
        if ((int)i == U.cLine) caretY = total;
        total += lh;
    }
    float viewH = NOTE_H - 14;
    if (focus) {
        if (caretY - U.noteScroll < 0) U.noteScroll = caretY;
        if (caretY + NOTE_LH - U.noteScroll > viewH) U.noteScroll = caretY + NOTE_LH - viewH;
    }
    U.noteScroll = std::clamp(U.noteScroll, 0.0f, std::max(0.0f, total - viewH));
    g_rt->PushAxisAlignedClip(D2D1::RectF(x + 2, y + 4, x + w - 2, y + NOTE_H - 4), D2D1_ANTIALIAS_MODE_ALIASED);
    if (empty && !focus) Text(L"Jot something down… it saves as you type", x + NOTE_PAD, y + 7, iw, NOTE_LH, 12.5f, Wh(0.35f));
    float ly = y + 7 - U.noteScroll;
    bool blink = ((GetTickCount64() / 530) % 2) == 0;
    for (size_t i = 0; i < U.note.size(); i++) {
        const wstring& s = U.note[i];
        float lh = NoteLineHeight(s, iw);
        if (s == L"---") {
            FillR(x + NOTE_PAD, ly + 6, iw, 1, Wh(0.28f));  // exactly the text area's width
            if (focus && (int)i == U.cLine && blink) FillR(x + NOTE_PAD + (U.cCol ? iw : 0) - 1, ly, 1.5f, 13, Col(BLUE));
        } else {
            IDWriteTextLayout* l = NoteLayout(s, iw);
            if (l) {
                g_br->SetColor(D2D1::ColorF(0xF4F5F7, 1));
                g_rt->DrawTextLayout(D2D1::Point2F(x + NOTE_PAD, ly), l, g_br);
                if (focus && (int)i == U.cLine && blink) {
                    DWRITE_HIT_TEST_METRICS hm{};
                    FLOAT cx = 0, cy = 0;
                    l->HitTestTextPosition((UINT32)std::min<size_t>(U.cCol, s.size()), FALSE, &cx, &cy, &hm);
                    FillR(x + NOTE_PAD + cx, ly + cy + 1, 1.5f, std::max(14.0f, hm.height - 2), Col(BLUE));
                }
                l->Release();
            }
        }
        ly += lh;
    }
    g_rt->PopAxisAlignedClip();
    float fy = y + NOTE_H + 2;
    Text(empty ? L"Empty · saves as you type" : std::to_wstring(NoteChars()) + L" characters · saved", x, fy, 150, 16, 10.5f, Wh(0.45f));
    float lx = Link(L"Clear", x + w, fy, 16, A_NT_CLEAR);
    Text(L"·", lx - 10, fy, 8, 16, 10.5f, Wh(0.45f), DWRITE_FONT_WEIGHT_NORMAL, 1);
    lx = Link(L"Copy", lx - 10, fy, 16, A_NT_COPY);
    Text(L"·", lx - 10, fy, 8, 16, 10.5f, Wh(0.45f), DWRITE_FONT_WEIGHT_NORMAL, 1);
    lx = Link(L"Line", lx - 10, fy, 16, A_NT_LINE);
    Icon(G::Minus, lx - 6, fy + 8, 8, Wh(0.55f));
}

static void DrawExpanded(float x, float w) {
    // Tab bar: four on the left, three + pin on the right, the section name between.
    static const wchar_t icons[T_COUNT] = {G::Bell, G::Music, G::Clock, G::Shelf, G::Clip, G::Note};
    bool live[T_COUNT] = {};
    {
        std::lock_guard<std::mutex> lk(g_mx);
        live[T_MUSIC] = g_media.playing;
    }
    live[T_ALERTS] = U.popActive;
    live[T_CLOCK] = (U.timerRun && !U.timerPaused) || U.swRun;
    float bx = x + 7;
    for (int t = 0; t < 3; t++) {
        bool on = U.tab == t;
        if (on || Hovered(bx, 3, 22, 18)) FillRR(bx, 3, 22, 18, 6, Wh(on ? 0.13f : 0.07f));
        Icon(icons[t], bx + 11, 12, 10.5f, Wh(on ? 1 : 0.5f));
        if (live[t]) Circle(bx + 18, 6, 2, Col(0xFFCF6B));
        AddHit(bx, 3, 22, 18, A_TAB, t);
        bx += 24;
    }
    Text(TAB_NAMES[U.tab], x + 90, 2, w - 180, 20, 10.5f, Wh(0.6f), DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    bx = x + w - 7 - 24 * 4 + 2;
    for (int t = 3; t < T_COUNT; t++) {
        bool on = U.tab == t;
        if (on || Hovered(bx, 3, 22, 18)) FillRR(bx, 3, 22, 18, 6, Wh(on ? 0.13f : 0.07f));
        Icon(icons[t], bx + 11, 12, 10.5f, Wh(on ? 1 : 0.5f));
        AddHit(bx, 3, 22, 18, A_TAB, t);
        bx += 24;
    }
    if (U.pinned || Hovered(bx, 3, 22, 18)) FillRR(bx, 3, 22, 18, 6, Wh(U.pinned ? 0.0f : 0.07f));
    Icon(U.pinned ? G::Pinned : G::Pin, bx + 11, 12, 10.5f, U.pinned ? Col(0xFFCF6B) : Wh(0.5f));
    AddHit(bx, 3, 22, 18, A_PIN);

    float px = x + 12, pw = w - 24, py = 28;
    switch (U.tab) {
        case T_ALERTS: PaneAlerts(px, py, pw); break;
        case T_MUSIC: PaneMusic(px, py, pw); break;
        case T_CLOCK: PaneClock(px, py, pw); break;
        case T_SHELF: PaneShelf(px, py, pw); break;
        case T_CLIP: PaneClip(px, py, pw); break;
        case T_NOTES: PaneNotes(px, py, pw); break;
        default: break;
    }
    if (g_now < U.inlineUntil) {  // volume/brightness changed while open: a compact row under the pane
        float ly = py + PaneHeight(U.tab) + 4;
        FillRR(px, ly, pw, 26, 9, Wh(0.06f));
        DrawLevel(px - 4, pw + 8, 26, U.levelKind, ly);
    }
}

// ============================================================================
// Frame: step springs, draw, push to the screen
// ============================================================================

static void IslandPath(ID2D1GeometrySink* sk, float cx, float w, float h, float radius) {
    float x0 = cx - w / 2, x1 = cx + w / 2;
    float r = std::min(radius, h / 2), e = std::min(12.0f, h * 0.8f);
    sk->BeginFigure(D2D1::Point2F(x0 - e, 0), D2D1_FIGURE_BEGIN_FILLED);
    sk->AddQuadraticBezier(D2D1::QuadraticBezierSegment(D2D1::Point2F(x0, 0), D2D1::Point2F(x0, e)));
    sk->AddLine(D2D1::Point2F(x0, h - r));
    sk->AddArc(D2D1::ArcSegment(D2D1::Point2F(x0 + r, h), D2D1::SizeF(r, r), 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    sk->AddLine(D2D1::Point2F(x1 - r, h));
    sk->AddArc(D2D1::ArcSegment(D2D1::Point2F(x1, h - r), D2D1::SizeF(r, r), 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    sk->AddLine(D2D1::Point2F(x1, e));
    sk->AddQuadraticBezier(D2D1::QuadraticBezierSegment(D2D1::Point2F(x1, 0), D2D1::Point2F(x1 + e, 0)));
    sk->EndFigure(D2D1_FIGURE_END_CLOSED);
}

static D2D1_RECT_F g_islandRect{};  // canvas DIPs, for hit/hover checks

static bool Step(double dt) {
    g_now = NowMs();
    bool busy = false;
    if (U.popActive && g_now >= U.popUntil && !U.expanded) U.popActive = false;
    View v = CurrentView();
    D2D1_SIZE_F target = ViewSize(v);
    U.lastView = v;
    bool hoverable = U.hover && v != V_EXP && v != V_HIDDEN && v != V_AUTH;
    busy |= SpringStep(U.hov, hoverable ? 1 : 0, 0.16, dt, 1, 0.002);
    bool growW = target.width > U.w.v, growH = target.height > U.h.v;
    busy |= SpringStep(U.w, target.width, growW ? 0.48 : 0.34, dt, growW ? 0.82 : 1);
    busy |= SpringStep(U.h, target.height, growH ? 0.48 : 0.34, dt, growH ? 0.82 : 1);
    double p = 1 - (std::fabs(target.width - U.w.v) / std::max(40.0f, target.width) + std::fabs(target.height - U.h.v) / std::max(20.0f, target.height)) / 2;
    for (int i = 0; i < V_COUNT; i++) {
        double ramp = std::clamp((p - 0.35) / 0.5, 0.0, 1.0);
        double tgt = i == v ? ramp * ramp * (3 - 2 * ramp) : 0;
        if (g_speedScale <= 0) { U.op[i] = (float)(i == v ? 1 : 0); continue; }
        double tau = (i == v ? 0.07 : 0.05) / g_speedScale;
        U.op[i] += (float)((tgt - U.op[i]) * (1 - std::exp(-dt / tau)));
        if (std::fabs(tgt - U.op[i]) < 0.003 && (i != v || tgt == 1 || tgt == 0)) U.op[i] = (float)tgt;
        else busy = true;
    }
    busy |= SpringStep(U.levelV, U.levelKind == 1 ? std::max(0, U.bri) : U.vol, 0.18, dt, 1, 0.05);
    if (!U.lyrUserScrolled) busy |= SpringStep(U.lyrScroll, U.lyrTarget, 0.4, dt, 1, 0.3);
    // Things that move on their own while visible.
    bool mediaPlaying;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        mediaPlaying = g_media.playing && g_media.has;
    }
    if (v == V_HALF && (mediaPlaying || U.swRun)) busy = true;
    if (v == V_EXP && ((U.tab == T_MUSIC && mediaPlaying) || (U.tab == T_CLOCK && U.swRun) || (U.tab == T_NOTES && U.noteFocus))) busy = true;
    if (v == V_AUTH || U.dipUntil > g_now || U.levelUntil > g_now || U.inlineUntil > g_now) busy = true;
    return busy;
}

static void Render() {
    EnsureSurface();
    g_hits.clear();
    View v = U.lastView;
    float hv = (float)U.hov.v;
    bool thin = U.h.v < 12;
    float w = std::max(0.0f, (float)U.w.v + hv * (thin ? 14 : 8));
    float h = std::max(0.0f, (float)U.h.v + hv * (thin ? 3 : 2));
    float cx = CW / 2;
    g_islandRect = D2D1::RectF(cx - w / 2, 0, cx + w / 2, h);

    g_rt->BeginDraw();
    g_rt->SetTransform(D2D1::Matrix3x2F::Scale(g_scale, g_scale));
    g_rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    {
        if (h >= 0.5f) {
            ID2D1PathGeometry* pg = nullptr;
            ID2D1GeometrySink* sk = nullptr;
            if (SUCCEEDED(g_d2d->CreatePathGeometry(&pg)) && SUCCEEDED(pg->Open(&sk))) {
                IslandPath(sk, cx, w, h, (float)g_set.radius);
                sk->Close();
                sk->Release();
                g_br->SetColor(Col(BG));
                g_rt->FillGeometry(pg, g_br);
                if (thin) {  // a faint edge so the thin bar reads on dark wallpapers
                    g_br->SetColor(Wh(0.06f));
                    g_rt->DrawGeometry(pg, g_br, 0.75f);
                }
                for (int i = 0; i < V_COUNT; i++) {
                    if (U.op[i] < 0.01f || i == V_HIDDEN || i == V_DIP) continue;
                    D2D1_SIZE_F vs = ViewSize((View)i);
                    float vx = cx - vs.width / 2;
                    float dy = (1 - U.op[i]) * -3;
                    g_rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), pg, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                          D2D1::IdentityMatrix(), U.op[i]),
                                    g_layers[0]);
                    g_rt->SetTransform(D2D1::Matrix3x2F::Translation(0, dy) * D2D1::Matrix3x2F::Scale(g_scale, g_scale));
                    size_t hitsBefore = g_hits.size();
                    switch (i) {
                        case V_LINE: DrawLine(vx, vs.width, (float)U.h.v); break;
                        case V_NOTIF: DrawNotif(vx); break;
                        case V_HALF: DrawHalf(vx, vs.width); break;
                        case V_EXP: DrawExpanded(vx, vs.width); break;
                        case V_LEVEL: DrawLevel(vx, vs.width, 36, U.levelKind); break;
                        case V_AUTH: DrawAuth(vx); break;
                    }
                    // Only the settled current view takes clicks.
                    if (i != v || U.op[i] < 0.6f) g_hits.resize(hitsBefore);
                    g_rt->SetTransform(D2D1::Matrix3x2F::Scale(g_scale, g_scale));
                    g_rt->PopLayer();
                }
                pg->Release();
            } else {
                SafeRelease(sk);
                SafeRelease(pg);
            }
        } else if (!U.fullscreen) {
            // Hidden: an invisible strip at the top centre still takes the click.
            FillR(cx - 45, 0, 90, 6, D2D1::ColorF(0, 0, 0, 1.0f / 255));
        }
    }
    if (g_rt->EndDraw() == D2DERR_RECREATE_TARGET) {
        ReleaseBitmaps();
        for (auto& l : g_layers) SafeRelease(l);
        SafeRelease(g_br);
        SafeRelease(g_rt);
        auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                  D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        g_d2d->CreateDCRenderTarget(&props, &g_rt);
        g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        g_rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &g_br);
        for (auto& l : g_layers) g_rt->CreateLayer(nullptr, &l);
        g_pxW = 0;
        return;
    }
    POINT src{0, 0};
    SIZE size{g_pxW, g_pxH};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, (BYTE)(Ghosted() ? 41 : 255), AC_SRC_ALPHA};
    HDC screen = GetDC(nullptr);
    UpdateLayeredWindow(g_hwnd, screen, &g_winPos, &size, g_memDC, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

// ============================================================================
// Window, input and actions (UI thread)
// ============================================================================

#define WM_ISLAND_HOLD (WM_APP + 10)      // wParam 1: hold key went down, 0: cancel
#define WM_ISLAND_ESC (WM_APP + 11)
#define WM_ISLAND_CLICKOFF (WM_APP + 12)
#define WM_ISLAND_SETTINGS (WM_APP + 13)

enum { TM_FRAME = 1, TM_SLOW, TM_POINTER, TM_HOLD, TM_AUTHEND, TM_SAVE, TM_POPEND };
enum { HK_CYCLE = 1, HK_LOCK };

static bool g_frameOn = false, g_pointerOn = false;
static double g_lastFrame = 0;
static HHOOK g_kbHook = nullptr, g_mouseHook = nullptr;
static HWINEVENTHOOK g_winEvHook = nullptr;
static std::atomic<bool> g_holdDown{false}, g_holdOther{false};
static int g_dragAct = A_NONE, g_dragArg = 0;
static D2D1_RECT_F g_dragRect{};
static int g_shelfPress = -1;
static POINT g_pressPt{};
static bool g_inDragOut = false;

static void Wake() {
    if (!g_hwnd) return;
    if (!g_frameOn) {
        g_frameOn = true;
        g_lastFrame = QpcSeconds();
        SetTimer(g_hwnd, TM_FRAME, 7, nullptr);
    }
}

static void UpdateExStyle() {
    LONG_PTR ex = GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
    LONG_PTR want = Ghosted() ? (ex | WS_EX_TRANSPARENT) : (ex & ~WS_EX_TRANSPARENT);
    if (want != ex) SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, want);
}

static void InstallMouseHookIfNeeded();
static void UpdatePointerTimer();

static void OnFrame() {
    double t = QpcSeconds();
    double dt = std::min(0.05, std::max(0.0, t - g_lastFrame));
    g_lastFrame = t;
    bool busy = Step(dt);
    Render();
    UpdateExStyle();
    InstallMouseHookIfNeeded();
    UpdatePointerTimer();
    if (!busy) {
        KillTimer(g_hwnd, TM_FRAME);
        g_frameOn = false;
    }
}

static void Reposition() {
    HMONITOR mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dx = 96, dy = 96;
    using GetDpiForMonitor_t = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static auto getDpi = (GetDpiForMonitor_t)GetProcAddress(LoadLibraryW(L"shcore.dll"), "GetDpiForMonitor");
    if (getDpi) getDpi(mon, 0 /* MDT_EFFECTIVE_DPI */, &dx, &dy);
    g_scale = dx / 96.0f;
    EnsureSurface();
    int monW = mi.rcMonitor.right - mi.rcMonitor.left;
    g_winPos = {mi.rcMonitor.left + (monW - g_pxW) / 2, mi.rcMonitor.top};
    SetWindowPos(g_hwnd, HWND_TOPMOST, g_winPos.x, g_winPos.y, g_pxW, g_pxH, SWP_NOACTIVATE);
    Wake();
}

static D2D1_POINT_2F ClientToCanvas(int x, int y) { return D2D1::Point2F(x / g_scale, y / g_scale); }
static D2D1_POINT_2F ScreenToCanvas(POINT p) { return D2D1::Point2F((p.x - g_winPos.x) / g_scale, (p.y - g_winPos.y) / g_scale); }

static bool IsBackgroundAct(int a) { return a == A_HALF_ROW || a == A_N_LIST || a == A_CL_LIST || a == A_M_LYRLIST || a == A_NT_AREA; }

static const Hit* HitAt(D2D1_POINT_2F p, bool includeBackground = true) {
    for (int pass = 0; pass < 2; pass++) {
        for (auto it = g_hits.rbegin(); it != g_hits.rend(); ++it) {
            if (IsBackgroundAct(it->act) != (pass == 1)) continue;
            if (p.x >= it->r.left && p.x < it->r.right && p.y >= it->r.top && p.y < it->r.bottom) return &*it;
        }
        if (!includeBackground) break;
    }
    return nullptr;
}

static bool InIsland(D2D1_POINT_2F p) {
    return p.x >= g_islandRect.left && p.x < g_islandRect.right && p.y >= 0 && p.y < std::max(6.0f, g_islandRect.bottom);
}

static void CloseAll() {
    U.expanded = false;
    U.halfOn = false;
    U.noteFocus = false;
    Wake();
}

// ---- notifications ----

static void ShowPopup(const Notif& n) {
    if (U.popActive && CurrentView() == V_NOTIF && g_speedScale > 0) U.dipUntil = g_now + 190;  // fold, then pop again
    U.pop = n;
    U.popActive = true;
    U.popUntil = g_now + 5000 + (U.dipUntil > g_now ? 190 : 0);
    SetTimer(g_hwnd, TM_POPEND, (UINT)(U.popUntil - g_now + 20), nullptr);  // the frame loop sleeps meanwhile
    Wake();
}

static void AddNotification(Notif n, bool force = false) {
    g_now = NowMs();
    int rule = force ? 0 : RuleFor(n.app);
    if (rule == 2) return;
    U.list.insert(U.list.begin(), n);
    if (U.list.size() > 30) U.list.resize(30);
    bool pop = g_set.popups && !U.fullscreen && rule == 0;
    if (pop && U.dnd && !force && !AllowedDuringDnd(n.app)) {
        U.dndHeld++;
        pop = false;
    }
    if (pop) ShowPopup(n);
    Wake();
}

static void InternalNotify(const wstring& title, const wstring& body) {
    Notif n;
    n.app = L"Clock";
    n.title = title;
    n.body = body;
    GetLocalTime(&n.at);
    AddNotification(n, true);
    MessageBeep(MB_ICONASTERISK);
}

static void OpenApp(const wstring& aumid) {
    if (aumid.empty()) return;
    wstring target = L"shell:AppsFolder\\" + aumid;
    ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---- clipboard ----

static void SetClipboardText(const wstring& s) {
    if (!OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
    if (h) {
        memcpy(GlobalLock(h), s.c_str(), (s.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(h);
        if (SetClipboardData(CF_UNICODETEXT, h)) U.ignoreClip++;
        else GlobalFree(h);
    }
    CloseClipboard();
}

static void OnClipboardUpdate() {
    if (U.ignoreClip > 0) { U.ignoreClip--; return; }
    static UINT excl = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    if (IsClipboardFormatAvailable(excl)) return;  // password managers ask not to be recorded
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return;
    wstring text;
    for (int i = 0; i < 5 && text.empty(); i++) {
        if (OpenClipboard(g_hwnd)) {
            if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                if (auto p = (const wchar_t*)GlobalLock(h)) {
                    text = p;
                    GlobalUnlock(h);
                }
            }
            CloseClipboard();
            break;
        }
        Sleep(15);
    }
    if (text.empty() || text.size() > 20000) return;
    auto it = std::remove_if(U.clips.begin(), U.clips.end(), [&](const ClipItem& c) { return c.text == text; });
    U.clips.erase(it, U.clips.end());
    U.clips.insert(U.clips.begin(), {text, NowMs()});
    if (U.clips.size() > 8) U.clips.resize(8);
    if (U.expanded && U.tab == T_CLIP) Wake();
}

// ---- notes ----

static wstring NoteJoined(bool forCopy) {
    wstring s;
    for (size_t i = 0; i < U.note.size(); i++) {
        if (i) s += forCopy ? L"\r\n" : L"\n";
        s += (forCopy && U.note[i] == L"---") ? L"———" : U.note[i];
    }
    return s;
}
static void NoteChanged() {
    SetTimer(g_hwnd, TM_SAVE, 600, nullptr);
    Wake();
}
static void NoteInsertText(const wstring& t) {
    for (wchar_t c : t) {
        if (c == L'\r') continue;
        if (c == L'\n') {
            wstring& line = U.note[U.cLine];
            wstring rest = line == L"---" ? L"" : line.substr(std::min<size_t>(U.cCol, line.size()));
            if (line != L"---") line.erase(std::min<size_t>(U.cCol, line.size()));
            U.note.insert(U.note.begin() + U.cLine + 1, rest);
            U.cLine++;
            U.cCol = 0;
        } else if (c >= 32 || c == L'\t') {
            if (U.note[U.cLine] == L"---") {  // typing on a rule starts a new line under it
                U.note.insert(U.note.begin() + U.cLine + 1, L"");
                U.cLine++;
                U.cCol = 0;
            }
            U.note[U.cLine].insert(std::min<size_t>(U.cCol, U.note[U.cLine].size()), 1, c);
            U.cCol++;
        }
    }
    NoteChanged();
}
static void NoteKey(WPARAM vk) {
    bool ctrl = GetKeyState(VK_CONTROL) < 0;
    wstring& line = U.note[U.cLine];
    int len = line == L"---" ? 1 : (int)line.size();
    switch (vk) {
        case VK_LEFT:
            if (U.cCol > 0) U.cCol--;
            else if (U.cLine > 0) { U.cLine--; U.cCol = U.note[U.cLine] == L"---" ? 1 : (int)U.note[U.cLine].size(); }
            break;
        case VK_RIGHT:
            if (U.cCol < len) U.cCol++;
            else if (U.cLine + 1 < (int)U.note.size()) { U.cLine++; U.cCol = 0; }
            break;
        case VK_UP:
            if (U.cLine > 0) { U.cLine--; U.cCol = std::min<int>(U.cCol, U.note[U.cLine] == L"---" ? 1 : (int)U.note[U.cLine].size()); }
            break;
        case VK_DOWN:
            if (U.cLine + 1 < (int)U.note.size()) { U.cLine++; U.cCol = std::min<int>(U.cCol, U.note[U.cLine] == L"---" ? 1 : (int)U.note[U.cLine].size()); }
            break;
        case VK_HOME: U.cCol = 0; break;
        case VK_END: U.cCol = len; break;
        case VK_BACK:
            if (line == L"---") { U.note.erase(U.note.begin() + U.cLine); if (U.cLine > 0) U.cLine--; U.cCol = (int)U.note[U.cLine].size(); }
            else if (U.cCol > 0) { line.erase(U.cCol - 1, 1); U.cCol--; }
            else if (U.cLine > 0) {
                wstring cur = line;
                U.note.erase(U.note.begin() + U.cLine);
                U.cLine--;
                if (U.note[U.cLine] == L"---") { U.note[U.cLine] = cur; U.cCol = 0; }
                else { U.cCol = (int)U.note[U.cLine].size(); U.note[U.cLine] += cur; }
            }
            NoteChanged();
            break;
        case VK_DELETE:
            if (line == L"---") { U.note.erase(U.note.begin() + U.cLine); if (U.note.empty()) U.note.push_back(L""); U.cLine = std::min<int>(U.cLine, (int)U.note.size() - 1); U.cCol = 0; }
            else if (U.cCol < (int)line.size()) line.erase(U.cCol, 1);
            else if (U.cLine + 1 < (int)U.note.size()) {
                wstring next = U.note[U.cLine + 1];
                U.note.erase(U.note.begin() + U.cLine + 1);
                if (next != L"---") line += next;
            }
            NoteChanged();
            break;
        case 'V':
            if (ctrl && OpenClipboard(g_hwnd)) {
                wstring t;
                if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                    if (auto p = (const wchar_t*)GlobalLock(h)) { t = p; GlobalUnlock(h); }
                }
                CloseClipboard();
                NoteInsertText(t);
            }
            break;
        case 'C':
            if (ctrl) SetClipboardText(NoteJoined(true));
            break;
        case VK_ESCAPE:
            U.noteFocus = false;
            break;
    }
    if (U.note.empty()) U.note.push_back(L"");
    U.cLine = std::clamp(U.cLine, 0, (int)U.note.size() - 1);
    Wake();
}
// Place the caret where the note was clicked.
static void NoteClick(D2D1_POINT_2F p, const D2D1_RECT_F& area) {
    float iw = (area.right - area.left) - NOTE_PAD * 2;
    float y = area.top + 7 - U.noteScroll;
    for (size_t i = 0; i < U.note.size(); i++) {
        float lh = NoteLineHeight(U.note[i], iw);
        if (p.y < y + lh || i + 1 == U.note.size()) {
            U.cLine = (int)i;
            if (U.note[i] == L"---") U.cCol = p.x > (area.left + area.right) / 2 ? 1 : 0;
            else if (IDWriteTextLayout* l = NoteLayout(U.note[i], iw)) {
                BOOL trailing = FALSE, inside = FALSE;
                DWRITE_HIT_TEST_METRICS hm{};
                l->HitTestPoint(p.x - area.left - NOTE_PAD, p.y - y, &trailing, &inside, &hm);
                U.cCol = std::min<int>((int)U.note[i].size(), (int)hm.textPosition + (trailing ? 1 : 0));
                l->Release();
            }
            break;
        }
        y += lh;
    }
}

// ---- shelf ----

static void ShelfAdd(const wstring& path) {
    for (auto& s : U.shelf)
        if (_wcsicmp(s.c_str(), path.c_str()) == 0) return;
    U.shelf.push_back(path);
    if (U.shelf.size() > 12) U.shelf.erase(U.shelf.begin());
}

static void DragOutShelf(int idx) {
    if (idx < 0 || idx >= (int)U.shelf.size()) return;
    IShellItem* item = nullptr;
    if (FAILED(SHCreateItemFromParsingName(U.shelf[idx].c_str(), nullptr, IID_PPV_ARGS(&item)))) return;
    IDataObject* dobj = nullptr;
    if (SUCCEEDED(item->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&dobj)))) {
        DWORD eff = 0;
        g_inDragOut = true;
        SHDoDragDrop(g_hwnd, dobj, nullptr, DROPEFFECT_COPY | DROPEFFECT_LINK | DROPEFFECT_MOVE, &eff);
        g_inDragOut = false;
        dobj->Release();
    }
    item->Release();
}

class DropTarget : public IDropTarget {
    LONG ref = 1;
    bool ok = false;

   public:
    virtual ~DropTarget() = default;
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&ref);
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** pp) override {
        if (iid == IID_IUnknown || iid == IID_IDropTarget) {
            *pp = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *pp = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* d, DWORD, POINTL, DWORD* eff) override {
        FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        ok = !g_inDragOut && d->QueryGetData(&fe) == S_OK;
        *eff = ok ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        if (ok) {  // files over the island: it opens on the shelf, ready for the drop
            U.dropping = true;
            U.openedByDrop = !U.expanded;
            U.expanded = true;
            U.halfOn = false;
            U.tab = T_SHELF;
            Wake();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* eff) override {
        *eff = ok ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        if (U.dropping) {
            U.dropping = false;
            if (U.openedByDrop) U.expanded = false;
            Wake();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* d, DWORD, POINTL, DWORD* eff) override {
        FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM sm{};
        *eff = DROPEFFECT_NONE;
        if (ok && SUCCEEDED(d->GetData(&fe, &sm))) {
            HDROP hd = (HDROP)GlobalLock(sm.hGlobal);
            if (hd) {
                UINT n = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
                for (UINT i = 0; i < n; i++) {
                    wchar_t path[MAX_PATH * 4];
                    if (DragQueryFileW(hd, i, path, ARRAYSIZE(path))) ShelfAdd(path);
                }
                GlobalUnlock(sm.hGlobal);
            }
            ReleaseStgMedium(&sm);
            *eff = DROPEFFECT_COPY;
            SaveUiValues();
        }
        U.dropping = false;
        Wake();
        return S_OK;
    }
};
static DropTarget* g_drop = nullptr;

// ---- timer / stopwatch ----

static void StartTimer() {
    if (U.timerSet <= 0) return;
    U.timerDur = U.timerSet;
    U.timerEnd = QpcSeconds() + U.timerSet;
    U.timerRun = true;
    U.timerPaused = false;
    if (g_set.autoHalf && !U.expanded && !U.halfOn) U.halfOn = true;
}
static void ToggleTimerPause() {
    if (!U.timerRun) return;
    if (U.timerPaused) { U.timerEnd = QpcSeconds() + U.timerLeft; U.timerPaused = false; }
    else { U.timerLeft = TimerRemaining(); U.timerPaused = true; }
}
static void ToggleStopwatch() {
    if (U.swRun) { U.swAcc = StopwatchMs(); U.swRun = false; }
    else { U.swStart = QpcSeconds(); U.swRun = true; }
}

// ---- clicks ----

static void SeekFromX(float x, const D2D1_RECT_F& r) {
    Media m = MediaSnapshot(false);
    if (m.durMs <= 0) return;
    float frac = std::clamp((x - r.left - 6) / std::max(1.0f, r.right - r.left - 12), 0.0f, 1.0f);
    SendMediaCommand(MediaCmd::Seek, (long long)(frac * m.durMs));
}
static void LevelFromX(float x, const D2D1_RECT_F& r, int kind) {
    if (kind != 0) return;  // brightness is read-only in v1
    float frac = std::clamp((x - r.left - 6) / std::max(1.0f, r.right - r.left - 12), 0.0f, 1.0f);
    SetVolume((int)std::lround(frac * 100));
    U.vol = (int)std::lround(frac * 100);
}

static void DoAction(const Hit& h, D2D1_POINT_2F p) {
    switch (h.act) {
        case A_TAB:
            U.tab = (Tab)h.arg;
            U.noteFocus = false;
            SaveUiValues();
            break;
        case A_PIN: U.pinned = !U.pinned; SaveUiValues(); break;
        case A_N_OPEN:
            if (h.arg < (int)U.list.size()) {
                wstring a = U.list[h.arg].aumid;
                U.list.erase(U.list.begin() + h.arg);
                OpenApp(a);
                if (!U.pinned) CloseAll();
            }
            break;
        case A_N_REMOVE: if (h.arg < (int)U.list.size()) U.list.erase(U.list.begin() + h.arg); break;
        case A_N_CLEAR: U.list.clear(); U.dndHeld = 0; break;
        case A_M_PREV: SendMediaCommand(MediaCmd::Prev); break;
        case A_M_PLAY: case A_H_PLAY: SendMediaCommand(MediaCmd::PlayPause); break;
        case A_M_NEXT: SendMediaCommand(MediaCmd::Next); break;
        case A_M_LYRICS: U.lyricsOpen = !U.lyricsOpen; U.lastLyric = -2; SaveUiValues(); break;
        case A_M_SEEKBAR: SeekFromX(p.x, h.r); break;
        case A_M_LINE: {
            Media m = MediaSnapshot(true);
            if (h.arg < (int)m.lyrics.size() && m.lyrics[h.arg].ms >= 0) SendMediaCommand(MediaCmd::Seek, m.lyrics[h.arg].ms);
            U.lyrUserScrolled = false;
            break;
        }
        case A_M_SOURCE: {
            Media m = MediaSnapshot(false);
            OpenApp(m.aumid);
            if (!U.pinned) CloseAll();
            break;
        }
        case A_C_MODE: U.cmode = (ClockMode)h.arg; break;
        case A_T_MINUS: U.timerSet = std::max(0, U.timerSet - 60); break;
        case A_T_PLUS: U.timerSet = std::min(999 * 60 + 59, U.timerSet + 60); break;
        case A_T_PRESET: U.timerSet = h.arg * 60; break;
        case A_T_START: StartTimer(); break;
        case A_T_PAUSE: case A_H_TIMER: ToggleTimerPause(); break;
        case A_T_CANCEL: U.timerRun = false; break;
        case A_AL_TOGGLE:
            if (h.arg < (int)U.alarmOn.size()) U.alarmOn[h.arg] = !U.alarmOn[h.arg];
            break;
        case A_SW_GO: case A_H_SW: ToggleStopwatch(); break;
        case A_SW_LAP:
            if (U.swRun) U.laps.insert(U.laps.begin(), StopwatchMs());
            else { U.swAcc = 0; U.laps.clear(); }
            break;
        case A_S_OPEN:
            if (h.arg < (int)U.shelf.size()) ShellExecuteW(nullptr, L"open", U.shelf[h.arg].c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case A_S_REMOVE: if (h.arg < (int)U.shelf.size()) { U.shelf.erase(U.shelf.begin() + h.arg); SaveUiValues(); } break;
        case A_S_CLEAR: U.shelf.clear(); SaveUiValues(); break;
        case A_CL_COPY:
            if (h.arg < (int)U.clips.size()) {
                ClipItem c = U.clips[h.arg];
                U.clips.erase(U.clips.begin() + h.arg);
                c.at = NowMs();
                U.clips.insert(U.clips.begin(), c);
                SetClipboardText(c.text);
            }
            break;
        case A_CL_REMOVE: if (h.arg < (int)U.clips.size()) U.clips.erase(U.clips.begin() + h.arg); break;
        case A_CL_CLEAR: U.clips.clear(); break;
        case A_NT_AREA:
            U.noteFocus = true;
            NoteClick(p, h.r);
            SetForegroundWindow(g_hwnd);
            SetFocus(g_hwnd);
            break;
        case A_NT_LINE: {
            // A rule under the caret's line, then a fresh line to keep typing on.
            int at = std::clamp(U.cLine, 0, (int)U.note.size() - 1);
            bool emptyNote = U.note.size() == 1 && U.note[0].empty();
            if (emptyNote) { U.note[0] = L"---"; U.note.push_back(L""); U.cLine = 1; }
            else { U.note.insert(U.note.begin() + at + 1, L"---"); U.note.insert(U.note.begin() + at + 2, L""); U.cLine = at + 2; }
            U.cCol = 0;
            U.noteFocus = true;
            SetForegroundWindow(g_hwnd);
            SetFocus(g_hwnd);
            NoteChanged();
            break;
        }
        case A_NT_COPY: SetClipboardText(NoteJoined(true)); break;
        case A_NT_CLEAR: U.note = {L""}; U.cLine = U.cCol = 0; NoteChanged(); break;
        case A_LEVEL: LevelFromX(p.x, h.r, h.arg); break;
        default: break;
    }
    Wake();
}

static void OnLeftClick(D2D1_POINT_2F p) {
    g_now = NowMs();
    View v = CurrentView();
    if (v == V_AUTH) return;
    const Hit* h = HitAt(p);
    if (v == V_NOTIF) {  // the pop-up: open the Notifications tab with it at the top
        U.tab = T_ALERTS;
        U.expanded = true;
        U.halfOn = false;
        U.popActive = false;
        SaveUiValues();
        Wake();
        return;
    }
    if (v == V_EXP) {
        if (h) DoAction(*h, p);
        if (!h || h->act != A_NT_AREA) {
            if (U.noteFocus && (!h || (h->act != A_NT_LINE))) U.noteFocus = false;
        }
        Wake();
        return;
    }
    if (v == V_HALF) {
        if (h && (h->act == A_H_TIMER || h->act == A_H_SW || h->act == A_H_PLAY)) { DoAction(*h, p); return; }
        if (h && h->act == A_CHIP) U.tab = (Tab)h->arg;
        U.expanded = true;
        U.halfOn = false;
        U.ducking = false;
        SaveUiValues();
        Wake();
        return;
    }
    if (v == V_LEVEL && h && h->act == A_LEVEL) { DoAction(*h, p); return; }
    // the bar, or hidden
    U.expanded = true;
    U.levelUntil = 0;
    Wake();
}

static void OnRightClick() {
    g_now = NowMs();
    View v = CurrentView();
    switch (v) {
        case V_AUTH: return;
        case V_NOTIF: U.popActive = false; break;  // dismiss only; it stays in the list
        case V_LEVEL: U.levelUntil = 0; break;
        case V_EXP: U.expanded = false; U.halfOn = HalfHasContent(); U.noteFocus = false; break;
        case V_HALF: U.halfOn = false; U.ducking = false; break;
        default:
            if (HalfHasContent()) U.halfOn = true;
            else U.expanded = true;
    }
    Wake();
}

static void OnWheel(D2D1_POINT_2F p, int delta, bool horizontal) {
    const Hit* h = HitAt(p);
    float d = -delta / 120.0f;
    View v = CurrentView();
    if (v == V_LEVEL || (h && h->act == A_LEVEL)) {
        if (U.levelKind == 0) { SetVolume(U.vol + (delta > 0 ? 2 : -2)); U.vol = GetVolume(&U.muted); }
        U.levelUntil = U.levelUntil > g_now ? NowMs() + 1600 : U.levelUntil;
        Wake();
        return;
    }
    if (!h) return;
    switch (h->act) {
        case A_HALF_ROW: case A_CHIP: case A_H_PLAY: case A_H_SW: case A_H_TIMER: U.halfScroll += d * 40; break;
        case A_N_LIST: case A_N_OPEN: case A_N_REMOVE: U.alertsScroll += d * 24; break;
        case A_CL_LIST: case A_CL_COPY: case A_CL_REMOVE: U.clipScroll += d * 28; break;
        case A_M_LYRLIST: case A_M_LINE: U.lyrScroll.v += d * 26; U.lyrScroll.vel = 0; U.lyrUserScrolled = true; break;
        case A_NT_AREA: U.noteScroll += d * 18; break;
        case A_M_SEEKBAR: {
            Media m = MediaSnapshot(false);
            SendMediaCommand(MediaCmd::Seek, std::max(0LL, m.posMs + (delta > 0 ? 5000 : -5000)));
            break;
        }
        case A_T_DIGITS: {
            bool minutes = p.x < (h->r.left + h->r.right) / 2;
            U.timerSet = std::clamp(U.timerSet + (delta > 0 ? 1 : -1) * (minutes ? 60 : 5), 0, 999 * 60 + 59);
            break;
        }
        default: break;
    }
    (void)horizontal;
    Wake();
}

// ---- shake to see through (half view) ----

static struct {
    std::vector<double> turns;
    int dir = 0;
    float turnX = 0, prevX = -1e9f;
    double doneAt = -1e9, awayAt = 0;
} g_shake;

static bool InShakeArea(D2D1_POINT_2F p) {
    return p.x >= g_islandRect.left - 10 && p.x <= g_islandRect.right + 10 && p.y >= -2 && p.y <= g_islandRect.bottom + 8;
}

static void PointerTick() {
    POINT pt;
    GetCursorPos(&pt);
    D2D1_POINT_2F p = ScreenToCanvas(pt);
    double t = QpcSeconds();
    View v = CurrentView();
    if (U.ducking && v != V_HALF) { U.ducking = false; Wake(); }
    bool in = InShakeArea(p);
    if (U.ducking) {
        if (in) g_shake.awayAt = 0;
        else if (!g_shake.awayAt) g_shake.awayAt = t;
        if (g_shake.awayAt && t - g_shake.awayAt > 0.25) { U.ducking = false; g_shake.awayAt = 0; Wake(); }
    }
    if (!g_set.shake || v != V_HALF || !in || t - g_shake.doneAt < 0.5) {
        g_shake.turns.clear();
        g_shake.dir = 0;
        g_shake.prevX = p.x;
        return;
    }
    float prevX = g_shake.prevX < -1e8f ? p.x : g_shake.prevX;
    float dx = p.x - prevX;
    g_shake.prevX = p.x;
    if (std::fabs(dx) < 0.5f) return;
    int dir = dx > 0 ? 1 : -1;
    if (!g_shake.dir) g_shake.turnX = prevX;
    else if (dir != g_shake.dir) {
        if (std::fabs(prevX - g_shake.turnX) >= 4) g_shake.turns.push_back(t);  // small shakes count
        g_shake.turnX = prevX;
    }
    g_shake.dir = dir;
    auto& tr = g_shake.turns;
    if (tr.size() > 1 && tr[tr.size() - 1] - tr[tr.size() - 2] > 0.35) tr.erase(tr.begin(), tr.end() - 1);
    if (tr.size() >= 3 && t - tr[0] >= 0.2) {  // about 0.4 s of shaking toggles it
        tr.clear();
        g_shake.doneAt = t;
        U.ducking = !U.ducking;
        g_shake.awayAt = 0;
        Wake();
    }
}

static void UpdatePointerTimer() {
    bool want = (g_set.shake && U.lastView == V_HALF) || U.ducking;
    if (want != g_pointerOn) {
        g_pointerOn = want;
        if (want) SetTimer(g_hwnd, TM_POINTER, 16, nullptr);
        else KillTimer(g_hwnd, TM_POINTER);
    }
}

// ---- global hooks: keys (hold-to-see-through, Esc) and clicks elsewhere ----

static bool IsHoldVk(DWORD vk) {
    switch (g_set.holdKey) {
        case VK_MENU: return vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU;
        case VK_CONTROL: return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL;
        case VK_SHIFT: return vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT;
        default: return false;
    }
}

static LRESULT CALLBACK KeyboardProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        auto* k = (KBDLLHOOKSTRUCT*)lp;
        bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
        bool up = wp == WM_KEYUP || wp == WM_SYSKEYUP;
        if (IsHoldVk(k->vkCode)) {
            if (down && !g_holdDown) {
                g_holdDown = true;
                g_holdOther = false;
                PostToUi(WM_ISLAND_HOLD, 1);
            } else if (up) {
                g_holdDown = false;
                PostToUi(WM_ISLAND_HOLD, 0);
            }
        } else if (down) {
            if (g_holdDown && !g_holdOther) {
                g_holdOther = true;  // Alt+Tab, Alt+Z… is not a "hold"
                PostToUi(WM_ISLAND_HOLD, 0);
            }
            if (k->vkCode == VK_ESCAPE) PostToUi(WM_ISLAND_ESC);
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static LRESULT CALLBACK MouseProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN || wp == WM_MBUTTONDOWN)) {
        auto* m = (MSLLHOOKSTRUCT*)lp;
        if (WindowFromPoint(m->pt) != g_hwnd) PostToUi(WM_ISLAND_CLICKOFF);
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static void InstallMouseHookIfNeeded() {
    bool want = (U.expanded || U.halfOn) && !U.pinned && !Ghosted() && !U.dropping;
    if (want && !g_mouseHook) g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandleW(nullptr), 0);
    else if (!want && g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr; }
}

// Windows Security "Making sure it's you" prompts (CredentialUIBroker) on the desktop.
static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD ev, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !hwnd) return;
    if (ev == EVENT_OBJECT_SHOW) {
        if (!g_set.verifyAnimation || U.auth == 1) return;
        wchar_t cls[128] = L"";
        GetClassNameW(hwnd, cls, 128);
        if (wcscmp(cls, L"Credential Dialog Xaml Host") != 0) return;
        if (!U.auth) {  // remember exactly what was showing
            U.prev.valid = true;
            U.prev.expanded = U.expanded;
            U.prev.halfOn = U.halfOn;
            U.prev.tab = U.tab;
            U.prev.alertsScroll = U.alertsScroll;
            U.prev.clipScroll = U.clipScroll;
            U.prev.noteScroll = U.noteScroll;
            U.prev.halfScroll = U.halfScroll;
        }
        KillTimer(g_hwnd, TM_AUTHEND);
        U.auth = 1;
        U.authAt = NowMs();
        U.authHwnd = hwnd;
        U.expanded = false;
        U.ducking = false;
        Wake();
    } else if ((ev == EVENT_OBJECT_HIDE || ev == EVENT_OBJECT_DESTROY) && hwnd == U.authHwnd && U.auth == 1) {
        U.auth = 2;
        U.authAt = NowMs();
        U.authHwnd = nullptr;
        SetTimer(g_hwnd, TM_AUTHEND, 1200, nullptr);
        Wake();
    }
}

// ---- slow tick: clock, alarms, Focus / full screen, mic & camera ----

static void SlowTick() {
    g_now = NowMs();
    if (U.timerRun && !U.timerPaused && TimerRemaining() <= 0) {
        U.timerRun = false;
        InternalNotify(L"Timer done", L"Your " + FmtClock(U.timerDur) + L" timer is up");
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    int minuteKey = st.wHour * 60 + st.wMinute;
    if (minuteKey != U.lastAlarmMinute) {
        U.lastAlarmMinute = minuteKey;
        for (size_t i = 0; i < g_set.alarms.size(); i++) {
            auto& a = g_set.alarms[i];
            if (i < U.alarmOn.size() && U.alarmOn[i] && a.hour == st.wHour && a.minute == st.wMinute) {
                SYSTEMTIME at{};
                at.wHour = (WORD)a.hour;
                at.wMinute = (WORD)a.minute;
                InternalNotify(FmtTimeOfDay(at), a.label.empty() ? L"Alarm" : a.label);
            }
        }
    }
    QUERY_USER_NOTIFICATION_STATE qs = QUNS_ACCEPTS_NOTIFICATIONS;
    SHQueryUserNotificationState(&qs);
    bool fs = qs == QUNS_BUSY || qs == QUNS_RUNNING_D3D_FULL_SCREEN || qs == QUNS_PRESENTATION_MODE;
    bool dnd = qs == QUNS_QUIET_TIME;
    if (fs != U.fullscreen) {
        U.fullscreen = fs;
        if (fs) { U.expanded = false; U.halfOn = false; U.popActive = false; }
        Wake();
    }
    if (dnd != U.dnd) {
        U.dnd = dnd;
        if (!dnd && U.dndHeld) {
            Notif n;
            n.app = L"Do not disturb";
            n.title = L"While you were in Do not disturb";
            n.body = std::to_wstring(U.dndHeld) + L" notification" + (U.dndHeld == 1 ? L"" : L"s") + L" arrived";
            GetLocalTime(&n.at);
            U.dndHeld = 0;
            ShowPopup(n);
        }
        if (dnd) U.dndHeld = 0;
        Wake();
    }
    bool mic = CapabilityInUse(L"microphone"), cam = CapabilityInUse(L"webcam");
    if (mic != U.mic || cam != U.cam) { U.mic = mic; U.cam = cam; Wake(); }
    // Stay above other topmost windows (unless something is full screen).
    if (!U.fullscreen) SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    View v = CurrentView();
    if (v == V_HALF || v == V_LINE || (v == V_EXP && (U.tab == T_CLOCK || U.tab == T_MUSIC || U.tab == T_ALERTS || U.tab == T_CLIP))) Wake();
}

static void RegisterHotkeys() {
    UnregisterHotKey(g_hwnd, HK_CYCLE);
    UnregisterHotKey(g_hwnd, HK_LOCK);
    if (!g_set.shortcuts) return;
    if (!RegisterHotKey(g_hwnd, HK_CYCLE, MOD_ALT | MOD_NOREPEAT, g_set.shortcutKey)) Wh_Log(L"Alt+%c is taken by another app", g_set.shortcutKey);
    RegisterHotKey(g_hwnd, HK_LOCK, MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, g_set.shortcutKey);
}

// ---- persistence ----

static void SaveUiValues() {
    SaveValue(L"tab", std::to_wstring((int)U.tab));
    SaveValue(L"pinned", U.pinned ? L"1" : L"0");
    SaveValue(L"lyrics", U.lyricsOpen ? L"1" : L"0");
    SaveValue(L"notes", NoteJoined(false));
    wstring shelf;
    for (auto& p : U.shelf) shelf += p + L"\n";
    SaveValue(L"shelf", shelf);
}

static void LoadUiValues() {
    wstring t = LoadValue(L"tab");
    if (!t.empty()) U.tab = (Tab)std::clamp(_wtoi(t.c_str()), 0, (int)T_COUNT - 1);
    U.pinned = LoadValue(L"pinned") == L"1";
    U.lyricsOpen = LoadValue(L"lyrics") == L"1";
    wstring notes = LoadValue(L"notes");
    U.note.clear();
    size_t s = 0;
    while (true) {
        size_t e = notes.find(L'\n', s);
        U.note.push_back(notes.substr(s, e == wstring::npos ? wstring::npos : e - s));
        if (e == wstring::npos) break;
        s = e + 1;
    }
    wstring shelf = LoadValue(L"shelf");
    s = 0;
    while (s < shelf.size()) {
        size_t e = shelf.find(L'\n', s);
        wstring p = shelf.substr(s, e == wstring::npos ? wstring::npos : e - s);
        if (!p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) U.shelf.push_back(p);
        if (e == wstring::npos) break;
        s = e + 1;
    }
}

// ---- window procedure ----

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_TIMER:
            if (wp == TM_FRAME) OnFrame();
            else if (wp == TM_SLOW) SlowTick();
            else if (wp == TM_POINTER) PointerTick();
            else if (wp == TM_HOLD) {
                KillTimer(hwnd, TM_HOLD);
                if (g_holdDown && !g_holdOther && g_set.holdKey) { U.holdSee = true; Wake(); }
            } else if (wp == TM_AUTHEND) {
                KillTimer(hwnd, TM_AUTHEND);
                U.auth = 0;
                if (U.prev.valid) {  // back to exactly what was there before
                    U.expanded = U.prev.expanded;
                    U.halfOn = U.prev.halfOn;
                    U.tab = U.prev.tab;
                    U.alertsScroll = U.prev.alertsScroll;
                    U.clipScroll = U.prev.clipScroll;
                    U.noteScroll = U.prev.noteScroll;
                    U.halfScroll = U.prev.halfScroll;
                    U.prev.valid = false;
                }
                Wake();
            } else if (wp == TM_POPEND) {
                KillTimer(hwnd, TM_POPEND);
                Wake();
            } else if (wp == TM_SAVE) {
                KillTimer(hwnd, TM_SAVE);
                SaveUiValues();
            }
            return 0;
        case WM_MOUSEMOVE: {
            D2D1_POINT_2F p = ClientToCanvas(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            U.mouse = p;
            bool was = U.hover;
            U.hover = InIsland(p);
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            if (wp & MK_LBUTTON) {
                if (g_dragAct == A_M_SEEKBAR) SeekFromX(p.x, g_dragRect);
                else if (g_dragAct == A_LEVEL) LevelFromX(p.x, g_dragRect, g_dragArg);
                else if (g_shelfPress >= 0) {
                    POINT cur{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                    if (std::abs(cur.x - g_pressPt.x) + std::abs(cur.y - g_pressPt.y) > 6) {
                        int idx = g_shelfPress;
                        g_shelfPress = -1;
                        ReleaseCapture();
                        DragOutShelf(idx);
                    }
                }
            }
            if (was != U.hover || U.lastView == V_EXP || U.lastView == V_HALF) Wake();
            return 0;
        }
        case WM_MOUSELEAVE:
            U.hover = false;
            U.mouse = {-1, -1};
            Wake();
            return 0;
        case WM_LBUTTONDOWN: {
            D2D1_POINT_2F p = ClientToCanvas(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            const Hit* h = HitAt(p);
            g_dragAct = A_NONE;
            g_shelfPress = -1;
            if (h && (h->act == A_M_SEEKBAR || h->act == A_LEVEL)) {
                g_dragAct = h->act;
                g_dragArg = h->arg;
                g_dragRect = h->r;
                SetCapture(hwnd);
            } else if (h && h->act == A_S_OPEN) {
                g_shelfPress = h->arg;
                g_pressPt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                SetCapture(hwnd);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            bool wasDrag = g_dragAct != A_NONE;
            bool shelfPressed = g_shelfPress >= 0;
            g_dragAct = A_NONE;
            g_shelfPress = -1;
            if (GetCapture() == hwnd) ReleaseCapture();
            D2D1_POINT_2F p = ClientToCanvas(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (wasDrag) { if (const Hit* h = HitAt(p)) if (h->act == A_M_SEEKBAR || h->act == A_LEVEL) DoAction(*h, p); return 0; }
            (void)shelfPressed;
            if (InIsland(p) || HitAt(p)) OnLeftClick(p);
            return 0;
        }
        case WM_RBUTTONUP: OnRightClick(); return 0;
        case WM_CONTEXTMENU: return 0;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            OnWheel(ScreenToCanvas(pt), msg == WM_MOUSEHWHEEL ? -delta : delta, msg == WM_MOUSEHWHEEL);
            return 0;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (U.noteFocus && U.expanded && U.tab == T_NOTES) { NoteKey(wp); return 0; }
            break;
        case WM_CHAR:
            if (U.noteFocus && U.expanded && U.tab == T_NOTES) {
                if (wp == L'\r') NoteInsertText(L"\n");
                else if (wp >= 32) NoteInsertText(wstring(1, (wchar_t)wp));
                return 0;
            }
            break;
        case WM_KILLFOCUS:
            if (U.noteFocus) { U.noteFocus = false; SaveUiValues(); Wake(); }
            return 0;
        case WM_HOTKEY:
            g_now = NowMs();
            if (U.fullscreen || U.auth) return 0;
            if (wp == HK_CYCLE) {  // closed -> half -> full -> closed (half skipped when empty)
                if (U.expanded) CloseAll();
                else if (U.halfOn || !HalfHasContent()) { U.halfOn = false; U.expanded = true; U.levelUntil = 0; }
                else U.halfOn = true;
            } else if (wp == HK_LOCK) {
                U.seeLock = !U.seeLock;
            }
            Wake();
            return 0;
        case WM_ISLAND_HOLD:
            if (wp == 1) SetTimer(hwnd, TM_HOLD, 250, nullptr);
            else { KillTimer(hwnd, TM_HOLD); if (U.holdSee) { U.holdSee = false; Wake(); } }
            return 0;
        case WM_ISLAND_ESC:
            if (U.expanded || U.halfOn) CloseAll();
            return 0;
        case WM_ISLAND_CLICKOFF:
            if ((U.expanded || U.halfOn) && !U.pinned && !Ghosted()) CloseAll();
            return 0;
        case WM_ISLAND_NOTIF: {
            std::deque<Notif> got;
            {
                std::lock_guard<std::mutex> lk(g_mx);
                got.swap(g_incoming);
            }
            for (auto& n : got) AddNotification(n);
            return 0;
        }
        case WM_ISLAND_MEDIA: Wake(); return 0;
        case WM_ISLAND_LEVEL: {
            g_now = NowMs();
            if (wp == 0) { U.vol = LOWORD(lp); U.muted = (lp >> 16) & 1; }
            else U.bri = (int)lp;
            if (!g_set.levelPopup) return 0;
            if (U.levelKind != (int)wp) U.levelV.v = wp == 0 ? U.vol : U.bri;
            U.levelKind = (int)wp;
            if (U.expanded) U.inlineUntil = g_now + 1600;
            else U.levelUntil = g_now + 1600;
            Wake();
            return 0;
        }
        case WM_ISLAND_REBIND:
            BindVolume();
            U.vol = GetVolume(&U.muted);
            return 0;
        case WM_ISLAND_SETTINGS:
            LoadSettings();
            U.alarmOn.resize(g_set.alarms.size());
            for (size_t i = 0; i < g_set.alarms.size(); i++) U.alarmOn[i] = g_set.alarms[i].enabled;
            RegisterHotkeys();
            Wake();
            return 0;
        case WM_CLIPBOARDUPDATE: OnClipboardUpdate(); return 0;
        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
        case WM_SETTINGCHANGE:
            Reposition();
            return msg == WM_SETTINGCHANGE ? DefWindowProcW(hwnd, msg, wp, lp) : 0;
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ============================================================================
// Threads and lifetime
// ============================================================================

static std::thread g_uiThread, g_notifThread, g_mediaThread, g_briThread;

static void UiThread() {
    using SetCtx_t = HANDLE(WINAPI*)(HANDLE);
    if (auto setCtx = (SetCtx_t)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext"))
        setCtx((HANDLE)-4 /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */);
    OleInitialize(nullptr);
    LoadUiValues();
    U.alarmOn.resize(g_set.alarms.size());
    for (size_t i = 0; i < g_set.alarms.size(); i++) U.alarmOn[i] = g_set.alarms[i].enabled;
    if (!InitGraphics()) {
        Wh_Log(L"Direct2D/DirectWrite failed to start");
        OleUninitialize();
        return;
    }
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512) /* IDC_ARROW */);
    wc.lpszClassName = L"IslandNotchWindow";
    RegisterClassExW(&wc);
    g_hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, wc.lpszClassName, L"Island", WS_POPUP, 0, 0, 10, 10,
                             nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd) {
        Wh_Log(L"CreateWindowEx failed: %u", GetLastError());
        ShutdownGraphics();
        OleUninitialize();
        return;
    }
    U.w.v = 90;
    Reposition();
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    BindVolume();
    U.vol = GetVolume(&U.muted);
    U.levelV.v = U.vol;
    g_drop = new DropTarget();
    RegisterDragDrop(g_hwnd, g_drop);
    AddClipboardFormatListener(g_hwnd);
    RegisterHotkeys();
    g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, GetModuleHandleW(nullptr), 0);
    g_winEvHook = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE, nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    SetTimer(g_hwnd, TM_SLOW, 1000, nullptr);
    SlowTick();

    g_mediaWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_notifThread = std::thread(NotificationThread);
    g_mediaThread = std::thread(MediaThread);
    g_briThread = std::thread(BrightnessThread);
    Wake();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_stop = true;
    if (g_mediaWake) SetEvent(g_mediaWake);
    SaveUiValues();
    if (g_kbHook) UnhookWindowsHookEx(g_kbHook);
    if (g_mouseHook) UnhookWindowsHookEx(g_mouseHook);
    if (g_winEvHook) UnhookWinEvent(g_winEvHook);
    ShutdownVolume();
    g_hwnd = nullptr;
    if (g_notifThread.joinable()) g_notifThread.join();
    if (g_mediaThread.joinable()) g_mediaThread.join();
    if (g_briThread.joinable()) g_briThread.join();
    if (g_drop) { g_drop->Release(); g_drop = nullptr; }
    ShutdownGraphics();
    OleUninitialize();
}

static bool WhTool_ModInit() {
    LoadSettings();
    g_uiThread = std::thread(UiThread);
    return true;
}

static void WhTool_ModSettingsChanged() {
    if (g_hwnd) PostMessageW(g_hwnd, WM_ISLAND_SETTINGS, 0, 0);
}

static void WhTool_ModUninit() {
    HWND h = g_hwnd;
    if (h) PostMessageW(h, WM_CLOSE, 0, 0);
    if (g_uiThread.joinable()) g_uiThread.join();
}

// ============================================================================
// Windhawk tool-mod boilerplate: the mod runs in its own windhawk.exe process
// ============================================================================

static bool g_isToolModProcessLauncher;
static HANDLE g_toolModProcessMutex;

static void WINAPI EntryPoint_Hook() {
    Wh_Log(L">");
    ExitThread(0);
}

BOOL Wh_ModInit() {
    bool isService = false, isToolModProcess = false, isCurrentToolModProcess = false;
    int argc;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        Wh_Log(L"CommandLineToArgvW failed");
        return FALSE;
    }
    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"-service") == 0) {
            isService = true;
            break;
        }
    }
    for (int i = 1; i < argc - 1; i++) {
        if (wcscmp(argv[i], L"-tool-mod") == 0) {
            isToolModProcess = true;
            if (wcscmp(argv[i + 1], WH_MOD_ID) == 0) isCurrentToolModProcess = true;
            break;
        }
    }
    LocalFree(argv);
    if (isService) return FALSE;
    if (isCurrentToolModProcess) {
        g_toolModProcessMutex = CreateMutexW(nullptr, TRUE, L"windhawk-tool-mod_" WH_MOD_ID);
        if (!g_toolModProcessMutex) {
            Wh_Log(L"CreateMutex failed");
            ExitProcess(1);
        }
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            Wh_Log(L"Tool mod already running (%s)", WH_MOD_ID);
            ExitProcess(1);
        }
        if (!WhTool_ModInit()) ExitProcess(1);
        IMAGE_DOS_HEADER* dosHeader = (IMAGE_DOS_HEADER*)GetModuleHandleW(nullptr);
        IMAGE_NT_HEADERS* ntHeaders = (IMAGE_NT_HEADERS*)((BYTE*)dosHeader + dosHeader->e_lfanew);
        DWORD entryPointRVA = ntHeaders->OptionalHeader.AddressOfEntryPoint;
        void* entryPoint = (BYTE*)dosHeader + entryPointRVA;
        Wh_SetFunctionHook(entryPoint, (void*)EntryPoint_Hook, nullptr);
        return TRUE;
    }
    if (isToolModProcess) return FALSE;
    g_isToolModProcessLauncher = true;
    return TRUE;
}

void Wh_ModAfterInit() {
    if (!g_isToolModProcessLauncher) return;
    WCHAR currentProcessPath[MAX_PATH];
    switch (GetModuleFileNameW(nullptr, currentProcessPath, ARRAYSIZE(currentProcessPath))) {
        case 0:
        case ARRAYSIZE(currentProcessPath):
            Wh_Log(L"GetModuleFileName failed");
            return;
    }
    WCHAR commandLine[MAX_PATH + 2 + (sizeof(L" -tool-mod \"" WH_MOD_ID "\"") / sizeof(WCHAR)) - 1];
    swprintf_s(commandLine, L"\"%s\" -tool-mod \"%s\"", currentProcessPath, WH_MOD_ID);
    HMODULE kernelModule = GetModuleHandleW(L"kernelbase.dll");
    if (!kernelModule) {
        kernelModule = GetModuleHandleW(L"kernel32.dll");
        if (!kernelModule) {
            Wh_Log(L"No kernelbase.dll/kernel32.dll");
            return;
        }
    }
    using CreateProcessInternalW_t = BOOL(WINAPI*)(HANDLE hUserToken, LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
                                                    LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes, WINBOOL bInheritHandles,
                                                    DWORD dwCreationFlags, LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory, LPSTARTUPINFOW lpStartupInfo,
                                                    LPPROCESS_INFORMATION lpProcessInformation, PHANDLE hRestrictedUserToken);
    CreateProcessInternalW_t pCreateProcessInternalW = (CreateProcessInternalW_t)GetProcAddress(kernelModule, "CreateProcessInternalW");
    if (!pCreateProcessInternalW) {
        Wh_Log(L"No CreateProcessInternalW");
        return;
    }
    STARTUPINFOW si{};
    si.cb = sizeof(STARTUPINFOW);
    si.dwFlags = STARTF_FORCEOFFFEEDBACK;
    PROCESS_INFORMATION pi;
    if (!pCreateProcessInternalW(nullptr, currentProcessPath, commandLine, nullptr, nullptr, FALSE, NORMAL_PRIORITY_CLASS, nullptr, nullptr, &si, &pi,
                                 nullptr)) {
        Wh_Log(L"CreateProcess failed");
        return;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

void Wh_ModSettingsChanged() {
    if (g_isToolModProcessLauncher) return;
    WhTool_ModSettingsChanged();
}

void Wh_ModUninit() {
    if (g_isToolModProcessLauncher) return;
    WhTool_ModUninit();
    ExitProcess(0);
}
