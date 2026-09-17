// feed_mgpu.h -- MGPU Bridge (maohgad-web/Neural-coprocessor) as a neural consumer, producer side.
//
// MGPU Bridge is a D3D12-only ReShade add-on (nvngx.dll_mgpu_bridge.addon64 -- the file name is
// load-bearing on its side) that runs the DLSS 5 neural model on a SECOND GPU and presents the
// result in a window of its own. It is not an NGX detour and not an NGX implementation: it reads
// the finished frame at reshade_finish_effects, reads depth from its own effect
// (mgpu_depth_tap.fx, texture MGPU_DepthOutTex), and gets motion vectors in exactly one way --
// its "calibrator" swaps GetProcAddress in every module's import table (and swaps already-cached
// pointers in every module's writable data), so NVSDK_NGX_D3D12_CreateFeature and
// NVSDK_NGX_D3D12_EvaluateFeature land in its hooks first. At an evaluate of a feature it saw
// being CREATED (ids 1 and 13 only, at most four handles, never released) it records, on the
// caller's command list, a barrier PIXEL|NON_PIXEL shader resource -> COPY_SOURCE around a copy
// of the MotionVectors resource, once per frame.
//
// That is the call this project already makes every frame, with validated vectors in exactly
// that state, so there is no protocol here either -- like feed_opti.h this is a set of checks:
//   1. is an MGPU Bridge add-on beside us / loaded in this process;
//   2. is its install laid out the way IT needs (nvngx_dlssnr.dll in a subfolder beside the
//      add-on and NOT beside the exe -- the opposite of what renodx-dlss5 and Deep Fried Chicken
//      want; ReShade2.ini pointing at gpu1.ini; mgpu.ini beside the add-on);
//   3. does mgpu.ini leave the evaluate tap on (Calib, MVec, MvecFromEval), and does its
//      DepthInverted agree with what this add-on tells DLSS.
//
// mgpu.ini is read with MGPU's own rules, not GetPrivateProfile: first line-start `key=` wins,
// ';' and '#' comment a whole line, the [MGPU] header is decoration, and an ABSENT key has a
// compiled default that is not always the shipped one (src/probe.cpp, src/gpu1_context.cpp at
// 0.2.3): Calib 2, CalibRung 0, MVec 0, MvecFromEval 0, Depth 0, DepthInverted 1, SRUpscale 0,
// NoActivate 2.
//
// Nothing here ever writes to mgpu.ini or to any other MGPU file.
//
// UNVERIFIED END TO END. MGPU refuses to arm without a second neural-capable GPU, and no
// machine this project has access to has one; what is measured is its source (0.2.3) and what
// its calibrator logs on a single-GPU rig.
//
// Requires <windows.h>. No NGX headers: all three translation units include it. Header-only and
// static, like its siblings.

#pragma once

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define MGPU_LABEL        "MGPU Bridge"
#define MGPU_INI          "mgpu.ini"
#define MGPU_TAP_FX       "mgpu_depth_tap.fx"
#define MGPU_SNIPPET      "nvngx_dlssnr.dll"
#define MGPU_WNDCLASS     "MGPU_Bridge_Wnd_"   // its GPU-1 window: "MGPU_Bridge_Wnd_<module handle>"
#define MGPU_HANDLE_SLOTS 4                    // scene-feature handles its calibrator can latch, for the process lifetime

struct MgpuIni
{
    bool found;
    char path[MAX_PATH];
    int  calib;           // 0 = the NGX tap is never installed: no vectors from anyone's evaluate
    int  calib_rung;      // 0 both, 1 import swap alone, 2 cached-pointer scan alone
    int  mvec;            // 3 ("real") is the only mode that takes the evaluate's vectors
    int  mvec_from_eval;  // 0 never, 1 from the start, 2 auto (arms itself once the barrier route measures dead)
    int  depth;           // 0 off, 1 waits for its tap to be bound before arming, 2 transport-only
    int  depth_inverted;  // what IT tells DLSS-NR about depth
    int  sr_upscale;
    int  no_activate;
};

