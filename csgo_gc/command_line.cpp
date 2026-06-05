#include "stdafx.h"
#include "command_line.h"
#include "platform.h"

#if defined(_WIN32)

#include <windows.h>
#include <funchook.h>
#include <string>

namespace
{
    using GetCommandLineAFn = LPSTR(WINAPI *)();
    using GetCommandLineWFn = LPWSTR(WINAPI *)();

    GetCommandLineAFn Og_GetCommandLineA = nullptr;
    GetCommandLineWFn Og_GetCommandLineW = nullptr;

    // Augmented command lines, built once and kept for the process lifetime.
    char *s_cmdLineA = nullptr;
    wchar_t *s_cmdLineW = nullptr;

    LPSTR WINAPI Hk_GetCommandLineA()
    {
        return s_cmdLineA ? s_cmdLineA : Og_GetCommandLineA();
    }

    LPWSTR WINAPI Hk_GetCommandLineW()
    {
        return s_cmdLineW ? s_cmdLineW : Og_GetCommandLineW();
    }

    // Resolve the real kernelbase function (GetProcAddress follows the kernel32
    // forwarder), which is what every caller in the process ends up at.
    void *ResolveExport(const char *name)
    {
        if (HMODULE m = GetModuleHandleW(L"kernelbase.dll"))
        {
            if (void *p = reinterpret_cast<void *>(GetProcAddress(m, name)))
            {
                return p;
            }
        }
        if (HMODULE m = GetModuleHandleW(L"kernel32.dll"))
        {
            return reinterpret_cast<void *>(GetProcAddress(m, name));
        }
        return nullptr;
    }
}

void InstallCommandLineHook()
{
    {
        const char *orig = GetCommandLineA();
        std::string line = orig ? orig : "";
        if (line.find("-steam") == std::string::npos)
        {
            line += " -steam";
        }
        s_cmdLineA = _strdup(line.c_str());
    }
    {
        const wchar_t *orig = GetCommandLineW();
        std::wstring line = orig ? orig : L"";
        if (line.find(L"-steam") == std::wstring::npos)
        {
            line += L" -steam";
        }
        s_cmdLineW = _wcsdup(line.c_str());
    }

    void *targetA = ResolveExport("GetCommandLineA");
    void *targetW = ResolveExport("GetCommandLineW");

    funchook_t *funchook = funchook_create();
    if (!funchook || !targetA || !targetW)
    {
        Platform::Print("command line hook: unavailable, -steam not forced\n");
        return;
    }

    void *bridgeA = targetA;
    void *bridgeW = targetW;
    if (funchook_prepare(funchook, &bridgeA, reinterpret_cast<void *>(Hk_GetCommandLineA)) != 0
        || funchook_prepare(funchook, &bridgeW, reinterpret_cast<void *>(Hk_GetCommandLineW)) != 0
        || funchook_install(funchook, 0) != 0)
    {
        Platform::Print("command line hook: funchook failed, -steam not forced: %s\n",
            funchook_error_message(funchook));
        return;
    }

    Og_GetCommandLineA = reinterpret_cast<GetCommandLineAFn>(bridgeA);
    Og_GetCommandLineW = reinterpret_cast<GetCommandLineWFn>(bridgeW);
    Platform::Print("command line: -steam enforced\n");
}

#else // !_WIN32

// The unix launcher injects -steam into argv directly (launcher_unix.cpp), which
// the engine uses, so no hook is needed here.
void InstallCommandLineHook()
{
}

#endif
