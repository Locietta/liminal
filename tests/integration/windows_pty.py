"""Windows pseudoconsole support for integration tests."""

import ctypes
import os
import subprocess
import time
from ctypes import wintypes


if os.name != "nt":
    raise ImportError("ConPTY is only available on Windows")


kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

HPCON = wintypes.HANDLE
HRESULT = ctypes.c_long
SIZE_T = ctypes.c_size_t
LPPROC_THREAD_ATTRIBUTE_LIST = ctypes.c_void_p

PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE = 0x00020016
EXTENDED_STARTUPINFO_PRESENT = 0x00080000
CREATE_UNICODE_ENVIRONMENT = 0x00000400
WAIT_OBJECT_0 = 0
WAIT_TIMEOUT = 258
ERROR_BROKEN_PIPE = 109


class COORD(ctypes.Structure):
    _fields_ = [("X", wintypes.SHORT), ("Y", wintypes.SHORT)]


class STARTUPINFOW(ctypes.Structure):
    _fields_ = [
        ("cb", wintypes.DWORD),
        ("lpReserved", wintypes.LPWSTR),
        ("lpDesktop", wintypes.LPWSTR),
        ("lpTitle", wintypes.LPWSTR),
        ("dwX", wintypes.DWORD),
        ("dwY", wintypes.DWORD),
        ("dwXSize", wintypes.DWORD),
        ("dwYSize", wintypes.DWORD),
        ("dwXCountChars", wintypes.DWORD),
        ("dwYCountChars", wintypes.DWORD),
        ("dwFillAttribute", wintypes.DWORD),
        ("dwFlags", wintypes.DWORD),
        ("wShowWindow", wintypes.WORD),
        ("cbReserved2", wintypes.WORD),
        ("lpReserved2", ctypes.POINTER(wintypes.BYTE)),
        ("hStdInput", wintypes.HANDLE),
        ("hStdOutput", wintypes.HANDLE),
        ("hStdError", wintypes.HANDLE),
    ]


class STARTUPINFOEXW(ctypes.Structure):
    _fields_ = [
        ("StartupInfo", STARTUPINFOW),
        ("lpAttributeList", LPPROC_THREAD_ATTRIBUTE_LIST),
    ]


class PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("hProcess", wintypes.HANDLE),
        ("hThread", wintypes.HANDLE),
        ("dwProcessId", wintypes.DWORD),
        ("dwThreadId", wintypes.DWORD),
    ]


kernel32.CreatePipe.argtypes = [
    ctypes.POINTER(wintypes.HANDLE),
    ctypes.POINTER(wintypes.HANDLE),
    ctypes.c_void_p,
    wintypes.DWORD,
]
kernel32.CreatePipe.restype = wintypes.BOOL
kernel32.CreatePseudoConsole.argtypes = [
    COORD,
    wintypes.HANDLE,
    wintypes.HANDLE,
    wintypes.DWORD,
    ctypes.POINTER(HPCON),
]
kernel32.CreatePseudoConsole.restype = HRESULT
kernel32.ResizePseudoConsole.argtypes = [HPCON, COORD]
kernel32.ResizePseudoConsole.restype = HRESULT
kernel32.ClosePseudoConsole.argtypes = [HPCON]
kernel32.InitializeProcThreadAttributeList.argtypes = [
    LPPROC_THREAD_ATTRIBUTE_LIST,
    wintypes.DWORD,
    wintypes.DWORD,
    ctypes.POINTER(SIZE_T),
]
kernel32.InitializeProcThreadAttributeList.restype = wintypes.BOOL
kernel32.UpdateProcThreadAttribute.argtypes = [
    LPPROC_THREAD_ATTRIBUTE_LIST,
    wintypes.DWORD,
    SIZE_T,
    ctypes.c_void_p,
    SIZE_T,
    ctypes.c_void_p,
    ctypes.c_void_p,
]
kernel32.UpdateProcThreadAttribute.restype = wintypes.BOOL
kernel32.DeleteProcThreadAttributeList.argtypes = [LPPROC_THREAD_ATTRIBUTE_LIST]
kernel32.CreateProcessW.argtypes = [
    wintypes.LPCWSTR,
    wintypes.LPWSTR,
    ctypes.c_void_p,
    ctypes.c_void_p,
    wintypes.BOOL,
    wintypes.DWORD,
    ctypes.c_void_p,
    wintypes.LPCWSTR,
    ctypes.POINTER(STARTUPINFOW),
    ctypes.POINTER(PROCESS_INFORMATION),
]
kernel32.CreateProcessW.restype = wintypes.BOOL
kernel32.PeekNamedPipe.argtypes = [
    wintypes.HANDLE,
    ctypes.c_void_p,
    wintypes.DWORD,
    ctypes.c_void_p,
    ctypes.POINTER(wintypes.DWORD),
    ctypes.c_void_p,
]
kernel32.PeekNamedPipe.restype = wintypes.BOOL
kernel32.ReadFile.argtypes = [
    wintypes.HANDLE,
    ctypes.c_void_p,
    wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD),
    ctypes.c_void_p,
]
kernel32.ReadFile.restype = wintypes.BOOL
kernel32.WriteFile.argtypes = [
    wintypes.HANDLE,
    ctypes.c_void_p,
    wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD),
    ctypes.c_void_p,
]
kernel32.WriteFile.restype = wintypes.BOOL
kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
kernel32.WaitForSingleObject.restype = wintypes.DWORD
kernel32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
kernel32.GetExitCodeProcess.restype = wintypes.BOOL
kernel32.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
kernel32.TerminateProcess.restype = wintypes.BOOL
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
kernel32.CloseHandle.restype = wintypes.BOOL


