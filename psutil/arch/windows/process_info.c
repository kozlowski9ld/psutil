/*
 * Copyright (c) 2009, Jay Loden, Giampaolo Rodola'. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Helper functions related to fetching process information. Used by
 * _psutil_windows module methods.
 */

#include <Python.h>
#include <windows.h>
#include <Psapi.h>
#include <tlhelp32.h>

#include "security.h"
#include "process_info.h"
#include "ntextapi.h"
#include "../../_psutil_common.h"


// ====================================================================
// Helper structures to access the memory correctly.
// Some of these might also be defined in the winternl.h header file
// but unfortunately not in a usable way.
// ====================================================================

// see http://msdn2.microsoft.com/en-us/library/aa489609.aspx
#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) ((NTSTATUS)(Status) >= 0)
#endif

// http://msdn.microsoft.com/en-us/library/aa813741(VS.85).aspx
typedef struct {
    BYTE Reserved1[16];
    PVOID Reserved2[5];
    UNICODE_STRING CurrentDirectoryPath;
    PVOID CurrentDirectoryHandle;
    UNICODE_STRING DllPath;
    UNICODE_STRING ImagePathName;
    UNICODE_STRING CommandLine;
    LPCWSTR env;
} RTL_USER_PROCESS_PARAMETERS_, *PRTL_USER_PROCESS_PARAMETERS_;

// https://msdn.microsoft.com/en-us/library/aa813706(v=vs.85).aspx
#ifdef _WIN64
typedef struct {
    BYTE Reserved1[2];
    BYTE BeingDebugged;
    BYTE Reserved2[21];
    PVOID LoaderData;
    PRTL_USER_PROCESS_PARAMETERS_ ProcessParameters;
    /* More fields ...  */
} PEB_;
#else
typedef struct {
    BYTE Reserved1[2];
    BYTE BeingDebugged;
    BYTE Reserved2[1];
    PVOID Reserved3[2];
    PVOID Ldr;
    PRTL_USER_PROCESS_PARAMETERS_ ProcessParameters;
    /* More fields ...  */
} PEB_;
#endif

#ifdef _WIN64
/* When we are a 64 bit process accessing a 32 bit (WoW64) process we need to
   use the 32 bit structure layout. */
typedef struct {
    USHORT Length;
    USHORT MaxLength;
    DWORD Buffer;
} UNICODE_STRING32;

typedef struct {
    BYTE Reserved1[16];
    DWORD Reserved2[5];
    UNICODE_STRING32 CurrentDirectoryPath;
    DWORD CurrentDirectoryHandle;
    UNICODE_STRING32 DllPath;
    UNICODE_STRING32 ImagePathName;
    UNICODE_STRING32 CommandLine;
    DWORD env;
} RTL_USER_PROCESS_PARAMETERS32;

typedef struct {
    BYTE Reserved1[2];
    BYTE BeingDebugged;
    BYTE Reserved2[1];
    DWORD Reserved3[2];
    DWORD Ldr;
    DWORD ProcessParameters;
    /* More fields ...  */
} PEB32;
#else
/* When we are a 32 bit (WoW64) process accessing a 64 bit process we need to
   use the 64 bit structure layout and a special function to read its memory.
   */
typedef NTSTATUS (NTAPI *_NtWow64ReadVirtualMemory64)(
        IN HANDLE ProcessHandle,
        IN PVOID64 BaseAddress,
        OUT PVOID Buffer,
        IN ULONG64 Size,
        OUT PULONG64 NumberOfBytesRead);

typedef enum {
    MemoryInformationBasic
} MEMORY_INFORMATION_CLASS;

typedef struct {
    PVOID Reserved1[2];
    PVOID64 PebBaseAddress;
    PVOID Reserved2[4];
    PVOID UniqueProcessId[2];
    PVOID Reserved3[2];
} PROCESS_BASIC_INFORMATION64;

typedef struct {
    USHORT Length;
    USHORT MaxLength;
    PVOID64 Buffer;
} UNICODE_STRING64;