struct MgpuInfo
{
    bool present;              // an MGPU Bridge add-on file sits in the folder
    int  addon_files;          // more than one is two copies fighting over one window
    char addon[96];            // its file name
    char dir[MAX_PATH];        // the folder, trailing backslash
    bool snippet_private;      // nvngx_dlssnr.dll in mgpu\ (or any immediate subfolder -- MGPU looks in all of them)
    char snippet_dir[64];
    bool snippet_beside_exe;   // MGPU's red "INSTALL PROBLEM"
    bool reshade2_ini;         // ReShade2.ini: the config ReShade gives the SECOND runtime in the process
    bool reshade2_preset;      // ... and it points PresetPath at gpu1.ini
    bool gpu1_ini;
    MgpuIni ini;
};

// First active line-start `key=` in a NUL-terminated buffer, MGPU's own syntax (mgpu_ini_parser.hpp).
static inline const char *MgpuIniFind(const char *data, const char *key)
{
    const size_t kn = strlen(key);
    const char *p = data;
    if (static_cast<unsigned char>(p[0]) == 0xEF && static_cast<unsigned char>(p[1]) == 0xBB &&
        static_cast<unsigned char>(p[2]) == 0xBF) p += 3;
    while (*p != 0)
    {
        const char *q = p;
        while (*q == ' ' || *q == '\t') ++q;
        if (*q != ';' && *q != '#' && strncmp(q, key, kn) == 0 && q[kn] == '=') return q + kn + 1;
        while (*p != 0 && *p != '\n') ++p;
        if (*p == '\n') ++p;
    }
    return nullptr;
}

static inline int MgpuIniInt(const char *data, const char *key, int absent, int lo, int hi, int out_of_range)
{
    const char *v = MgpuIniFind(data, key);
    if (v == nullptr) return absent;
    const int n = atoi(v);
    return (n < lo || n > hi) ? out_of_range : n;
}

