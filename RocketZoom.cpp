// RocketZoom.asi - GTA San Andreas PC v1.0 (US)
//
// When aiming with the rocket launcher (RPG) or the Heat-Seeker, you can zoom in / zoom out just like the
// game's camera, with the same limits and the same speed:
//   Zoom IN : mouse wheel up   | RT (R2)
//   Zoom OUT: mouse wheel down | LT (L2)
// (GInput controller support: the plugin reads the state of the game's own CPad)
//
// Everything below was confirmed in gta_sa.exe 1.0 US:
//  - The camera (mode 46) and sniper (mode 7) use the same zoom code:
//      maximum FOV 70 degrees; minimum 3.0 degrees for the camera (15.0 for the sniper);
//      buttons: every frame FOV *= (1 + 0.0255*timestep) (zoom out) or /= (zoom in);
//      mouse wheel: factor (|Z|*7 + 10000) * 0.0001 per "scroll" (Z = CPad mouse +0xB73420).
//    In all other modes (including the rocket launcher mode), the game locks the FOV to 70 every frame.
//  - Rocket launcher aiming camera modes: 8 (RPG) and 51/0x33 (Heat-Seeker).
//    Active mode: word at CCamera(0xB6F028) + 0x180 + (byte at +0x59) * 0x238.
//  - CCamera::Process = 0x52B730. At the end of it, the final FOV is sent to CDraw::SetFOV (0x6FF410)
//    by the call at 0x52C976 (cdecl, 1 float). The plugin intercepts this call and multiplies the FOV
//    by (zoom / 70).
//  - CTimer::ms_fTimeStep = 0xB7CB5C, CTimer::m_FrameCounter = 0xB7CB4C, player CPad = 0xB73458
//    (L2 +0x0A, R2 +0x0E; shorts).
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

static const uintptr_t VERSION_ADDR = 0x82457C;
static const uint32_t  VERSION_OK   = 0x94BF;
static const uintptr_t SITE_SETFOV  = 0x52C976;
static const uintptr_t FN_SETFOV    = 0x6FF410;
static const uintptr_t THE_CAMERA   = 0xB6F028;
static const uintptr_t PAD0         = 0xB73458;
static const uintptr_t MOUSE_Z      = 0xB73420;
static const uintptr_t TIMESTEP     = 0xB7CB5C;
static const uintptr_t FRAME_COUNT  = 0xB7CB4C;

static const int MODE_ROCKET    = 8;
static const int MODE_ROCKET_HS = 51;

typedef void (__cdecl *SetFOV_t)(float fov);

static HMODULE hSelf = NULL;
static char    gIni[MAX_PATH];
static bool    gLog = false;

static float gMinFov   = 3.0f;    // same limit as the camera
static float gMaxFov   = 70.0f;
static float gSpeed    = 1.0f;
static bool  gWheel    = true;
static bool  gPadKeys  = true;
static bool  gHeatSeek = true;

static float    gTarget = 70.0f;       // target FOV
static float    gCur    = 70.0f;       // current FOV (smoothed)
static unsigned gLastFrame = 0xFFFFFFFFu;
static int      gLogCount = 0;

static void Log(const char* fmt, ...)
{
    if (!gLog) return;
    char path[MAX_PATH];
    if (!GetModuleFileNameA(hSelf, path, MAX_PATH)) return;
    char* dot = strrchr(path, '.');
    if (dot) strcpy(dot, ".log"); else strcat(path, ".log");
    FILE* f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "[%lu] ", GetTickCount());
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

static int ActiveCamMode()
{
    uint8_t active = *(volatile uint8_t*)(THE_CAMERA + 0x59);
    if (active > 2) return -1;
    return *(volatile uint16_t*)(THE_CAMERA + 0x180 + (uintptr_t)active * 0x238);
}

static bool AimingRocket(int mode)
{
    return mode == MODE_ROCKET || (gHeatSeek && mode == MODE_ROCKET_HS);
}

static inline float Clamp(float v) { return v < gMinFov ? gMinFov : (v > gMaxFov ? gMaxFov : v); }

// Updates the zoom ONCE per game frame
static void UpdateZoom()
{
    const float ts = *(volatile float*)TIMESTEP;
    float tgt = gTarget;

    if (gWheel)
    {
        float z = *(volatile float*)MOUSE_Z;
        if (z != 0.0f)
        {
            float f = (fabsf(z) * 7.0f + 10000.0f) * 0.0001f;       // same formula as the game's camera
            f = 1.0f + (f - 1.0f) * gSpeed;
            tgt = (z > 0.0f) ? tgt / f : tgt * f;
        }
    }
    if (gPadKeys)
    {
        volatile short* pad = (volatile short*)PAD0;
        bool zin  = pad[0x0E / 2] > 30;                              // RT (R2) = zoom in
        bool zout = pad[0x0A / 2] > 30;                              // LT (L2) = zoom out
        float f = (ts * 255.0f + 10000.0f) * 0.0001f;                // same formula as the game's camera
        f = 1.0f + (f - 1.0f) * gSpeed;
        if (zin && !zout)      tgt /= f;
        else if (zout && !zin) tgt *= f;
    }
    gTarget = Clamp(tgt);
    gCur += (gTarget - gCur) * 0.35f;                                // smooths the mouse wheel zoom changes
    if (fabsf(gTarget - gCur) < 0.01f) gCur = gTarget;
}