typedef struct {
    BYTE Reserved1[16];
    PVOID64 Reserved2[5];
    UNICODE_STRING64 CurrentDirectoryPath;
    PVOID64 CurrentDirectoryHandle;
    UNICODE_STRING64 DllPath;
    UNICODE_STRING64 ImagePathName;
    UNICODE_STRING64 CommandLine;
    PVOID64 env;
} RTL_USER_PROCESS_PARAMETERS64;

typedef struct {
    BYTE Reserved1[2];
    BYTE BeingDebugged;
    BYTE Reserved2[21];
    PVOID64 LoaderData;
    PVOID64 ProcessParameters;
    /* More fields ...  */
} PEB64;
#endif


#define PSUTIL_FIRST_PROCESS(Processes) ( \
    (PSYSTEM_PROCESS_INFORMATION)(Processes))
#define PSUTIL_NEXT_PROCESS(Process) ( \
   ((PSYSTEM_PROCESS_INFORMATION)(Process))->NextEntryOffset ? \
   (PSYSTEM_PROCESS_INFORMATION)((PCHAR)(Process) + \
        ((PSYSTEM_PROCESS_INFORMATION)(Process))->NextEntryOffset) : NULL)

const int STATUS_INFO_LENGTH_MISMATCH = 0xC0000004;
const int STATUS_BUFFER_TOO_SMALL = 0xC0000023L;


// A wrapper around GetModuleHandle and GetProcAddress.
PVOID
psutil_GetProcAddress(LPCSTR libname, LPCSTR procname) {
    HMODULE mod;
    FARPROC addr;

    if ((mod = GetModuleHandleA(libname)) == NULL) {
        PyErr_SetFromWindowsErrWithFilename(0, libname);
        return NULL;
    }
    if ((addr = GetProcAddress(mod, procname)) == NULL) {
        PyErr_SetFromWindowsErrWithFilename(0, procname);
        return NULL;
    }
    return addr;
}


// A wrapper around LoadLibrary and GetProcAddress.
PVOID
psutil_GetProcAddressFromLib(LPCSTR libname, LPCSTR procname) {
    HMODULE mod;
    FARPROC addr;

    if ((mod = LoadLibraryA(libname)) == NULL) {
        PyErr_SetFromWindowsErrWithFilename(0, libname);
        return NULL;
    }
    if ((addr = GetProcAddress(mod, procname)) == NULL) {
        PyErr_SetFromWindowsErrWithFilename(0, procname);
        FreeLibrary(mod);
        return NULL;
    }
    FreeLibrary(mod);
    return addr;
}


// ====================================================================
// Process and PIDs utiilties.
// ====================================================================


/*
 * Return 1 if PID exists, 0 if not, -1 on error.
 */
int
psutil_pid_in_pids(DWORD pid) {
    DWORD *proclist = NULL;
    DWORD numberOfReturnedPIDs;
    DWORD i;

    proclist = psutil_get_pids(&numberOfReturnedPIDs);
    if (proclist == NULL)
        return -1;
    for (i = 0; i < numberOfReturnedPIDs; i++) {
        if (proclist[i] == pid) {
            free(proclist);
            return 1;
        }
    }
    free(proclist);
    return 0;
}


/*
 * Given a process HANDLE checks whether it's actually running.
 * Returns:
 * - 1: running
 * - 0: not running
 * - -1: WindowsError
 * - -2: AssertionError
 */
