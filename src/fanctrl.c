/* A fan control application for Lenovo laptops, e.g. Ideapad, Xiaoxin and etc.
 * 
 * References:
 * https://github.com/bitrate16/FanControl/blob/main/FanControl/FanControl.cpp
 * https://github.com/Soberia/Lenovo-IdeaPad-Z500-Fan-Controller?tab=readme-ov-file#-about
 * https://www.allstone.lt/ideafan/
 */

#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <math.h>
#include <Windows.h>

#include "fanctrl.h"

volatile int is_keep_fan_running = 0;
volatile int is_keep_fan_speed_low = 0;

/* ---------- Small diagnostic log: %APPDATA%\LenovoFanControl\fan.log ---------- */

static wchar_t log_path[MAX_PATH];
static int log_ready = 0;

static void log_init(void) {
    wchar_t dir[MAX_PATH];
    log_ready = 1;
    log_path[0] = 0;
    DWORD n = GetEnvironmentVariableW(L"APPDATA", dir, MAX_PATH);
    if (n == 0 || n + 32 >= MAX_PATH) {
        return;
    }
    wcscat(dir, L"\\LenovoFanControl");
    CreateDirectoryW(dir, NULL);
    wcscpy(log_path, dir);
    wcscat(log_path, L"\\fan.log");
}

void fan_log(const char *fmt, ...) {
    char line[256];
    SYSTEMTIME st;
    va_list ap;

    if (!log_ready) {
        log_init();
    }
    if (log_path[0] == 0) {
        return;
    }
    GetLocalTime(&st);
    int n = _snprintf(line, 32, "%02d:%02d:%02d ", st.wHour, st.wMinute, st.wSecond);
    va_start(ap, fmt);
    int m = _vsnprintf(line + n, sizeof(line) - n - 3, fmt, ap);
    va_end(ap);
    if (m < 0) {
        m = (int)sizeof(line) - n - 3;  /* truncated */
    }
    n += m;
    line[n++] = '\r';
    line[n++] = '\n';

    HANDLE f = CreateFileW(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        return;
    }
    LARGE_INTEGER size;
    if (GetFileSizeEx(f, &size) && size.QuadPart > 200000) {
        /* Keep the log small: start over once it grows past ~200 KB. */
        CloseHandle(f);
        f = CreateFileW(log_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f == INVALID_HANDLE_VALUE) {
            return;
        }
    }
    DWORD written = 0;
    WriteFile(f, line, (DWORD)n, &written, NULL);
    CloseHandle(f);
}

/* ---------- EC access ---------- */

int fan_control(enum FanMode mode) {
    HANDLE hndl = CreateFileW(L"\\\\.\\EnergyDrv", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hndl == INVALID_HANDLE_VALUE) {
        return -1;
    }
    // lpInBuffer value: 06 00 00 00  01 00 00 00  01 00 00 00 ~ [ 6, 1, 1 ] (inv endian)
    DWORD inBuffer[3] = { 6, 1 };
    inBuffer[2] = mode;
    DWORD bytesReturned = 0;

    DeviceIoControl(hndl, 0x831020C0, inBuffer, sizeof(inBuffer), NULL, 0, &bytesReturned, NULL);
    CloseHandle(hndl);

    return 1;
}

/* Read the raw EC fan register. Returns 1 on success, 0 if the device could not be
 * read (for example when another program has it open for a moment). */
static int read_raw(DWORD *out) {
    HANDLE hndl = CreateFileW(L"\\\\.\\EnergyDrv", GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hndl == INVALID_HANDLE_VALUE) {
        return 0;
    }
    // lpInBuffer value: 0E 00 00 00 ~ [ 14 ] (inv endian)
    DWORD inBuffer[1] = { 14 };
    DWORD outBuffer[1] = { 0xFFFFFFFF };
    DWORD bytesReturned = 0;

    BOOL ok = DeviceIoControl(hndl, 0x831020C4, inBuffer, sizeof(inBuffer), outBuffer, sizeof(outBuffer), &bytesReturned, NULL);
    CloseHandle(hndl);

    if (!ok && outBuffer[0] == 0xFFFFFFFF) {
        return 0;
    }
    *out = outBuffer[0];
    return 1;
}

static int have_normal = 0;   /* normal_value is known */
static DWORD normal_value = 0;