// Replaces "call CDraw::SetFOV" at the end of CCamera::Process
static void __cdecl HookSetFOV(float fov)
{
    int mode = ActiveCamMode();
    if (AimingRocket(mode))
    {
        unsigned frame = *(volatile unsigned*)FRAME_COUNT;
        if (frame != gLastFrame) { gLastFrame = frame; UpdateZoom(); }
        float out = fov * (gCur / 70.0f);
        if (gLogCount < 6 && fabsf(gCur - 70.0f) > 0.5f)
        { ++gLogCount; Log("Zoom no lanca-foguetes: modo=%d, FOV %.1f -> %.1f", mode, fov, out); }
        fov = out;
    }
    else
    {
        gTarget = gCur = gMaxFov;                                    // every aiming mode starts without zoom
        gLastFrame = 0xFFFFFFFFu;
    }
    ((SetFOV_t)FN_SETFOV)(fov);
}

static uintptr_t CallTarget(uintptr_t site)
{
    uint8_t* c = (uint8_t*)site;
    if (c[0] != 0xE8) return 0;
    return site + 5 + (uintptr_t)(intptr_t)(*(int32_t*)(c + 1));
}

static bool WriteCall(uintptr_t at, void* target)
{
    DWORD old;
    if (!VirtualProtect((void*)at, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    *(uint8_t*)at = 0xE8;
    *(int32_t*)(at + 1) = (int32_t)((uintptr_t)target - (at + 5));
    VirtualProtect((void*)at, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, 5);
    return true;
}

static float ReadFloat(const char* key, float def)
{
    char buf[32], d[32];
    sprintf(d, "%g", def);
    GetPrivateProfileStringA("RocketZoom", key, d, buf, sizeof buf, gIni);
    return (float)atof(buf);
}

BOOL APIENTRY DllMain(HMODULE hm, DWORD reason, LPVOID)
{
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(hm);
    hSelf = hm;

    GetModuleFileNameA(hm, gIni, MAX_PATH);
    char* dot = strrchr(gIni, '.');
    if (dot) strcpy(dot, ".ini"); else strcat(gIni, ".ini");

    gLog = GetPrivateProfileIntA("RocketZoom", "Log", 0, gIni) != 0;
    if (!GetPrivateProfileIntA("RocketZoom", "Enable", 1, gIni)) { Log("Desativado no .ini"); return TRUE; }
    if (*(uint32_t*)VERSION_ADDR != VERSION_OK) { Log("Versao do exe nao reconhecida (esperado 1.0 US). Mod desativado."); return TRUE; }

    gMinFov   = ReadFloat("MinFov", 3.0f);
    gMaxFov   = ReadFloat("MaxFov", 70.0f);
    gSpeed    = ReadFloat("Speed", 1.0f);
    gWheel    = GetPrivateProfileIntA("RocketZoom", "MouseWheel", 1, gIni) != 0;
    gPadKeys  = GetPrivateProfileIntA("RocketZoom", "PadButtons", 1, gIni) != 0;
    gHeatSeek = GetPrivateProfileIntA("RocketZoom", "HeatSeeker", 1, gIni) != 0;
    if (gMinFov < 1.0f) gMinFov = 1.0f;
    if (gMaxFov > 70.0f) gMaxFov = 70.0f;
    if (gMinFov > gMaxFov) gMinFov = gMaxFov;
    if (gSpeed < 0.1f) gSpeed = 0.1f;
    if (gSpeed > 10.0f) gSpeed = 10.0f;
    gTarget = gCur = gMaxFov;

    // Verify the game's code before modifying it
    if (CallTarget(SITE_SETFOV) != FN_SETFOV)
    {
        Log("Chamada em 0x%08X nao aponta para CDraw::SetFOV (alvo=0x%08X). Outro mod pode ter alterado a area. Mod desativado.",
            (unsigned)SITE_SETFOV, (unsigned)CallTarget(SITE_SETFOV));
        return TRUE;
    }
    bool ok = WriteCall(SITE_SETFOV, (void*)&HookSetFOV);
    Log("Ativo: FOV %.1f..%.1f, velocidade %.2f, roda=%d, botoes=%d, heatseeker=%d. Patch: %s",
        gMinFov, gMaxFov, gSpeed, (int)gWheel, (int)gPadKeys, (int)gHeatSeek, ok ? "OK" : "FALHOU");
    return TRUE;
}