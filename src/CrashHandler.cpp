/* Copyright 2006-2011 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include "CrashHandler.h"

#include "Version.h"

#include "BaseUtil.h"
#include "StrUtil.h"
#include "Vec.h"
#include "WinUtil.h"
#include "FileUtil.h"

#include "translations.h"

typedef BOOL WINAPI MiniDumpWriteProc(
    HANDLE hProcess,
    DWORD ProcessId,
    HANDLE hFile,
    LONG DumpType,
    PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam,
    PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam,
    PMINIDUMP_CALLBACK_INFORMATION CallbackParam);

typedef BOOL _stdcall SymInitializeProc(
    HANDLE hProcess,
    PCSTR UserSearchPath,
    BOOL fInvadeProcess);

typedef DWORD _stdcall SymGetOptionsProc();
typedef DWORD _stdcall SymSetOptionsProc(DWORD SymOptions);

typedef BOOL _stdcall StackWalk64Proc(
    DWORD MachineType,
    HANDLE hProcess,
    HANDLE hThread,
    LPSTACKFRAME64 StackFrame,
    PVOID ContextRecord,
    PREAD_PROCESS_MEMORY_ROUTINE64 ReadMemoryRoutine,
    PFUNCTION_TABLE_ACCESS_ROUTINE64 FunctionTableAccessRoutine,
    PGET_MODULE_BASE_ROUTINE64 GetModuleBaseRoutine,
    PTRANSLATE_ADDRESS_ROUTINE64 TranslateAddress);

typedef BOOL _stdcall SymFromAddrProc(
    HANDLE hProcess,
    DWORD64 Address,
    PDWORD64 Displacement,
    PSYMBOL_INFO Symbol
);

typedef PVOID _stdcall SymFunctionTableAccess64Proc(
    HANDLE hProcess,
    DWORD64 AddrBase);

typedef DWORD64 _stdcall SymGetModuleBase64Proc(
    HANDLE hProcess,
    DWORD64 qwAddr);

static MiniDumpWriteProc *              _MiniDumpWriteDump;
static SymInitializeProc *              _SymInitialize;
static SymGetOptionsProc *              _SymGetOptions;
static SymSetOptionsProc *              _SymSetOptions;
static StackWalk64Proc   *              _StackWalk64;
static SymFunctionTableAccess64Proc *   _SymFunctionTableAccess64;
static SymGetModuleBase64Proc *         _SymGetModuleBase64;
static SymFromAddrProc *                _SymFromAddr;

static ScopedMem<TCHAR> g_crashDumpPath(NULL);
static ScopedMem<TCHAR> g_crashTxtPath(NULL);
static HANDLE g_dumpEvent = NULL;
static HANDLE g_dumpThread = NULL;
static MINIDUMP_EXCEPTION_INFORMATION mei = { 0 };
static BOOL gSymInitializeOk = FALSE;

#define F(X) \
    { #X, (void**)&_ ## X },

FuncNameAddr g_dbgHelpFuncs[] = {
    F(MiniDumpWriteDump)
    F(SymInitialize)
    F(SymGetOptions)
    F(SymSetOptions)
    F(StackWalk64)
    F(SymFunctionTableAccess64)
    F(SymGetModuleBase64)
    F(SymFromAddr)
    { NULL, NULL }
};
#undef F

static void LoadDbgHelpFuncs()
{
    if (_MiniDumpWriteDump)
        return;
    LoadDllFuncs(_T("DBGHELP.DLL"), g_dbgHelpFuncs);
}

static bool CanStackWalk()
{
    return gSymInitializeOk && _SymInitialize && _SymGetOptions 
        && _SymSetOptions&& _StackWalk64 && _SymFunctionTableAccess64 
        && _SymGetModuleBase64 && _SymFromAddr;
}

static BOOL CALLBACK OpenMiniDumpCallback(void* /*param*/, PMINIDUMP_CALLBACK_INPUT input, PMINIDUMP_CALLBACK_OUTPUT output)
{
    if (!input || !output) 
        return FALSE; 

    switch (input->CallbackType) {
    case ModuleCallback:
        if (!(output->ModuleWriteFlags & ModuleReferencedByMemory))
            output->ModuleWriteFlags &= ~ModuleWriteModule; 
        return TRUE;
    case IncludeModuleCallback:
    case IncludeThreadCallback:
    case ThreadCallback:
    case ThreadExCallback:
        return TRUE;
    default:
        return FALSE;
    }
}