int
psutil_is_phandle_running(HANDLE hProcess, DWORD pid) {
    DWORD processExitCode = 0;

    if (hProcess == NULL) {
        if (GetLastError() == ERROR_INVALID_PARAMETER) {
            // Yeah, this is the actual error code in case of
            // "no such process".
            if (! psutil_assert_pid_not_exists(
                    pid, "iphr: OpenProcess() -> ERROR_INVALID_PARAMETER")) {
                return -2;
            }
            return 0;
        }
        return -1;
    }

    if (GetExitCodeProcess(hProcess, &processExitCode)) {
        // XXX - maybe STILL_ACTIVE is not fully reliable as per:
        // http://stackoverflow.com/questions/1591342/#comment47830782_1591379
        if (processExitCode == STILL_ACTIVE) {
            if (! psutil_assert_pid_exists(
                    pid, "iphr: GetExitCodeProcess() -> STILL_ACTIVE")) {
                return -2;
            }
            return 1;
        }
        else {
            // We can't be sure so we look into pids.
            if (psutil_pid_in_pids(pid) == 1) {
                return 1;
            }
            else {
                CloseHandle(hProcess);
                return 0;
            }
        }
    }

    CloseHandle(hProcess);
    if (! psutil_assert_pid_not_exists( pid, "iphr: exit fun")) {
        return -2;
    }
    return -1;
}


/*
 * Given a process HANDLE checks whether it's actually running and if
 * it does return it, else return NULL with the proper Python exception
 * set.
 */
HANDLE
psutil_check_phandle(HANDLE hProcess, DWORD pid) {
    int ret = psutil_is_phandle_running(hProcess, pid);
    if (ret == 1)
        return hProcess;
    else if (ret == 0)
        return NoSuchProcess("");
    else if (ret == -1)
        return PyErr_SetFromWindowsErr(0);
    else  // -2
        return NULL;
}


/*
 * A wrapper around OpenProcess setting NSP exception if process
 * no longer exists.
 * "pid" is the process pid, "dwDesiredAccess" is the first argument
 * exptected by OpenProcess.
 * Return a process handle or NULL.
 */
HANDLE
psutil_handle_from_pid(DWORD pid, DWORD dwDesiredAccess) {
    HANDLE hProcess;

    if (pid == 0) {
        // otherwise we'd get NoSuchProcess
        return AccessDenied("");
    }

    hProcess = OpenProcess(dwDesiredAccess, FALSE, pid);
    return psutil_check_phandle(hProcess, pid);
}


DWORD *
psutil_get_pids(DWORD *numberOfReturnedPIDs) {
    // Win32 SDK says the only way to know if our process array
    // wasn't large enough is to check the returned size and make
    // sure that it doesn't match the size of the array.
    // If it does we allocate a larger array and try again

    // Stores the actual array
    DWORD *procArray = NULL;
    DWORD procArrayByteSz;
    int procArraySz = 0;

    // Stores the byte size of the returned array from enumprocesses
    DWORD enumReturnSz = 0;

    do {
        procArraySz += 1024;
        if (procArray != NULL)
            free(procArray);
        procArrayByteSz = procArraySz * sizeof(DWORD);
        procArray = malloc(procArrayByteSz);
        if (procArray == NULL) {
            PyErr_NoMemory();
            return NULL;
        }
        if (! EnumProcesses(procArray, procArrayByteSz, &enumReturnSz)) {
            free(procArray);
            PyErr_SetFromWindowsErr(0);
            return NULL;
        }
    } while (enumReturnSz == procArraySz * sizeof(DWORD));

    // The number of elements is the returned size / size of each element
    *numberOfReturnedPIDs = enumReturnSz / sizeof(DWORD);

    return procArray;
}


int
psutil_assert_pid_exists(DWORD pid, char *err) {
    if (PSUTIL_TESTING) {
        if (psutil_pid_in_pids(pid) == 0) {
            PyErr_SetString(PyExc_AssertionError, err);
            return 0;
        }
    }
    return 1;
}


int
psutil_assert_pid_not_exists(DWORD pid, char *err) {
    if (PSUTIL_TESTING) {
        if (psutil_pid_in_pids(pid) == 1) {
            PyErr_SetString(PyExc_AssertionError, err);
            return 0;
        }
    }
    return 1;
}


/*
/* Check for PID existance by using OpenProcess() + GetExitCodeProcess.
/* Returns:
 * 1: pid exists
 * 0: it doesn't
 * -1: error
 */
