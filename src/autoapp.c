#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <tlhelp32.h>
#include <commdlg.h>
#include <wchar.h>
#include <string.h>

#include "autoapp.h"

#define AUTOAPP_TIMER_ID 7001
#define POLL_INTERVAL_MS 2000   /* how often the process list is checked */
#define COOLDOWN_MS      15000  /* wait after the app closes before going back to Normal */

#define MAX_APPS 16

enum { IDC_APP = 2001, IDC_ADD, IDC_BROWSE, IDC_LIST, IDC_REMOVE, IDC_SPEED, IDC_HINT, IDC_ENABLE };

typedef struct {
    const wchar_t *menu;
    const wchar_t *title;
    const wchar_t *pick_app;
    const wchar_t *browse;
    const wchar_t *add;
    const wchar_t *remove;
    const wchar_t *pick_speed;
    const wchar_t *speed_high;
    const wchar_t *speed_low;
    const wchar_t *speed_normal;
    const wchar_t *hint;
    const wchar_t *enable;
    const wchar_t *ok;
    const wchar_t *cancel;
    const wchar_t *need_app;
} Strings;

static const Strings str_en = {
    L"Auto (App Trigger)...",
    L"Auto Mode",
    L"Add the apps that trigger a fan speed:",
    L"Browse...",
    L"Add",
    L"Remove",
    L"Fan speed while the app is running:",
    L"High Speed",
    L"Low Speed",
    L"Normal Speed",
    L"Returns to Normal 15 s after the last app closes.",
    L"Enable auto mode",
    L"OK",
    L"Cancel",
    L"Please add at least one app."
};

static const Strings str_zh = {
    L"自动模式（按应用触发）...",
    L"自动模式",
    L"添加触发风扇转速的应用程序：",
    L"浏览...",
    L"添加",
    L"移除",
    L"应用运行时的风扇转速：",
    L"高转速",
    L"低转速",
    L"正常转速",
    L"最后一个应用关闭 15 秒后恢复正常转速。",
    L"启用自动模式",
    L"确定",
    L"取消",
    L"请至少添加一个应用程序。"
};

static const Strings *S = &str_en;

int autoapp_applying = 0;

static struct {
    int enabled;
    int count;
    wchar_t apps[MAX_APPS][MAX_PATH];
    int speed;
} cfg;

static HWND g_main = NULL;
static HWND g_dlg = NULL;
static autoapp_apply_fn g_apply = NULL;

/* Runtime state */
static int was_running = 0;  /* the app was seen running at the last transition */
static int cooling = 0;      /* the app closed; waiting out the cool-down */
static int applied = 0;      /* Auto mode (not the user) set the current speed */
static DWORD cool_start = 0;

static wchar_t ini_path[MAX_PATH];

static void pick_strings(void) {
    S = (PRIMARYLANGID(GetUserDefaultLangID()) == LANG_CHINESE) ? &str_zh : &str_en;
}

/* ---------- Settings (%APPDATA%\LenovoFanControl\auto.ini) ---------- */