// returns false for 32bit processes running on 64bit os
static bool Is64Bit()
{
    typedef void (WINAPI * GetSystemInfoProc)(LPSYSTEM_INFO);
    WinLibrary lib(_T("kernel32.dll"));
    GetSystemInfoProc _GetNativeSystemInfo = (GetSystemInfoProc)lib.GetProcAddr("GetNativeSystemInfo");

    if (!_GetNativeSystemInfo)
        return false;
    SYSTEM_INFO sysInfo = {0};
    _GetNativeSystemInfo(&sysInfo);
    return sysInfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64;
}

static char *OsNameFromVer(OSVERSIONINFOEX ver)
{
    if (VER_PLATFORM_WIN32_NT != ver.dwPlatformId)
        return "Unknown";
    // Windows 7 beta 1 reports the same major version as Vista does.
    if (ver.dwMajorVersion == 6 && ver.dwMinorVersion == 1) {
        return "7";
    } else if (ver.dwMajorVersion == 6 && ver.dwMinorVersion == 0) {
        return "Vista";
    } else if (ver.dwMajorVersion == 5 && ver.dwMinorVersion == 2) {
        return "Sever 2003";
    } else if (ver.dwMajorVersion == 5 && ver.dwMinorVersion == 1) {
        return "XP";
    } else if (ver.dwMajorVersion == 5 && ver.dwMinorVersion == 0) {
        return "2000";
    } else if (ver.dwMajorVersion <= 4) {
        return "9xOrNT";
    } else {
        return "Unknown";
    }
}


static bool IsWow64()
{
    typedef BOOL (WINAPI *IsWow64ProcessProc)(HANDLE, PBOOL);
    IsWow64ProcessProc _IsWow64Process;

    WinLibrary lib(_T("kernel32.dll"));
    _IsWow64Process = (IsWow64ProcessProc)lib.GetProcAddr("IsWow64Process");
    if (!_IsWow64Process)
        return false;
    BOOL isWow = FALSE;
    _IsWow64Process(GetCurrentProcess(), &isWow);
    return isWow;
}

static void GetOsVersion(Str::Str<char>& s)
{
    OSVERSIONINFOEX ver;
    ZeroMemory(&ver, sizeof(ver));
    ver.dwOSVersionInfoSize = sizeof(ver);
    BOOL ok = GetVersionEx((OSVERSIONINFO*)&ver);
    if (!ok)
        return;
    char *os = OsNameFromVer(ver);
    int servicePackMajor = ver.wServicePackMajor;
    int servicePackMinor = ver.wServicePackMinor;
    int buildNumber = ver.dwBuildNumber & 0xFFFF;
    char *is64bit = Is64Bit() ? "32bit" : "64bit";
    char *isWow = IsWow64() ?  "Wow64" : "";
    s.AppendFmt("OS: %s %d.%d build %d %s %s\r\n", os, servicePackMajor, servicePackMinor, buildNumber, is64bit, isWow);
}

static void GetModules(Str::Str<char>& s)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
      if (snap == INVALID_HANDLE_VALUE) return;

    MODULEENTRY32 mod;
    mod.dwSize = sizeof(mod);
    BOOL cont = Module32First(snap, &mod);
    while (cont) {
        TCHAR *name = mod.szModule;
        TCHAR *path = mod.szExePath;
        char *nameA = Str::Conv::ToUtf8(name);
        char *pathA = Str::Conv::ToUtf8(path);
        s.AppendFmt("Module: %08X %06X %-16s %s\r\n", (DWORD)mod.modBaseAddr, (DWORD)mod.modBaseSize, nameA, pathA);
        free(nameA);
        free(pathA);
        cont = Module32Next(snap, &mod);
    }
    CloseHandle(snap);
}