int
psutil_pid_is_running(DWORD pid) {
    HANDLE hProcess;
    DWORD exitCode;
    DWORD err;

    // Special case for PID 0 System Idle Process
    if (pid == 0)
        return 1;
    if (pid < 0)
        return 0;
    hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                           FALSE, pid);
    if (NULL == hProcess) {
        err = GetLastError();
        // Yeah, this is the actual error code in case of "no such process".
        if (err == ERROR_INVALID_PARAMETER) {
            if (! psutil_assert_pid_not_exists(
                    pid, "pir: OpenProcess() -> INVALID_PARAMETER")) {
                return -1;
            }
            return 0;
        }
        // Access denied obviously means there's a process to deny access to.
        else if (err == ERROR_ACCESS_DENIED) {
            if (! psutil_assert_pid_exists(
                    pid, "pir: OpenProcess() ACCESS_DENIED")) {
                return -1;
            }
            return 1;
        }
        // Be strict and raise an exception; the caller is supposed
        // to take -1 into account.
        else {
            PyErr_SetFromWindowsErr(err);
            return -1;
        }
    }

    if (GetExitCodeProcess(hProcess, &exitCode)) {
        CloseHandle(hProcess);
        // XXX - maybe STILL_ACTIVE is not fully reliable as per:
        // http://stackoverflow.com/questions/1591342/#comment47830782_1591379
        if (exitCode == STILL_ACTIVE) {
            if (! psutil_assert_pid_exists(
                    pid, "pir: GetExitCodeProcess() -> STILL_ACTIVE")) {
                return -1;
            }
            return 1;
        }
        // We can't be sure so we look into pids.
        else {
            return psutil_pid_in_pids(pid);
        }
    }
    else {
        err = GetLastError();
        CloseHandle(hProcess);
        // Same as for OpenProcess, assume access denied means there's
        // a process to deny access to.
        if (err == ERROR_ACCESS_DENIED) {
            if (! psutil_assert_pid_exists(
                    pid, "pir: GetExitCodeProcess() -> ERROR_ACCESS_DENIED")) {
                return -1;
            }
            return 1;
        }
        else {
            PyErr_SetFromWindowsErr(err);
            return -1;
        }
    }
}


/* Given a pointer into a process's memory, figure out how much data can be
 * read from it. */
static int
psutil_get_process_region_size(HANDLE hProcess, LPCVOID src, SIZE_T *psize) {
    MEMORY_BASIC_INFORMATION info;

    if (!VirtualQueryEx(hProcess, src, &info, sizeof(info))) {
        PyErr_SetFromWindowsErr(0);
        return -1;
    }

    *psize = info.RegionSize - ((char*)src - (char*)info.BaseAddress);
    return 0;
}


enum psutil_process_data_kind {
    KIND_CMDLINE,
    KIND_CWD,
    KIND_ENVIRON,
};

/* Get data from the process with the given pid.  The data is returned in the
   pdata output member as a nul terminated string which must be freed on
   success.

   On success 0 is returned.  On error the output parameter is not touched, -1
   is returned, and an appropriate Python exception is set. */