static void build_ini_path(void) {
    wchar_t dir[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", dir, MAX_PATH);
    if (n > 0 && n + 32 < MAX_PATH) {
        wcscat(dir, L"\\LenovoFanControl");
        CreateDirectoryW(dir, NULL);
        wcscpy(ini_path, dir);
        wcscat(ini_path, L"\\auto.ini");
    } else {
        /* Fallback: next to the executable */
        DWORD m = GetModuleFileNameW(NULL, ini_path, MAX_PATH);
        wchar_t *slash = wcsrchr(ini_path, L'\\');
        if (m > 0 && slash && (size_t)(slash - ini_path) + 10 < MAX_PATH) {
            wcscpy(slash + 1, L"auto.ini");
        } else {
            wcscpy(ini_path, L"auto.ini");
        }
    }
}

static void app_key(wchar_t *buf, int i) {
    /* "App0".."App15" (swprintf is avoided on purpose: it differs on MinGW) */
    buf[0] = L'A'; buf[1] = L'p'; buf[2] = L'p';
    if (i < 10) {
        buf[3] = (wchar_t)(L'0' + i);
        buf[4] = 0;
    } else {
        buf[3] = (wchar_t)(L'0' + i / 10);
        buf[4] = (wchar_t)(L'0' + i % 10);
        buf[5] = 0;
    }
}

static void load_settings(void) {
    wchar_t key[8];
    cfg.enabled = GetPrivateProfileIntW(L"Auto", L"Enabled", 0, ini_path) ? 1 : 0;
    cfg.speed = (int)GetPrivateProfileIntW(L"Auto", L"Speed", AUTOAPP_SPEED_HIGH, ini_path);
    if (cfg.speed < 0 || cfg.speed > 2) {
        cfg.speed = AUTOAPP_SPEED_HIGH;
    }
    cfg.count = 0;

    int n = (int)GetPrivateProfileIntW(L"Auto", L"Count", -1, ini_path);
    if (n < 0) {
        /* Older single-app settings file */
        GetPrivateProfileStringW(L"Auto", L"App", L"", cfg.apps[0], MAX_PATH, ini_path);
        if (cfg.apps[0][0] != 0) {
            cfg.count = 1;
        }
        return;
    }
    if (n > MAX_APPS) n = MAX_APPS;
    for (int i = 0; i < n; ++i) {
        app_key(key, i);
        GetPrivateProfileStringW(L"Auto", key, L"", cfg.apps[cfg.count], MAX_PATH, ini_path);
        if (cfg.apps[cfg.count][0] != 0) {
            cfg.count++;
        }
    }
}

static void save_settings(void) {
    wchar_t key[8];
    wchar_t num[4];
    WritePrivateProfileStringW(L"Auto", L"Enabled", cfg.enabled ? L"1" : L"0", ini_path);
    num[0] = (wchar_t)(L'0' + cfg.speed);
    num[1] = 0;
    WritePrivateProfileStringW(L"Auto", L"Speed", num, ini_path);

    if (cfg.count < 10) {
        num[0] = (wchar_t)(L'0' + cfg.count);
        num[1] = 0;
    } else {
        num[0] = (wchar_t)(L'0' + cfg.count / 10);
        num[1] = (wchar_t)(L'0' + cfg.count % 10);
        num[2] = 0;
    }
    WritePrivateProfileStringW(L"Auto", L"Count", num, ini_path);

    for (int i = 0; i < MAX_APPS; ++i) {
        app_key(key, i);
        WritePrivateProfileStringW(L"Auto", key, i < cfg.count ? cfg.apps[i] : NULL, ini_path);
    }
    WritePrivateProfileStringW(L"Auto", L"App", NULL, ini_path);  /* drop the old single-app key */
}

/* Trim spaces, drop any folder path, make sure the name ends with ".exe". */
static void normalize_app(wchar_t *s) {
    wchar_t *p = s;
    while (*p == L' ') p++;
    if (p != s) memmove(s, p, (wcslen(p) + 1) * sizeof(wchar_t));

    size_t n = wcslen(s);
    while (n > 0 && s[n - 1] == L' ') s[--n] = 0;

    wchar_t *b = wcsrchr(s, L'\\');
    wchar_t *f = wcsrchr(s, L'/');
    if (!b || (f && f > b)) b = f;
    if (b) memmove(s, b + 1, (wcslen(b + 1) + 1) * sizeof(wchar_t));

    n = wcslen(s);
    if (n > 0 && (n < 4 || _wcsicmp(s + n - 4, L".exe") != 0) && n + 4 < MAX_PATH) {
        wcscat(s, L".exe");
    }
}

/* ---------- Monitoring ---------- */

/* True if any of the configured apps is running (one process snapshot per check). */
static int is_any_app_running(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return 0;
    }
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    int found = 0;
    BOOL ok = Process32FirstW(snap, &pe);
    while (ok && !found) {
        for (int i = 0; i < cfg.count; ++i) {
            if (_wcsicmp(pe.szExeFile, cfg.apps[i]) == 0) {
                found = 1;
                break;
            }
        }
        ok = Process32NextW(snap, &pe);
    }
    CloseHandle(snap);
    return found;
}

static void do_apply(int speed) {
    if (!g_apply) {
        return;
    }
    autoapp_applying = 1;
    g_apply(speed);
    autoapp_applying = 0;
}

static void poll(void) {
    if (!cfg.enabled || cfg.count == 0) {
        return;
    }
    if (is_any_app_running()) {
        cooling = 0;
        if (!was_running) {
            was_running = 1;
            applied = 1;
            do_apply(cfg.speed);
        }
    } else if (was_running) {
        if (!cooling) {
            cooling = 1;
            cool_start = GetTickCount();
        } else if (GetTickCount() - cool_start >= COOLDOWN_MS) {
            was_running = 0;
            cooling = 0;
            if (applied) {
                applied = 0;
                do_apply(AUTOAPP_SPEED_NORMAL);
            }
        }
    }
}

static void CALLBACK on_timer(HWND hwnd, UINT msg, UINT_PTR id, DWORD time) {
    (void)hwnd; (void)msg; (void)id; (void)time;
    poll();
}

static void start_monitoring(void) {
    was_running = 0;
    cooling = 0;
    applied = 0;
    SetTimer(g_main, AUTOAPP_TIMER_ID, POLL_INTERVAL_MS, on_timer);
}