// TODO: should offsetOut be DWORD_PTR for 64-bit compat?
static bool GetAddrInfo(void *addr, char *module, DWORD moduleLen, DWORD& sectionOut, DWORD& offsetOut)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(addr, &mbi, sizeof(mbi)))
            return false;

    DWORD hMod = (DWORD)mbi.AllocationBase;
    if (!GetModuleFileNameA((HMODULE)hMod, module, moduleLen))
            return false;

    PIMAGE_DOS_HEADER dosHeader = (PIMAGE_DOS_HEADER)hMod;
    PIMAGE_NT_HEADERS pNtHeader = (PIMAGE_NT_HEADERS)(hMod + dosHeader->e_lfanew);
    PIMAGE_SECTION_HEADER section = IMAGE_FIRST_SECTION(pNtHeader);
    
    DWORD lAddr = (DWORD)addr - hMod;
    for (unsigned int i = 0; i < pNtHeader->FileHeader.NumberOfSections; i++)
    {
            DWORD startAddr = section->VirtualAddress;
            DWORD endAddr = startAddr;
            if (section->SizeOfRawData > section->Misc.VirtualSize)
                    endAddr += section->SizeOfRawData;
            else
                    section->Misc.VirtualSize;
    
            if (lAddr >= startAddr && lAddr <= endAddr)
            {
                    sectionOut = i+1;
                    offsetOut = lAddr - startAddr;
                    return true;
            }
            section++;
    }
    return false;
}

#define MAX_NAME_LEN 512

static void GetCallstack(Str::Str<char>& s, CONTEXT& ctx, HANDLE hThread)
{
    if (!CanStackWalk())
        return;

    HANDLE hProc = GetCurrentProcess();

    STACKFRAME64 stackFrame;
    memset(&stackFrame, 0, sizeof(stackFrame));
#ifdef  _WIN64
    stackFrame.AddrPC.Offset = ctx.Rip;
    stackFrame.AddrFrame.Offset = ctx.Rbp;
    stackFrame.AddrStack.Offset = ctx.Rsp;
#else
    stackFrame.AddrPC.Offset = ctx.Eip;
    stackFrame.AddrFrame.Offset = ctx.Ebp;
    stackFrame.AddrStack.Offset = ctx.Esp;
#endif
    stackFrame.AddrPC.Mode = AddrModeFlat;
    stackFrame.AddrFrame.Mode = AddrModeFlat;
    stackFrame.AddrStack.Mode = AddrModeFlat;

    char buf[sizeof(SYMBOL_INFO) + MAX_NAME_LEN * sizeof(char)];
    SYMBOL_INFO *symInfo = (SYMBOL_INFO*)buf;

    int framesCount = 0;
    int maxFrames = 32;
    while (framesCount < maxFrames)
    {
        BOOL ok = _StackWalk64(IMAGE_FILE_MACHINE_I386, hProc, hThread,
            &stackFrame, &ctx, NULL, _SymFunctionTableAccess64,
            _SymGetModuleBase64, NULL);
        if (!ok)
            break;

        memset(buf, 0, sizeof(buf));
        symInfo->SizeOfStruct = sizeof(SYMBOL_INFO);
        symInfo->MaxNameLen = MAX_NAME_LEN - 1;
        DWORD64 addr = stackFrame.AddrPC.Offset;

        DWORD64 symDisp = 0;
        ok = _SymFromAddr(hProc, addr, &symDisp, symInfo);
        if (ok) {
            char *name = &(symInfo->Name[0]);
            s.AppendFmt("0x%p %s+0x%x\r\n", (void*)addr, name, (int)symDisp);
        } else {
            char module[MAX_PATH] = { 0 };
            DWORD section, offset;
            // TODO: shouldn't we be able to use export table of the DLL to
            // at least know which function in the DLL it is even when we
            // don't have symbols?
            if (GetAddrInfo((void*)addr, module, sizeof(module), section, offset)) {
                const char *moduleShort = Path::GetBaseName(module);
                s.AppendFmt("0x%p %02X:%08X %s\r\n", (void*)addr, section, offset, moduleShort);
            } else {
                s.AppendFmt("%%08X\r\n", (DWORD)addr);
            }
        }
        framesCount++;
    }
}