static int
psutil_get_process_data(long pid,
                        enum psutil_process_data_kind kind,
                        WCHAR **pdata,
                        SIZE_T *psize) {
    /* This function is quite complex because there are several cases to be
       considered:

       Two cases are really simple:  we (i.e. the python interpreter) and the
       target process are both 32 bit or both 64 bit.  In that case the memory
       layout of the structures matches up and all is well.

       When we are 64 bit and the target process is 32 bit we need to use
       custom 32 bit versions of the structures.

       When we are 32 bit and the target process is 64 bit we need to use
       custom 64 bit version of the structures.  Also we need to use separate
       Wow64 functions to get the information.

       A few helper structs are defined above so that the compiler can handle
       calculating the correct offsets.

       Additional help also came from the following sources:

         https://github.com/kohsuke/winp and
         http://wj32.org/wp/2009/01/24/howto-get-the-command-line-of-processes/
         http://stackoverflow.com/a/14012919
         http://www.drdobbs.com/embracing-64-bit-windows/184401966
     */
    _NtQueryInformationProcess NtQueryInformationProcess = NULL;
#ifndef _WIN64
    static _NtQueryInformationProcess NtWow64QueryInformationProcess64 = NULL;
    static _NtWow64ReadVirtualMemory64 NtWow64ReadVirtualMemory64 = NULL;
#endif
    HANDLE hProcess = NULL;
    LPCVOID src;
    SIZE_T size;
    WCHAR *buffer = NULL;
#ifdef _WIN64
    LPVOID ppeb32 = NULL;
#else
    PVOID64 src64;
    BOOL weAreWow64;
    BOOL theyAreWow64;
#endif
    DWORD access = PROCESS_QUERY_INFORMATION | PROCESS_VM_READ;

    NtQueryInformationProcess = \
        psutil_GetProcAddress("ntdll.dll", "NtQueryInformationProcess");
    if (NtQueryInformationProcess == NULL)
        return -1;

    hProcess = psutil_handle_from_pid(pid, access);
    if (hProcess == NULL)
        return -1;

#ifdef _WIN64
    /* 64 bit case.  Check if the target is a 32 bit process running in WoW64
     * mode. */
    if (!NT_SUCCESS(NtQueryInformationProcess(hProcess,
                                              ProcessWow64Information,
                                              &ppeb32,
                                              sizeof(LPVOID),
                                              NULL))) {
        PyErr_SetFromWindowsErr(0);
        goto error;
    }

    if (ppeb32 != NULL) {
        /* We are 64 bit.  Target process is 32 bit running in WoW64 mode. */
        PEB32 peb32;
        RTL_USER_PROCESS_PARAMETERS32 procParameters32;

        // read PEB
        if (!ReadProcessMemory(hProcess, ppeb32, &peb32, sizeof(peb32), NULL)) {
            PyErr_SetFromWindowsErr(0);
            goto error;
        }

        // read process parameters
        if (!ReadProcessMemory(hProcess,
                              UlongToPtr(peb32.ProcessParameters),
                              &procParameters32,
                              sizeof(procParameters32),
                              NULL)) {
            PyErr_SetFromWindowsErr(0);
            goto error;
        }

        switch (kind) {
            case KIND_CMDLINE:
                src = UlongToPtr(procParameters32.CommandLine.Buffer),
                size = procParameters32.CommandLine.Length;
                break;
            case KIND_CWD:
                src = UlongToPtr(procParameters32.CurrentDirectoryPath.Buffer);
                size = procParameters32.CurrentDirectoryPath.Length;
                break;
            case KIND_ENVIRON:
                src = UlongToPtr(procParameters32.env);
                break;
        }
    } else
#else
    /* 32 bit case.  Check if the target is also 32 bit. */
    if (!IsWow64Process(GetCurrentProcess(), &weAreWow64) ||
        !IsWow64Process(hProcess, &theyAreWow64)) {
        PyErr_SetFromWindowsErr(0);
        goto error;
    }

    if (weAreWow64 && !theyAreWow64) {
        /* We are 32 bit running in WoW64 mode.  Target process is 64 bit. */
        PROCESS_BASIC_INFORMATION64 pbi64;
        PEB64 peb64;
        RTL_USER_PROCESS_PARAMETERS64 procParameters64;

        if (NtWow64QueryInformationProcess64 == NULL) {
            NtWow64QueryInformationProcess64 = \
                psutil_GetProcAddressFromLib(
                    "ntdll.dll", "NtWow64QueryInformationProcess64");
            if (NtWow64QueryInformationProcess64 == NULL) {
                // Too complicated. Give up.
                AccessDenied("can't query 64-bit process in 32-bit-WoW mode");
                goto error;
            }
        }
        if (NtWow64ReadVirtualMemory64 == NULL) {
            NtWow64ReadVirtualMemory64 = \
                psutil_GetProcAddressFromLib(
                    "ntdll.dll", "NtWow64ReadVirtualMemory64");
            if (NtWow64ReadVirtualMemory64 == NULL) {
                // Too complicated. Give up.
                AccessDenied("can't query 64-bit process in 32-bit-WoW mode");
                goto error;
            }
        }

        if (!NT_SUCCESS(NtWow64QueryInformationProcess64(
                        hProcess,
                        ProcessBasicInformation,
                        &pbi64,
                        sizeof(pbi64),
                        NULL))) {
            PyErr_SetFromWindowsErr(0);
            goto error;
        }

        // read peb
        if (!NT_SUCCESS(NtWow64ReadVirtualMemory64(hProcess,
                                                   pbi64.PebBaseAddress,
                                                   &peb64,
                                                   sizeof(peb64),
                                                   NULL))) {
            PyErr_SetFromWindowsErr(0);
            goto error;
        }

        // read process parameters
        if (!NT_SUCCESS(NtWow64ReadVirtualMemory64(hProcess,
                                                   peb64.ProcessParameters,
                                                   &procParameters64,
                                                   sizeof(procParameters64),
                                                   NULL))) {
            PyErr_SetFromWindowsErr(0);
            goto error;
        }

        switch (kind) {
            case KIND_CMDLINE:
                src64 = procParameters64.CommandLine.Buffer;
                size = procParameters64.CommandLine.Length;
                break;
            case KIND_CWD:
                src64 = procParameters64.CurrentDirectoryPath.Buffer,
                size = procParameters64.CurrentDirectoryPath.Length;
                break;
            case KIND_ENVIRON:
                src64 = procParameters64.env;
                break;
        }
    } else
#endif

    /* Target process is of the same bitness as us. */
    {
        PROCESS_BASIC_INFORMATION pbi;
        PEB_ peb;
        RTL_USER_PROCESS_PARAMETERS_ procParameters;

        if (!NT_SUCCESS(NtQueryInformationProcess(hProcess,
                                                  ProcessBasicInformation,
                                                  &pbi,
                                                  sizeof(pbi),
                                                  NULL))) {
            PyErr_SetFromWindowsErr(0);
            goto error;
        }

        // read peb
        if (!ReadProcessMemory(hProcess,
                               pbi.PebBaseAddress,
                               &peb,
                               sizeof(peb),
                               NULL)) {
            PyErr_SetFromWindowsErr(0);
            goto error;
        }

        // read process parameters
        if (!ReadProcessMemory(hProcess,
                               peb.ProcessParameters,
                               &procParameters,
                               sizeof(procParameters),
                               NULL)) {
            PyErr_SetFromWindowsErr(0);
            goto error;
        }

        switch (kind) {
            case KIND_CMDLINE:
                src = procParameters.CommandLine.Buffer;
                size = procParameters.CommandLine.Length;
                break;
            case KIND_CWD:
                src = procParameters.CurrentDirectoryPath.Buffer;
                size = procParameters.CurrentDirectoryPath.Length;
                break;
            case KIND_ENVIRON:
                src = procParameters.env;
                break;
        }
    }

    if (kind == KIND_ENVIRON) {
#ifndef _WIN64
        if (weAreWow64 && !theyAreWow64) {
            AccessDenied("can't query 64-bit process in 32-bit-WoW mode");
            goto error;
        }
        else
#endif
        if (psutil_get_process_region_size(hProcess, src, &size) != 0)
            goto error;
    }

    buffer = calloc(size + 2, 1);

    if (buffer == NULL) {
        PyErr_NoMemory();
        goto error;
    }

#ifndef _WIN64
    if (weAreWow64 && !theyAreWow64) {
        if (!NT_SUCCESS(NtWow64ReadVirtualMemory64(hProcess,
                                                   src64,
                                                   buffer,
                                                   size,
                                                   NULL))) {
            PyErr_SetFromWindowsErr(0);
            goto error;
        }
    } else
#endif
    if (!ReadProcessMemory(hProcess, src, buffer, size, NULL)) {
        PyErr_SetFromWindowsErr(0);
        goto error;
    }

    CloseHandle(hProcess);

    *pdata = buffer;
    *psize = size;

    return 0;

error:
    if (hProcess != NULL)
        CloseHandle(hProcess);
    if (buffer != NULL)
        free(buffer);
    return -1;
}


