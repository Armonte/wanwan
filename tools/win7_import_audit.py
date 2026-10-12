#!/usr/bin/env python3
"""Audit PE imports for anything a stock Windows 7 SP1 does not export.

Usage:
    python3 -I tools/win7_import_audit.py dist/FM2K_RollbackLauncher.exe dist/FM2KHook.dll ...

Three independent checks, because no single one is complete:
  1. DLLs that do not exist on stock Win7 SP1 (shcore, combase, ucrtbase,
     vcruntime140, d3dcompiler_47, api-ms-win-* sets newer than Win7, ...).
  2. A curated list of functions introduced in Windows 8 or later.
  3. Header-guard probe: each imported function name is referenced from a
     tiny C file compiled with mingw-w64 at _WIN32_WINNT=0x0601 and at
     0x0A00. A name declared only at 0x0A00 is gated on a newer Windows in
     the SDK headers, i.e. probably absent on Win7.
Exit code 1 if anything is flagged. A static import of a missing function
stops the process from starting at all ("The procedure entry point ... could
not be located"), so every hit must become a GetProcAddress with a fallback.
"""
import re
import subprocess
import sys
import tempfile
import os

# DLLs absent from a stock Windows 7 SP1 install.
MISSING_DLLS = {
    "shcore.dll", "combase.dll", "ucrtbase.dll", "vcruntime140.dll", "msvcp140.dll",
    "d3dcompiler_47.dll", "d3d12.dll", "dcomp.dll", "dxcore.dll", "xinput1_4.dll",
    "windows.storage.dll", "twinapi.dll",
}

# api-ms-win-* contract sets Win7 SP1 ships (as forwarders). Anything else is newer.
WIN7_APISETS_PREFIXES = (
    "api-ms-win-core-console-l1-1-0", "api-ms-win-core-datetime-l1-1-0",
    "api-ms-win-core-debug-l1-1-0", "api-ms-win-core-delayload-l1-1-0",
    "api-ms-win-core-errorhandling-l1-1-0", "api-ms-win-core-fibers-l1-1-0",
    "api-ms-win-core-file-l1-1-0", "api-ms-win-core-handle-l1-1-0",
    "api-ms-win-core-heap-l1-1-0", "api-ms-win-core-interlocked-l1-1-0",
    "api-ms-win-core-io-l1-1-0", "api-ms-win-core-libraryloader-l1-1-0",
    "api-ms-win-core-localization-l1-1-0", "api-ms-win-core-localregistry-l1-1-0",
    "api-ms-win-core-memory-l1-1-0", "api-ms-win-core-misc-l1-1-0",
    "api-ms-win-core-namedpipe-l1-1-0", "api-ms-win-core-processenvironment-l1-1-0",
    "api-ms-win-core-processthreads-l1-1-0", "api-ms-win-core-profile-l1-1-0",
    "api-ms-win-core-rtlsupport-l1-1-0", "api-ms-win-core-string-l1-1-0",
    "api-ms-win-core-synch-l1-1-0", "api-ms-win-core-sysinfo-l1-1-0",
    "api-ms-win-core-threadpool-l1-1-0", "api-ms-win-core-util-l1-1-0",
    "api-ms-win-core-xstate-l1-1-0", "api-ms-win-security-base-l1-1-0",
)

# Functions introduced in Windows 8 or later that commonly leak into imports.
WIN8_PLUS = set("""
GetSystemTimePreciseAsFileTime SetThreadDescription GetThreadDescription CreateFile2
CopyFile2 WaitOnAddress WakeByAddressSingle WakeByAddressAll GetOverlappedResultEx
InitializeSynchronizationBarrier EnterSynchronizationBarrier DeleteSynchronizationBarrier
PrefetchVirtualMemory DiscardVirtualMemory OfferVirtualMemory ReclaimVirtualMemory
GetCurrentThreadStackLimits SetThreadInformation GetThreadInformation
GetProcessMitigationPolicy SetProcessMitigationPolicy GetFirmwareType IsWow64Process2
GetSystemCpuSetInformation SetThreadSelectedCpuSets VirtualAlloc2 MapViewOfFile3
SetDefaultDllDirectories AddDllDirectory RemoveDllDirectory LoadPackagedLibrary
GetCurrentPackageFullName GetCurrentPackageId GetPackageFamilyName CreateFileMappingFromApp
SetThreadpoolTimerEx SetThreadpoolWaitEx RtlAddGrowableFunctionTable CompareObjectHandles
QueryInterruptTime QueryInterruptTimePrecise QueryUnbiasedInterruptTimePrecise
SetProcessDpiAwarenessContext SetThreadDpiAwarenessContext GetThreadDpiAwarenessContext
GetWindowDpiAwarenessContext GetAwarenessFromDpiAwarenessContext AreDpiAwarenessContextsEqual
IsValidDpiAwarenessContext GetDpiForWindow GetDpiForSystem GetSystemDpiForProcess
AdjustWindowRectExForDpi GetSystemMetricsForDpi EnableNonClientDpiScaling
SystemParametersInfoForDpi LogicalToPhysicalPointForPerMonitorDPI
PhysicalToLogicalPointForPerMonitorDPI GetDpiForMonitor SetProcessDpiAwareness
GetProcessDpiAwareness GetScaleFactorForMonitor GetPointerInfo GetPointerType
GetPointerPenInfo GetPointerTouchInfo GetPointerFrameInfo GetPointerDevices
GetPointerDeviceRects EnableMouseInPointer IsMouseInPointerEnabled SkipPointerFrameMessages
RegisterPointerInputTarget InjectTouchInput InitializeTouchInjection
GetCurrentInputMessageSource GetCIMSSM SetCoalescableTimer EvaluateProximityToRect
GetDisplayAutoRotationPreferences SetDisplayAutoRotationPreferences IsImmersiveProcess
PathCchCombine PathCchCombineEx PathCchAppend PathCchAppendEx PathCchCanonicalize
PathCchCanonicalizeEx PathCchRemoveFileSpec PathCchAddBackslash PathCchFindExtension
PathAllocCombine PathAllocCanonicalize
RoInitialize RoUninitialize RoGetActivationFactory RoActivateInstance WindowsCreateString
WindowsDeleteString WindowsGetStringRawBuffer CoIncrementMTAUsage CoDecrementMTAUsage
WinHttpWebSocketCompleteUpgrade WinHttpWebSocketSend WinHttpWebSocketReceive
WinHttpWebSocketClose WinHttpWebSocketShutdown WinHttpWebSocketQueryCloseStatus
GetAddrInfoExCancel GetAddrInfoExOverlappedResult CreateDXGIFactory2 DXGIGetDebugInterface1
BCryptHash CM_Register_Notification CM_Unregister_Notification EventSetInformation
GetSystemTimeAdjustmentPrecise RtlGetDeviceFamilyInfoEnum GetOsManufacturingMode
""".split())