static inline void MgpuReadIni(const char *dir, MgpuIni *o)
{
    *o = MgpuIni{};
    o->calib = 2; o->depth_inverted = 1; o->no_activate = 2;
    _snprintf_s(o->path, sizeof(o->path), _TRUNCATE, "%s" MGPU_INI, dir);
    HANDLE f = CreateFileA(o->path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    static char buf[64 * 1024];
    DWORD got = 0;
    const BOOL ok = ReadFile(f, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(f);
    if (!ok) return;
    buf[got] = 0;
    o->found          = true;
    o->calib          = MgpuIniInt(buf, "Calib", 2, 0, 2, 2);
    o->calib_rung     = MgpuIniInt(buf, "CalibRung", 0, 0, 2, 0);
    o->mvec_from_eval = MgpuIniInt(buf, "MvecFromEval", 0, 0, 2, 0);
    o->depth          = MgpuIniInt(buf, "Depth", 0, 0, 2, 0);
    o->depth_inverted = MgpuIniInt(buf, "DepthInverted", 1, 0, 1, 1);
    o->sr_upscale     = MgpuIniInt(buf, "SRUpscale", 0, 0, 1, 1);
    o->no_activate    = MgpuIniInt(buf, "NoActivate", 2, 0, 2, 2);
    // MVec also takes words: s(ynthetic) 1, e(...) 2, r(eal) 3, o(ff) 0.
    o->mvec = 0;
    if (const char *v = MgpuIniFind(buf, "MVec"))
    {
        const char c = static_cast<char>(*v | 0x20);
        o->mvec = c == 's' ? 1 : c == 'e' ? 2 : c == 'r' ? 3 : c == 'o' ? 0 : atoi(v);
        if (o->mvec < 0) o->mvec = 0;
        if (o->mvec > 3) o->mvec = 3;
    }
}

// Will an NGX evaluate made in this process reach MGPU's vector copy, as far as the ini decides?
static inline bool MgpuIniTakesEvalVectors(const MgpuIni &i)
{
    return i.calib != 0 && i.mvec == 3 && i.mvec_from_eval != 0;
}

static inline bool MgpuFileExists(const char *path)
{
    const DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static inline bool MgpuContainsNoCase(const char *hay, const char *needle)
{
    const size_t n = strlen(needle);
    for (const char *p = hay; *p != 0; ++p)
        if (_strnicmp(p, needle, n) == 0) return true;
    return false;
}

// The folder scan. `dir` has a trailing backslash and is where ReShade loads add-ons from -- for
// the 64-bit add-on its own folder, for the helper host64\. `exe_dir` is the running exe's folder
// (the same one in every layout this project deploys, but MGPU tests them separately, so do we).
static inline bool MgpuScan(const char *dir, const char *exe_dir, MgpuInfo *o)
{
    *o = MgpuInfo{};
    strcpy_s(o->dir, dir);
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s*.addon64", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(path, &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!MgpuContainsNoCase(fd.cFileName, "mgpu_bridge") && !MgpuContainsNoCase(fd.cFileName, "mgpu-bridge")) continue;
            if (o->addon_files++ == 0) strcpy_s(o->addon, fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    o->present = o->addon_files > 0;
    if (!o->present) return false;

    MgpuReadIni(dir, &o->ini);

    // The private snippet: mgpu\ first, then any immediate subfolder, as MGPU's own lookup does.
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%smgpu\\" MGPU_SNIPPET, dir);
    if (MgpuFileExists(path)) { o->snippet_private = true; strcpy_s(o->snippet_dir, "mgpu"); }
    else
    {
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s*", dir);
        h = FindFirstFileA(path, &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 || fd.cFileName[0] == '.') continue;
                char sub[MAX_PATH];
                _snprintf_s(sub, sizeof(sub), _TRUNCATE, "%s%s\\" MGPU_SNIPPET, dir, fd.cFileName);
                if (!MgpuFileExists(sub)) continue;
                o->snippet_private = true;
                strncpy_s(o->snippet_dir, fd.cFileName, _TRUNCATE);
                break;
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s" MGPU_SNIPPET, exe_dir);
    o->snippet_beside_exe = MgpuFileExists(path);

    _snprintf_s(path, sizeof(path), _TRUNCATE, "%sReShade2.ini", dir);
    o->reshade2_ini = MgpuFileExists(path);
    if (o->reshade2_ini)
    {
        char preset[MAX_PATH] = {};
        GetPrivateProfileStringA("GENERAL", "PresetPath", "", preset, sizeof(preset), path);
        o->reshade2_preset = MgpuContainsNoCase(preset, "gpu1.ini");
    }
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%sgpu1.ini", dir);
    o->gpu1_ini = MgpuFileExists(path);
    return true;
}

// The loaded module, if ReShade has loaded it yet: the add-on whose NAME export reads "MGPU Bridge".
// Add-ons load in directory order, so at OUR attach it may simply not be there yet -- ask again later.
static inline HMODULE MgpuLoadedModule(const MgpuInfo &i)
{
    if (!i.present) return nullptr;
    HMODULE m = GetModuleHandleA(i.addon);
    if (m == nullptr) return nullptr;
    const char *const *name = reinterpret_cast<const char *const *>(GetProcAddress(m, "NAME"));
    if (name == nullptr || *name == nullptr || strcmp(*name, MGPU_LABEL) != 0) return nullptr;
    return m;
}

static inline bool MgpuIsBridgeWindowClass(const char *wclass)
{
    return wclass != nullptr && strncmp(wclass, MGPU_WNDCLASS, sizeof(MGPU_WNDCLASS) - 1) == 0;
}

// Everything the file scan alone can say, in the caller's own log. `warn` and `log` are the
// caller's Warn()/Log(), so each side keeps its prefix; `where` names the folder for the reader
// ("next to this add-on", "in host64\").
static inline void MgpuReportLayout(const MgpuInfo &i, const char *tag, const char *where,
                                    void (*log)(const char *, ...), void (*warn)(const char *, ...))
{
    log("[%s] %s (%s) is %s. mgpu.ini: %s -- Calib=%d CalibRung=%d MVec=%d MvecFromEval=%d Depth=%d DepthInverted=%d "
        "SRUpscale=%d; " MGPU_SNIPPET ": %s%s%s; ReShade2.ini %s, gpu1.ini %s",
        tag, MGPU_LABEL, i.addon, where, i.ini.found ? "found" : "NOT FOUND (its compiled defaults apply)",
        i.ini.calib, i.ini.calib_rung, i.ini.mvec, i.ini.mvec_from_eval, i.ini.depth, i.ini.depth_inverted,
        i.ini.sr_upscale, i.snippet_private ? "in " : "NOT in a subfolder beside the add-on",
        i.snippet_private ? i.snippet_dir : "", i.snippet_private ? "\\" : "",
        !i.reshade2_ini ? "missing" : i.reshade2_preset ? "present (PresetPath -> gpu1.ini)" : "present, but PresetPath is not gpu1.ini",
        i.gpu1_ini ? "present" : "missing");
    if (i.addon_files > 1)
        warn("%d MGPU Bridge add-on files are %s. ReShade loads every one of them, and each opens its own window and "
             "patches every import table in the process. Keep one.", i.addon_files, where);
    if (!i.ini.found)
        warn("MGPU Bridge is %s but mgpu.ini is not beside it. Without the file its MVec and MvecFromEval default to 0, "
             "so it takes no motion vectors from anyone; it also shows ERROR 205 on its window. Copy mgpu.ini from its "
             "release zip next to %s.", where, i.addon);
    else if (!MgpuIniTakesEvalVectors(i.ini))
        warn("mgpu.ini has Calib=%d MVec=%d MvecFromEval=%d: with these MGPU Bridge never copies motion vectors out of an "
             "NGX evaluate, which is the only way it can get them from this feed. It needs Calib=1 or 2, MVec=3 and "
             "MvecFromEval=1 or 2 (its shipped defaults are 2, 3, 2). This add-on never edits mgpu.ini.",
             i.ini.calib, i.ini.mvec, i.ini.mvec_from_eval);
    if (i.snippet_beside_exe)
        warn(MGPU_SNIPPET " sits beside the executable. MGPU Bridge reports that as INSTALL PROBLEM: it must hold the only "
             "copy, in a subfolder beside its add-on (mgpu\\), so that NGX binds the neural model to the SECOND GPU. "
             "renodx-dlss5 and Deep Fried Chicken want the opposite, which is one more reason to run exactly one "
             "consumer. Move it into mgpu\\.");
    else if (!i.snippet_private)
        warn(MGPU_SNIPPET " is not in mgpu\\ beside %s (nor in any other subfolder there). MGPU Bridge cannot create the "
             "neural model without it.", i.addon);
    if (!i.reshade2_ini || !i.reshade2_preset || !i.gpu1_ini)
        warn("MGPU Bridge's window is a second swapchain, so ReShade builds a second effect runtime for it and configures "
             "it from ReShade2.ini. %s -- without its own ReShade2.ini (PresetPath=.\\gpu1.ini) and gpu1.ini that runtime "
             "gets ReShade's defaults, and whatever it enables is drawn over the neural output. Copy both from MGPU's "
             "release zip.", !i.reshade2_ini ? "ReShade2.ini is missing" : !i.gpu1_ini ? "gpu1.ini is missing"
                                                                        : "ReShade2.ini does not point PresetPath at gpu1.ini");
}