/*
 * Get process cmdline() by using NtQueryInformationProcess. This is
 * useful on Windows 8.1+ in order to get less ERROR_ACCESS_DENIED
 * errors when querying privileged PIDs.
 */
static int
psutil_get_cmdline_data(long pid, WCHAR **pdata, SIZE_T *psize) {
    HANDLE hProcess;
    ULONG ret_length = 4096;
    NTSTATUS status;
    char * cmdline_buffer = NULL;
    WCHAR * cmdline_buffer_wchar = NULL;
    PUNICODE_STRING tmp = NULL;
    DWORD string_size;
    _NtQueryInformationProcess NtQueryInformationProcess;

    NtQueryInformationProcess = \
        psutil_GetProcAddress("ntdll.dll", "NtQueryInformationProcess");
    if (NtQueryInformationProcess == NULL)
        goto error;

    cmdline_buffer = calloc(ret_length, 1);
    if (cmdline_buffer == NULL) {
        PyErr_NoMemory();
        goto error;
    }

    hProcess = psutil_handle_from_pid(pid, PROCESS_QUERY_LIMITED_INFORMATION);
    if (hProcess == NULL)
        goto error;
    status = NtQueryInformationProcess(
        hProcess,
        60, // ProcessCommandLineInformation
        cmdline_buffer,
        ret_length,
        &ret_length
    );
    if (!NT_SUCCESS(status)) {
        PyErr_SetFromWindowsErr(0);
        goto error;
    }

    tmp = (PUNICODE_STRING)cmdline_buffer;
    string_size = wcslen(tmp->Buffer) + 1;
    cmdline_buffer_wchar = (WCHAR *)calloc(string_size, sizeof(WCHAR));

    if (cmdline_buffer_wchar == NULL) {
        PyErr_NoMemory();
        goto error;
    }

    wcscpy_s(cmdline_buffer_wchar, string_size, tmp->Buffer);
    *pdata = cmdline_buffer_wchar;
    *psize = string_size * sizeof(WCHAR);
    free(cmdline_buffer);
    CloseHandle(hProcess);
    return 0;

error:
    if (cmdline_buffer != NULL)
        free(cmdline_buffer);
    if (hProcess != NULL)
        CloseHandle(hProcess);
    return -1;
}


