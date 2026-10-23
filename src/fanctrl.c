/* A fan control application for Lenovo laptops, e.g. Ideapad, Xiaoxin and etc.
 * 
 * References:
 * https://github.com/bitrate16/FanControl/blob/main/FanControl/FanControl.cpp
 * https://github.com/Soberia/Lenovo-IdeaPad-Z500-Fan-Controller?tab=readme-ov-file#-about
 * https://www.allstone.lt/ideafan/
 */

#include <stdio.h>
#include <time.h>
#include <math.h>
#include <Windows.h>

#include "fanctrl.h"

volatile int is_keep_fan_running = 0;
volatile int is_keep_fan_speed_low = 0;

static int NORMAL_MODE_EXPECTED_VALUE = -1;

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

enum FanMode read_state() {
    if (NORMAL_MODE_EXPECTED_VALUE == -1) {
        // Set fan spinning mode to NORMAL to get NORMAL_MODE_EXPECTED_VALUE at the first run
        fan_control(NORMAL);
        Sleep(50);
    }
    HANDLE hndl = CreateFileW(L"\\\\.\\EnergyDrv", GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hndl == INVALID_HANDLE_VALUE) {
        return -1;
    }
    // lpInBuffer value: 0E 00 00 00 ~ [ 14 ] (inv endian)
    DWORD inBuffer[1] = { 14 };
    DWORD outBuffer[1];
    DWORD bytesReturned = 0;

    DeviceIoControl(hndl, 0x831020C4, inBuffer, sizeof(inBuffer), outBuffer, sizeof(outBuffer), &bytesReturned, NULL);
    CloseHandle(hndl);

    if (NORMAL_MODE_EXPECTED_VALUE == -1) {
        // Set this value when the fan is in normal mode at the first run
        NORMAL_MODE_EXPECTED_VALUE = outBuffer[0];
    }
    return outBuffer[0] == NORMAL_MODE_EXPECTED_VALUE ? NORMAL : FAST;
}

void keep_fan_running() {
    is_keep_fan_speed_low = 0;
    is_keep_fan_running = 1;

    // Set the fan to FAST once initially.
    fan_control(FAST);

    // Continuously monitor the EC's reported fan state while HIGH mode is
    // active. If the BIOS/EC changes the state away from FAST on its own,
    // re-assert FAST. The EC requires a NORMAL -> FAST transition to accept
    // FAST again (writing FAST directly while it's already off of FAST is
    // ignored), so cycle through NORMAL first. Only do this when the state
    // has actually changed -- never write redundantly if it's already FAST.
    const int poll_interval = 100; // ms
    while (is_keep_fan_running) {
        if (read_state() != FAST) {
            fan_control(NORMAL);
            Sleep(50);
            fan_control(FAST);
        }
        Sleep(poll_interval);
    }

    // Only restore NORMAL once HIGH mode has actually been cancelled.
    fan_control(NORMAL);
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