static void stop_monitoring(void) {
    KillTimer(g_main, AUTOAPP_TIMER_ID);
    if (applied) {
        applied = 0;
        do_apply(AUTOAPP_SPEED_NORMAL);  /* undo what Auto mode set */
    }
    was_running = 0;
    cooling = 0;
}

static void apply_new_settings(int enable, wchar_t apps[][MAX_PATH], int count, int speed) {
    int was_enabled = cfg.enabled;

    if (was_enabled) {
        stop_monitoring();
    }
    cfg.enabled = enable;
    cfg.count = 0;
    for (int i = 0; i < count && i < MAX_APPS; ++i) {
        wcsncpy(cfg.apps[i], apps[i], MAX_PATH - 1);
        cfg.apps[i][MAX_PATH - 1] = 0;
        cfg.count++;
    }
    cfg.speed = speed;
    save_settings();

    if (cfg.enabled) {
        start_monitoring();
        poll();  /* react right away if one of the apps is already running */
    }
}

/* ---------- Dialog ---------- */

static HWND make_ctrl(HWND parent, const wchar_t *cls, const wchar_t *text, DWORD style,
                      int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h,
                             parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return c;
}

static void fill_process_list(HWND combo) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return;
    }
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (SendMessageW(combo, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)pe.szExeFile) == CB_ERR) {
                SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)pe.szExeFile);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

/* Add a name to the list box (normalized, no duplicates, max MAX_APPS). */
static void add_app_to_list(HWND dlg, const wchar_t *name) {
    wchar_t app[MAX_PATH];
    wcsncpy(app, name, MAX_PATH - 1);
    app[MAX_PATH - 1] = 0;
    normalize_app(app);
    if (app[0] == 0) {
        return;
    }
    if (SendDlgItemMessageW(dlg, IDC_LIST, LB_GETCOUNT, 0, 0) >= MAX_APPS) {
        return;
    }
    if (SendDlgItemMessageW(dlg, IDC_LIST, LB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)app) == LB_ERR) {
        SendDlgItemMessageW(dlg, IDC_LIST, LB_ADDSTRING, 0, (LPARAM)app);
    }
}

static void add_typed_app(HWND dlg) {
    wchar_t app[MAX_PATH];
    GetDlgItemTextW(dlg, IDC_APP, app, MAX_PATH);
    add_app_to_list(dlg, app);
    SetDlgItemTextW(dlg, IDC_APP, L"");
}

static void browse_for_exe(HWND dlg) {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = dlg;
    ofn.lpstrFilter = L"Programs (*.exe)\0*.exe\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&ofn)) {
        add_app_to_list(dlg, file);
    }
}