/*
 * Return a Python list representing the arguments for the process
 * with given pid or NULL on error.
 */
PyObject *
psutil_get_cmdline(long pid, int use_peb) {
    PyObject *ret = NULL;
    WCHAR *data = NULL;
    SIZE_T size;
    PyObject *py_retlist = NULL;
    PyObject *py_unicode = NULL;
    LPWSTR *szArglist = NULL;
    int nArgs, i;
    int func_ret;

    /*
    By defaut, still use PEB (if command line params have been patched in
    the PEB, we will get the actual ones). Reading the PEB to get the
    command line parameters still seem to be the best method if somebody
    has tampered with the parameters after creating the process.
    For instance, create a process as suspended, patch the command line
    in its PEB and unfreeze it.
    The process will use the "new" parameters whereas the system
    (with NtQueryInformationProcess) will give you the "old" ones
    See:
    - https://github.com/giampaolo/psutil/pull/1398
    - https://blog.xpnsec.com/how-to-argue-like-cobalt-strike/
    */
    if (use_peb == 1) {
        func_ret = psutil_get_process_data(pid, KIND_CMDLINE, &data, &size);
    }
    else {
        func_ret = psutil_get_cmdline_data(pid, &data, &size);
    }
    if (func_ret != 0)
        goto out;

    // attempt to parse the command line using Win32 API
    szArglist = CommandLineToArgvW(data, &nArgs);
    if (szArglist == NULL) {
        PyErr_SetFromWindowsErr(0);
        goto out;
    }

    // arglist parsed as array of UNICODE_STRING, so convert each to
    // Python string object and add to arg list
    py_retlist = PyList_New(nArgs);
    if (py_retlist == NULL)
        goto out;
    for (i = 0; i < nArgs; i++) {
        py_unicode = PyUnicode_FromWideChar(szArglist[i],
            wcslen(szArglist[i]));
        if (py_unicode == NULL)
            goto out;
        PyList_SET_ITEM(py_retlist, i, py_unicode);
        py_unicode = NULL;
    }
    ret = py_retlist;
    py_retlist = NULL;

out:
    if (szArglist != NULL)
        LocalFree(szArglist);
    if (data != NULL)
        free(data);
    Py_XDECREF(py_unicode);
    Py_XDECREF(py_retlist);
    return ret;
}