/* Put the fan in NORMAL and remember what the EC register looks like in that state. */
static void learn_normal(void) {
    fan_control(NORMAL);
    Sleep(50);
    for (int i = 0; i < 3; ++i) {
        DWORD raw;
        if (read_raw(&raw)) {
            normal_value = raw;
            have_normal = 1;
            return;
        }
        Sleep(50);
    }
}

enum FanMode read_state() {
    if (!have_normal) {
        // Set fan spinning mode to NORMAL to get the normal register value at the first run
        learn_normal();
    }
    DWORD raw;
    if (!read_raw(&raw)) {
        return -1;
    }
    if (!have_normal) {
        normal_value = raw;
        have_normal = 1;
    }
    return raw == normal_value ? NORMAL : FAST;
}

/* Each call of keep_fan_running() gets a new token; an older call that is still
 * winding down notices the change and leaves the fan alone. */
static volatile LONG keep_fan_token = 0;

static int have_fast = 0;     /* fast_value is known */
static DWORD fast_value = 0;

/* Ask for FAST (retrying if the device is busy), then remember what the EC
 * register looks like once FAST has taken effect. */
static void send_fast(void) {
    for (int i = 0; i < 5; ++i) {
        if (fan_control(FAST) == 1) {
            break;
        }
        Sleep(20);
    }
    Sleep(300);
    DWORD raw;
    if (read_raw(&raw) && !(have_normal && raw == normal_value)) {
        fast_value = raw;
        have_fast = 1;
    } else {
        have_fast = 0;
    }
}

void keep_fan_running() {
    LONG token = InterlockedIncrement(&keep_fan_token);
    is_keep_fan_speed_low = 0;
    is_keep_fan_running = 1;

    // Learn the NORMAL register value fresh for this session, then go to FAST.
    learn_normal();
    send_fast();
    fan_log("high: started, normal=0x%lx fast=%s0x%lx", (unsigned long)normal_value,
            have_fast ? "" : "unknown/", (unsigned long)fast_value);

    // Watch the EC while HIGH mode is active. If the BIOS/EC takes the fan out of FAST,
    // switch NORMAL -> FAST again (the EC only accepts FAST after a NORMAL -> FAST
    // transition). A failed read is NOT treated as a change, and a change must be seen
    // on consecutive checks before acting, so a busy device can't cause needless switching.
    const int poll_interval = 100; // ms
    int bad = 0;
    int fails = 0;
    DWORD last_fail_log = 0;
    DWORD last_reassert = GetTickCount();
    while (is_keep_fan_running && token == keep_fan_token) {
        DWORD raw;
        if (!read_raw(&raw)) {
            fails++;
            if (GetTickCount() - last_fail_log > 10000) {
                last_fail_log = GetTickCount();
                fan_log("high: could not read EC state (%d failures so far), ignoring", fails);
            }
        } else {
            int is_normal = have_normal && raw == normal_value;
            int changed = have_fast && raw != fast_value;
            if (is_normal || changed) {
                bad++;
            } else {
                bad = 0;
            }
            DWORD since = GetTickCount() - last_reassert;
            if ((is_normal && bad >= 2) || (changed && bad >= 3 && since >= 3000)) {
                fan_log("high: fan left FAST (raw=0x%lx normal=0x%lx fast=0x%lx after %lu s), re-asserting",
                        (unsigned long)raw, (unsigned long)normal_value, (unsigned long)fast_value,
                        (unsigned long)(since / 1000));
                fan_control(NORMAL);
                Sleep(50);
                send_fast();
                bad = 0;
                last_reassert = GetTickCount();
            }
        }
        Sleep(poll_interval);
    }

    // Only restore NORMAL once HIGH mode has actually been cancelled (and no newer
    // keep_fan_running() has taken over).
    if (token == keep_fan_token) {
        fan_control(NORMAL);
    }
    fan_log("high: stopped");
}

void keep_fan_speed_low() {
    is_keep_fan_running = 0;
    is_keep_fan_speed_low = 1;
    const int TOTAL_SLEEP_TIME = 5000; // ms
    const int CHECK_STATUS_INTERVAL = 1000; // ms
    const int CHECK_STATUS_COUNT = ceil((double)TOTAL_SLEEP_TIME / CHECK_STATUS_INTERVAL);
    while (is_keep_fan_speed_low) {
        fan_control(NORMAL);
        for (int i = 0; i < CHECK_STATUS_COUNT && is_keep_fan_speed_low; ++i) {
            Sleep(CHECK_STATUS_INTERVAL);
        }
    }
}