def _win_error(message):
    raise OSError(ctypes.get_last_error(), message)


def _check_bool(result, message):
    if not result:
        _win_error(message)


def _check_hresult(result, message):
    if result < 0:
        raise OSError(result & 0xFFFFFFFF, message)


def _pipe():
    read = wintypes.HANDLE()
    write = wintypes.HANDLE()
    _check_bool(
        kernel32.CreatePipe(ctypes.byref(read), ctypes.byref(write), None, 0),
        "CreatePipe failed",
    )
    return read, write


def _environment_block(environment):
    entries = [
        f"{key}={value}"
        for key, value in sorted(environment.items(), key=lambda item: item[0].upper())
    ]
    return ctypes.create_unicode_buffer("\0".join(entries) + "\0\0")


class ConPtyProcess:
    def __init__(self, args, *, cwd, env, columns, rows):
        self._input = None
        self._output = None
        self._pseudoconsole = None
        self._process = None
        self.pid = -1

        env = env.copy()
        env["LIGHTER_CONPTY"] = "1"
        input_read, input_write = _pipe()
        output_read, output_write = _pipe()
        try:
            pseudoconsole = HPCON()
            _check_hresult(
                kernel32.CreatePseudoConsole(
                    COORD(columns, rows),
                    input_read,
                    output_write,
                    0,
                    ctypes.byref(pseudoconsole),
                ),
                "CreatePseudoConsole failed",
            )
            self._pseudoconsole = pseudoconsole

            attribute_size = SIZE_T()
            kernel32.InitializeProcThreadAttributeList(
                None, 1, 0, ctypes.byref(attribute_size)
            )
            attribute_storage = ctypes.create_string_buffer(attribute_size.value)
            attribute_list = ctypes.cast(
                attribute_storage, LPPROC_THREAD_ATTRIBUTE_LIST
            )
            _check_bool(
                kernel32.InitializeProcThreadAttributeList(
                    attribute_list, 1, 0, ctypes.byref(attribute_size)
                ),
                "InitializeProcThreadAttributeList failed",
            )
            try:
                _check_bool(
                    kernel32.UpdateProcThreadAttribute(
                        attribute_list,
                        0,
                        PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                        pseudoconsole,
                        ctypes.sizeof(pseudoconsole),
                        None,
                        None,
                    ),
                    "UpdateProcThreadAttribute failed",
                )
                startup = STARTUPINFOEXW()
                startup.StartupInfo.cb = ctypes.sizeof(startup)
                startup.lpAttributeList = attribute_list
                process = PROCESS_INFORMATION()
                command_line = ctypes.create_unicode_buffer(
                    subprocess.list2cmdline([str(arg) for arg in args])
                )
                environment = _environment_block(env)
                _check_bool(
                    kernel32.CreateProcessW(
                        None,
                        command_line,
                        None,
                        None,
                        False,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                        environment,
                        str(cwd),
                        ctypes.byref(startup.StartupInfo),
                        ctypes.byref(process),
                    ),
                    "CreateProcessW failed",
                )
            finally:
                kernel32.DeleteProcThreadAttributeList(attribute_list)

            kernel32.CloseHandle(process.hThread)
            kernel32.CloseHandle(input_read)
            input_read = None
            kernel32.CloseHandle(output_write)
            output_write = None
            self._input = input_write
            self._output = output_read
            self._process = process.hProcess
            self.pid = process.dwProcessId
            input_write = None
            output_read = None
        finally:
            for handle in (input_read, input_write, output_read, output_write):
                if handle:
                    kernel32.CloseHandle(handle)
            if self._process is None and self._pseudoconsole:
                kernel32.ClosePseudoConsole(self._pseudoconsole)
                self._pseudoconsole = None

    def write(self, data):
        offset = 0
        while offset < len(data):
            written = wintypes.DWORD()
            chunk = ctypes.create_string_buffer(data[offset:])
            _check_bool(
                kernel32.WriteFile(
                    self._input, chunk, len(chunk.raw) - 1, ctypes.byref(written), None
                ),
                "writing ConPTY input failed",
            )
            if written.value == 0:
                raise OSError("writing ConPTY input made no progress")
            offset += written.value

    def read(self, timeout):
        deadline = time.monotonic() + timeout
        while True:
            available = wintypes.DWORD()
            if not kernel32.PeekNamedPipe(
                self._output, None, 0, None, ctypes.byref(available), None
            ):
                error = ctypes.get_last_error()
                if error == ERROR_BROKEN_PIPE:
                    return b""
                raise OSError(error, "peeking ConPTY output failed")
            if available.value:
                size = min(available.value, 4096)
                buffer = ctypes.create_string_buffer(size)
                count = wintypes.DWORD()
                _check_bool(
                    kernel32.ReadFile(
                        self._output, buffer, size, ctypes.byref(count), None
                    ),
                    "reading ConPTY output failed",
                )
                return buffer.raw[: count.value]
            if time.monotonic() >= deadline:
                return b""
            time.sleep(0.01)

    def resize(self, columns, rows):
        _check_hresult(
            kernel32.ResizePseudoConsole(self._pseudoconsole, COORD(columns, rows)),
            "ResizePseudoConsole failed",
        )

    def poll(self):
        result = kernel32.WaitForSingleObject(self._process, 0)
        if result == WAIT_TIMEOUT:
            return None
        if result != WAIT_OBJECT_0:
            _win_error("waiting for ConPTY process failed")
        return self._exit_code()

    def wait(self, timeout):
        milliseconds = max(0, min(round(timeout * 1000), 0xFFFFFFFE))
        result = kernel32.WaitForSingleObject(self._process, milliseconds)
        if result == WAIT_TIMEOUT:
            raise subprocess.TimeoutExpired("ConPTY process", timeout)
        if result != WAIT_OBJECT_0:
            _win_error("waiting for ConPTY process failed")
        return self._exit_code()

    def kill(self):
        _check_bool(
            kernel32.TerminateProcess(self._process, 1),
            "terminating ConPTY process failed",
        )

    def close(self):
        if self._input:
            kernel32.CloseHandle(self._input)
            self._input = None
        if self._pseudoconsole:
            kernel32.ClosePseudoConsole(self._pseudoconsole)
            self._pseudoconsole = None
        if self._output:
            kernel32.CloseHandle(self._output)
            self._output = None
        if self._process:
            kernel32.CloseHandle(self._process)
            self._process = None

    def _exit_code(self):
        exit_code = wintypes.DWORD()
        _check_bool(
            kernel32.GetExitCodeProcess(self._process, ctypes.byref(exit_code)),
            "getting ConPTY exit code failed",
        )
        return exit_code.value