static void GetCurrentThreadCallstack(Str::Str<char>&s)
{
    CONTEXT ctx;
    RtlCaptureContext(&ctx);
    s.AppendFmt("\r\nThread: %x\r\n", GetCurrentThreadId());
    GetCallstack(s, ctx, GetCurrentThread());
}

static void GetThreadCallstack(Str::Str<char>& s, DWORD threadId)
{
    if (threadId == GetCurrentThreadId())
        return;

    s.AppendFmt("\r\nThread: %x\r\n", threadId);

    DWORD access = THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION | THREAD_SUSPEND_RESUME;
    HANDLE hThread = OpenThread(access, false, threadId);
    if (!hThread) {
        s.Append("Failed to OpenThread()\r\n");
        return;
    }

    DWORD res = SuspendThread(hThread);
    if (-1 == res) {
        s.Append("Failed to SuspendThread()\r\n");
    } else {
        CONTEXT ctx = { 0 };
        ctx.ContextFlags = CONTEXT_FULL;
        BOOL ok = GetThreadContext(hThread, &ctx);
        if (ok)
            GetCallstack(s, ctx, hThread);
        else
            s.Append("Failed to GetThreadContext()\r\n");
            
        ResumeThread(hThread);
    }
    CloseHandle(hThread);
}
    
static void GetAllThreadsCallstacks(Str::Str<char>& s)
{
    HANDLE threadSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (threadSnap == INVALID_HANDLE_VALUE)
        return;

    THREADENTRY32 te32;
    te32.dwSize = sizeof(THREADENTRY32);

    DWORD pid = GetCurrentProcessId();
    BOOL ok = Thread32First(threadSnap, &te32);
    while (ok) {
        if (te32.th32OwnerProcessID == pid)
            GetThreadCallstack(s, te32.th32ThreadID);
        ok = Thread32Next(threadSnap, &te32);
    }

    CloseHandle(threadSnap);
}

static char *ExceptionNameFromCode(DWORD excCode)
{
#define EXC(x) case EXCEPTION_##x: return #x;

    switch (excCode)
    {
        EXC(ACCESS_VIOLATION)
        EXC(DATATYPE_MISALIGNMENT)
        EXC(BREAKPOINT)
        EXC(SINGLE_STEP)
        EXC(ARRAY_BOUNDS_EXCEEDED)
        EXC(FLT_DENORMAL_OPERAND)
        EXC(FLT_DIVIDE_BY_ZERO)
        EXC(FLT_INEXACT_RESULT)
        EXC(FLT_INVALID_OPERATION)
        EXC(FLT_OVERFLOW)
        EXC(FLT_STACK_CHECK)
        EXC(FLT_UNDERFLOW)
        EXC(INT_DIVIDE_BY_ZERO)
        EXC(INT_OVERFLOW)
        EXC(PRIV_INSTRUCTION)
        EXC(IN_PAGE_ERROR)
        EXC(ILLEGAL_INSTRUCTION)
        EXC(NONCONTINUABLE_EXCEPTION)
        EXC(STACK_OVERFLOW)
        EXC(INVALID_DISPOSITION)
        EXC(GUARD_PAGE)
        EXC(INVALID_HANDLE)
    }
#undef EXC

    static char buf[512] = { 0 };

    FormatMessageA(FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_FROM_HMODULE,
        GetModuleHandleA("NTDLL.DLL"),
        excCode, 0, buf, sizeof(buf), 0);

    return buf;

}