PyObject *
psutil_get_cwd(long pid) {
    PyObject *ret = NULL;
    WCHAR *data = NULL;
    SIZE_T size;

    if (psutil_get_process_data(pid, KIND_CWD, &data, &size) != 0)
        goto out;

    // convert wchar array to a Python unicode string
    ret = PyUnicode_FromWideChar(data, wcslen(data));

out:
    if (data != NULL)
        free(data);

    return ret;
}


/*
 * returns a Python string containing the environment variable data for the
 * process with given pid or NULL on error.
 */
PyObject *
psutil_get_environ(long pid) {
    PyObject *ret = NULL;
    WCHAR *data = NULL;
    SIZE_T size;

    if (psutil_get_process_data(pid, KIND_ENVIRON, &data, &size) != 0)
        goto out;

    // convert wchar array to a Python unicode string
    ret = PyUnicode_FromWideChar(data, size / 2);

out:
    if (data != NULL)
        free(data);
    return ret;
}


/*
 * Given a process PID and a PSYSTEM_PROCESS_INFORMATION structure
 * fills the structure with various process information by using
 * NtQuerySystemInformation.
 * We use this as a fallback when faster functions fail with access
 * denied. This is slower because it iterates over all processes.
 * On success return 1, else 0 with Python exception already set.
 */
int
psutil_get_proc_info(DWORD pid, PSYSTEM_PROCESS_INFORMATION *retProcess,
                     PVOID *retBuffer) {
    static ULONG initialBufferSize = 0x4000;
    NTSTATUS status;
    PVOID buffer;
    ULONG bufferSize;
    PSYSTEM_PROCESS_INFORMATION process;
    typedef DWORD (_stdcall * NTQSI_PROC) (int, PVOID, ULONG, PULONG);
    NTQSI_PROC NtQuerySystemInformation;

    NtQuerySystemInformation = \
        psutil_GetProcAddressFromLib("ntdll.dll", "NtQuerySystemInformation");
    if (NtQuerySystemInformation == NULL)
        goto error;

    bufferSize = initialBufferSize;
    buffer = malloc(bufferSize);
    if (buffer == NULL) {
        PyErr_NoMemory();
        goto error;
    }

    while (TRUE) {
        status = NtQuerySystemInformation(SystemProcessInformation, buffer,
                                          bufferSize, &bufferSize);
        if (status == STATUS_BUFFER_TOO_SMALL ||
                status == STATUS_INFO_LENGTH_MISMATCH)
        {
            free(buffer);
            buffer = malloc(bufferSize);
            if (buffer == NULL) {
                PyErr_NoMemory();
                goto error;
            }
        }
        else {
            break;
        }
    }

    if (status != 0) {
        PyErr_Format(
            PyExc_RuntimeError, "NtQuerySystemInformation() syscall failed");
        goto error;
    }

    if (bufferSize <= 0x20000)
        initialBufferSize = bufferSize;

    process = PSUTIL_FIRST_PROCESS(buffer);
    do {
        if (process->UniqueProcessId == (HANDLE)pid) {
            *retProcess = process;
            *retBuffer = buffer;
            return 1;
        }
    } while ( (process = PSUTIL_NEXT_PROCESS(process)) );

    NoSuchProcess("");
    goto error;

error:
    if (buffer != NULL)
        free(buffer);
    return 0;
}