static LRESULT CALLBACK DlgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            HWND combo;
            make_ctrl(h, L"STATIC", S->pick_app, 0, 12, 12, 356, 16, -1);
            combo = make_ctrl(h, L"COMBOBOX", L"",
                              WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL | CBS_SORT,
                              12, 32, 190, 220, IDC_APP);
            make_ctrl(h, L"BUTTON", S->add, WS_TABSTOP | BS_PUSHBUTTON, 208, 31, 70, 24, IDC_ADD);
            make_ctrl(h, L"BUTTON", S->browse, WS_TABSTOP | BS_PUSHBUTTON, 282, 31, 86, 24, IDC_BROWSE);
            make_ctrl(h, L"LISTBOX", L"", WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
                      12, 64, 266, 80, IDC_LIST);
            make_ctrl(h, L"BUTTON", S->remove, WS_TABSTOP | BS_PUSHBUTTON, 282, 64, 86, 24, IDC_REMOVE);
            make_ctrl(h, L"STATIC", S->pick_speed, 0, 12, 154, 356, 16, -1);
            make_ctrl(h, L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
                      12, 174, 356, 120, IDC_SPEED);
            make_ctrl(h, L"STATIC", S->hint, 0, 12, 206, 356, 16, IDC_HINT);
            make_ctrl(h, L"BUTTON", S->enable, WS_TABSTOP | BS_AUTOCHECKBOX, 12, 228, 356, 20, IDC_ENABLE);
            make_ctrl(h, L"BUTTON", S->ok, WS_TABSTOP | BS_DEFPUSHBUTTON, 196, 258, 84, 26, IDOK);
            make_ctrl(h, L"BUTTON", S->cancel, WS_TABSTOP | BS_PUSHBUTTON, 284, 258, 84, 26, IDCANCEL);

            fill_process_list(combo);
            for (int i = 0; i < cfg.count; ++i) {
                SendDlgItemMessageW(h, IDC_LIST, LB_ADDSTRING, 0, (LPARAM)cfg.apps[i]);
            }

            SendDlgItemMessageW(h, IDC_SPEED, CB_ADDSTRING, 0, (LPARAM)S->speed_high);
            SendDlgItemMessageW(h, IDC_SPEED, CB_ADDSTRING, 0, (LPARAM)S->speed_low);
            SendDlgItemMessageW(h, IDC_SPEED, CB_ADDSTRING, 0, (LPARAM)S->speed_normal);
            SendDlgItemMessageW(h, IDC_SPEED, CB_SETCURSEL, (WPARAM)cfg.speed, 0);

            CheckDlgButton(h, IDC_ENABLE,
                           (cfg.enabled || cfg.count == 0) ? BST_CHECKED : BST_UNCHECKED);
            return 0;
        }

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_ADD:
                    add_typed_app(h);
                    return 0;

                case IDC_BROWSE:
                    browse_for_exe(h);
                    return 0;

                case IDC_REMOVE: {
                    int sel = (int)SendDlgItemMessageW(h, IDC_LIST, LB_GETCURSEL, 0, 0);
                    if (sel != LB_ERR) {
                        SendDlgItemMessageW(h, IDC_LIST, LB_DELETESTRING, (WPARAM)sel, 0);
                    }
                    return 0;
                }

                case IDOK: {
                    wchar_t apps[MAX_APPS][MAX_PATH];
                    add_typed_app(h);  /* include a name that was typed but not added yet */
                    int count = (int)SendDlgItemMessageW(h, IDC_LIST, LB_GETCOUNT, 0, 0);
                    if (count < 0) count = 0;
                    if (count > MAX_APPS) count = MAX_APPS;
                    for (int i = 0; i < count; ++i) {
                        apps[i][0] = 0;
                        SendDlgItemMessageW(h, IDC_LIST, LB_GETTEXT, (WPARAM)i, (LPARAM)apps[i]);
                    }
                    int enable = (IsDlgButtonChecked(h, IDC_ENABLE) == BST_CHECKED);
                    if (enable && count == 0) {
                        MessageBoxW(h, S->need_app, S->title, MB_OK | MB_ICONINFORMATION);
                        return 0;
                    }
                    int sel = (int)SendDlgItemMessageW(h, IDC_SPEED, CB_GETCURSEL, 0, 0);
                    if (sel < 0 || sel > 2) sel = AUTOAPP_SPEED_HIGH;
                    apply_new_settings(enable, apps, count, sel);
                    DestroyWindow(h);
                    return 0;
                }

                case IDCANCEL:
                    DestroyWindow(h);
                    return 0;
            }
            break;

        case WM_CLOSE:
            DestroyWindow(h);
            return 0;

        case WM_DESTROY:
            g_dlg = NULL;
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ---------- Public API ---------- */

void autoapp_init(HWND hwnd, autoapp_apply_fn apply) {
    g_main = hwnd;
    g_apply = apply;
    pick_strings();
    build_ini_path();
    load_settings();
    if (cfg.enabled && cfg.count > 0) {
        start_monitoring();
    } else {
        cfg.enabled = 0;
    }
}

void autoapp_show_dialog(HWND owner) {
    static int registered = 0;

    if (g_dlg) {
        SetForegroundWindow(g_dlg);
        return;
    }
    pick_strings();

    if (!registered) {
        WNDCLASSEXW wc;
        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DlgProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"LenovoFanAutoDlgClass";
        if (!RegisterClassExW(&wc)) {
            return;
        }
        registered = 1;
    }

    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    RECT r = {0, 0, 380, 296};
    AdjustWindowRectEx(&r, style, FALSE, 0);
    int w = r.right - r.left;
    int h = r.bottom - r.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    g_dlg = CreateWindowExW(0, L"LenovoFanAutoDlgClass", S->title, style, x, y, w, h,
                            owner, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_dlg) {
        return;
    }
    ShowWindow(g_dlg, SW_SHOW);
    SetForegroundWindow(g_dlg);

    /* Local message loop so Tab/Enter/Esc work. Other windows (tray, hotkeys,
     * timers) keep being served because messages are still dispatched. */
    MSG m;
    while (g_dlg) {
        BOOL got = GetMessageW(&m, NULL, 0, 0);
        if (got == 0) {            /* WM_QUIT: pass it on to the main loop */
            PostQuitMessage((int)m.wParam);
            break;
        }
        if (got < 0) {
            break;
        }
        if (!IsDialogMessageW(g_dlg, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    if (g_dlg) {
        DestroyWindow(g_dlg);
        g_dlg = NULL;
    }
}

void autoapp_manual_override(void) {
    /* The user took control: don't force Normal when the app closes. */
    applied = 0;
}

int autoapp_is_enabled(void) {
    return cfg.enabled;
}

const wchar_t* autoapp_menu_text(void) {
    pick_strings();
    return S->menu;
}