static void GetExceptionInfo(Str::Str<char>& s, EXCEPTION_POINTERS *excPointers)
{
    if (!excPointers)
        return;
    EXCEPTION_RECORD *excRecord = excPointers->ExceptionRecord;
    DWORD excCode = excRecord->ExceptionCode;
    s.AppendFmt("Exception: %08X %s\r\n", (int)excCode, ExceptionNameFromCode(excCode));

    char module[MAX_PATH];
    DWORD section;
    DWORD offset;
    GetAddrInfo(excRecord->ExceptionAddress, module, sizeof(module), section, offset);
    const char *moduleShort = Path::GetBaseName(module);
#ifdef _M_IX86
    s.AppendFmt("Fault address:  %08X %02X:%08X %s\r\n", excRecord->ExceptionAddress, section, offset, moduleShort);
#endif
#ifdef _M_X64
    s.AppendFmt("Fault address:  %016I64X %02X:%016I64X %s\r\n", excRecord->ExceptionAddress, section, offset, moduleShort);
#endif

    PCONTEXT ctx = excPointers->ContextRecord;
    s.AppendFmt("\r\nRegisters:\r\n");

#ifdef _M_IX86
    s.AppendFmt("EAX:%08X\r\nEBX:%08X\r\nECX:%08X\r\nEDX:%08X\r\nESI:%08X\r\nEDI:%08X\r\n",
        ctx->Eax, ctx->Ebx, ctx->Ecx, ctx->Edx, ctx->Esi, ctx->Edi);
    s.AppendFmt("CS:EIP:%04X:%08X\r\n", ctx->SegCs, ctx->Eip);
    s.AppendFmt("SS:ESP:%04X:%08X  EBP:%08X\r\n", ctx->SegSs, ctx->Esp, ctx->Ebp);
    s.AppendFmt("DS:%04X  ES:%04X  FS:%04X  GS:%04X\r\n", ctx->SegDs, ctx->SegEs, ctx->SegFs, ctx->SegGs);
    s.AppendFmt("Flags:%08X\r\n", ctx->EFlags);
#endif

#ifdef _M_X64
    s.AppendFmt("RAX:%016I64X\r\nRBX:%016I64X\r\nRCX:%016I64X\r\nRDX:%016I64X\r\nRSI:%016I64X\r\nRDI:%016I64X\r\n"
        "R8: %016I64X\r\nR9: %016I64X\r\nR10:%016I64X\r\nR11:%016I64X\r\nR12:%016I64X\r\nR13:%016I64X\r\nR14:%016I64X\r\nR15:%016I64X\r\n",
        ctx->Rax, ctx->Rbx, ctx->Rcx, ctx->Rdx, ctx->Rsi, ctx->Rdi,
        ctx->R9,ctx->R10,ctx->R11,ctx->R12,ctx->R13,ctx->R14,ctx->R15);
    s.AppendFmt("CS:RIP:%04X:%016I64X\r\n", ctx->SegCs, ctx->Rip);
    s.AppendFmt("SS:RSP:%04X:%016X  RBP:%08X\r\n", ctx->SegSs, ctx->Rsp, ctx->Rbp);
    s.AppendFmt("DS:%04X  ES:%04X  FS:%04X  GS:%04X\r\n", ctx->SegDs, ctx->SegEs, ctx->SegFs, ctx->SegGs);
    s.AppendFmt("Flags:%08X\r\n", ctx->EFlags);
#endif
}

static char *BuildCrashInfoText()
{
    Str::Str<char> s(16 * 1024);
    s.AppendFmt("Ver: %s\r\n", QM(CURR_VERSION));
    GetOsVersion(s);
    s.Append("\r\n");
    GetModules(s);
    s.Append("\r\n");
    GetExceptionInfo(s, mei.ExceptionPointers);
    GetCurrentThreadCallstack(s);
    GetAllThreadsCallstacks(s);
    return s.StealData();
}

void SaveCrashInfoText()
{
    char *s = BuildCrashInfoText();
    if (s && g_crashTxtPath) {
        File::WriteAll(g_crashTxtPath, s, Str::Len(s));
    }
    free(s);
    // TODO: submit to a website
}