HEADERS = """
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <commdlg.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <iphlpapi.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <mmsystem.h>
#include <dwmapi.h>
#include <setupapi.h>
#include <imm.h>
#include <winver.h>
#include <xinput.h>
#include <hidsdi.h>
#include <cfgmgr32.h>
#include <uxtheme.h>
#include <objbase.h>
#include <oleauto.h>
#include <powrprof.h>
#include <winhttp.h>
"""


def imports(path):
    out = subprocess.run(["llvm-objdump", "-p", path], capture_output=True, text=True).stdout
    dll = None
    res = {}
    for line in out.splitlines():
        m = re.match(r"\s*DLL Name:\s*(\S+)", line)
        if m:
            dll = m.group(1).lower()
            res.setdefault(dll, set())
            continue
        if dll is None:
            continue
        m = re.match(r"\s+(\d+)\s+(\S+)\s*$", line)
        if m and not line.strip().startswith("Hint"):
            res[dll].add(m.group(2))
        elif line.strip() == "" and res.get(dll):
            pass
    return res


def header_probe(names):
    """Names that compile at 0x0A00 but not at 0x0601 (one error per name)."""
    gated = set()
    with tempfile.TemporaryDirectory() as td:
        def build(winnt):
            src = os.path.join(td, "probe_%s.c" % winnt)
            with open(src, "w") as f:
                f.write("#define _WIN32_WINNT %s\n#define WINVER %s\n" % (winnt, winnt))
                f.write("#define NTDDI_VERSION %s\n" % ("0x06010000" if winnt == "0x0601" else "0x0A000000"))
                f.write(HEADERS)
                for i, n in enumerate(sorted(names)):
                    f.write("void* probe_%d(void) { return (void*)&%s; }\n" % (i, n))
            r = subprocess.run(["i686-w64-mingw32-gcc", "-fsyntax-only", "-w",
                                "-Wno-error=implicit-function-declaration",
                                "-fmax-errors=0", src], capture_output=True, text=True)
            bad = set()
            for m in re.finditer(r"[\'\u2018]([A-Za-z_][A-Za-z0-9_]*)[\'\u2019] undeclared", r.stderr):
                bad.add(m.group(1))
            return bad
        at7, at10 = build("0x0601"), build("0x0A00")
        gated = at7 - at10
    return gated


def main():
    flagged = 0
    for path in sys.argv[1:]:
        imp = imports(path)
        print("== %s (%d DLLs, %d functions)" % (os.path.basename(path), len(imp),
                                                  sum(len(v) for v in imp.values())))
        for dll, funcs in sorted(imp.items()):
            if dll in MISSING_DLLS:
                print("  MISSING DLL on Win7: %s (%s)" % (dll, ", ".join(sorted(funcs))))
                flagged += 1
            if dll.startswith("api-ms-") and not dll.startswith(WIN7_APISETS_PREFIXES):
                print("  NEWER API SET: %s (%s)" % (dll, ", ".join(sorted(funcs))))
                flagged += 1
            for f in sorted(funcs):
                if f in WIN8_PLUS:
                    print("  WIN8+ FUNCTION: %s!%s" % (dll, f))
                    flagged += 1
        all_names = {f for v in imp.values() for f in v if re.match(r"^[A-Za-z_]\w*$", f)}
        for name in sorted(header_probe(all_names)):
            dll = next(d for d, v in imp.items() if name in v)
            print("  HEADER-GATED (>0x0601): %s!%s" % (dll, name))
            if name not in WIN8_PLUS:
                flagged += 1
        print("  dlls: " + ", ".join(sorted(imp)))
    print("FLAGGED: %d" % flagged)
    return 1 if flagged else 0


if __name__ == "__main__":
    sys.exit(main())