static DWORD WINAPI CrashDumpThread(LPVOID data)
{
    WaitForSingleObject(g_dumpEvent, INFINITE);
    LoadDbgHelpFuncs();

    if (!_MiniDumpWriteDump)
    {
#ifdef SVN_PRE_RELEASE_VER
        MessageBox(NULL, _T("Couldn't create a crashdump file: dbghelp.dll is unexpectedly missing."), _TR("SumatraPDF crashed"), MB_ICONEXCLAMATION | MB_OK);
#endif
        return 0;
    }

    HANDLE dumpFile = CreateFile(g_crashDumpPath.Get(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
    if (INVALID_HANDLE_VALUE == dumpFile)
    {
#ifdef SVN_PRE_RELEASE_VER
        MessageBox(NULL, _T("Couldn't create a crashdump file."), _TR("SumatraPDF crashed"), MB_ICONEXCLAMATION | MB_OK);
#endif
        return 0;
    }

    MINIDUMP_TYPE type = (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory);
    // set the SUMATRAPDF_FULLDUMP environment variable for far more complete minidumps
    if (GetEnvironmentVariable(_T("SUMATRAPDF_FULLDUMP"), NULL, 0))
        type = (MINIDUMP_TYPE)(type | MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithPrivateReadWriteMemory);
    MINIDUMP_CALLBACK_INFORMATION mci = { OpenMiniDumpCallback, NULL }; 

    _MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dumpFile, type, &mei, NULL, &mci);

    CloseHandle(dumpFile);

    SaveCrashInfoText();

    // exec_with_params(g_exePath, CMD_ARG_SEND_CRASHDUMP, TRUE /* hidden */);
    return 0;
}

static LONG WINAPI DumpExceptionHandler(EXCEPTION_POINTERS *exceptionInfo)
{
    if (!exceptionInfo || (EXCEPTION_BREAKPOINT == exceptionInfo->ExceptionRecord->ExceptionCode))
        return EXCEPTION_CONTINUE_SEARCH;

    static bool wasHere = false;
    if (wasHere)
        return EXCEPTION_CONTINUE_SEARCH;
    wasHere = true;

    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = exceptionInfo;
    // per msdn (which is backed by my experience), MiniDumpWriteDump() doesn't
    // write callstack for the calling thread correctly. We use msdn-recommended
    // work-around of spinning a thread to do the writing
    SetEvent(g_dumpEvent);
    WaitForSingleObject(g_dumpThread, INFINITE);

    ScopedMem<TCHAR> msg(Str::Format(_T("%s\n\n%s"), _TR("Please include the following file in your crash report:"), g_crashDumpPath.Get()));
    MessageBox(NULL, msg.Get(), _TR("SumatraPDF crashed"), MB_ICONERROR | MB_OK);

    return EXCEPTION_CONTINUE_SEARCH;
}

void InstallCrashHandler(const TCHAR *crashDumpPath, const TCHAR *crashTxtPath)
{
    LoadDbgHelpFuncs();

    if (_SymInitialize) {
       gSymInitializeOk = _SymInitialize(GetCurrentProcess(), NULL, FALSE);
       if (gSymInitializeOk) {
           DWORD symOptions =_SymGetOptions();
           symOptions |= SYMOPT_LOAD_LINES;
           symOptions |= SYMOPT_FAIL_CRITICAL_ERRORS; // don't show system msg box on errors
           symOptions |= SYMOPT_DEFERRED_LOADS;
           _SymSetOptions(symOptions);
           // TODO: should I setup sympath to Microsoft's symbol server via
           // SymSetSearchPath() so that at least I get names for system dlls?
       }
    }

    if (NULL == crashDumpPath)
        return;
    g_crashDumpPath.Set(Str::Dup(crashDumpPath));
    g_crashTxtPath.Set(Str::Dup(crashTxtPath));
    if (!g_dumpEvent && !g_dumpThread) {
        g_dumpEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
        g_dumpThread = CreateThread(NULL, 0, CrashDumpThread, NULL, 0, 0);

        SetUnhandledExceptionFilter(DumpExceptionHandler);
    }
}
