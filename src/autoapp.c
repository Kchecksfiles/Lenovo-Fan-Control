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
#include "fanctrl.h"

#define AUTOAPP_TIMER_ID 7001
#define POLL_INTERVAL_MS 2000   /* how often the process list is checked */
#define COOLDOWN_MS      60000  /* keep the speed for this long after the last app closes */

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
    L"Fan speed for the new or selected app:",
    L"High Speed",
    L"Low Speed",
    L"Normal Speed",
    L"Returns to Normal 1 minute after the last app closes.",
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
    L"新添加或选中应用的风扇转速：",
    L"高转速",
    L"低转速",
    L"正常转速",
    L"最后一个应用关闭 1 分钟后恢复正常转速。",
    L"启用自动模式",
    L"确定",
    L"取消",
    L"请至少添加一个应用程序。"
};

static const Strings *S = &str_en;

int autoapp_applying = 0;

typedef struct {
    wchar_t name[MAX_PATH];
    int speed;  /* AUTOAPP_SPEED_* used while this app is running */
} AppEntry;

static struct {
    int enabled;
    int count;
    AppEntry apps[MAX_APPS];
} cfg;

static HWND g_main = NULL;
static HWND g_dlg = NULL;
static autoapp_apply_fn g_apply = NULL;

/* Runtime state */
static int last_target = -1; /* speed currently triggered by running apps (-1 = none) */
static int cooling = 0;      /* all apps closed; waiting out the cool-down */
static int applied = 0;      /* Auto mode (not the user) set the current speed */
static DWORD cool_start = 0;

/* Dialog working copy of the app list */
static AppEntry dlg_items[MAX_APPS];
static int dlg_count = 0;

static wchar_t ini_path[MAX_PATH];

static void pick_strings(void) {
    S = (PRIMARYLANGID(GetUserDefaultLangID()) == LANG_CHINESE) ? &str_zh : &str_en;
}

static const wchar_t* speed_name(int speed) {
    switch (speed) {
        case AUTOAPP_SPEED_LOW:    return S->speed_low;
        case AUTOAPP_SPEED_NORMAL: return S->speed_normal;
        default:                   return S->speed_high;
    }
}

/* If several apps run at once, the one needing the most cooling wins. */
static int speed_rank(int speed) {
    switch (speed) {
        case AUTOAPP_SPEED_HIGH:   return 2;
        case AUTOAPP_SPEED_NORMAL: return 1;
        default:                   return 0;
    }
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

/* Builds "App0".."App15" / "Speed0".."Speed15" (swprintf differs on MinGW, so it is avoided). */
static void make_key(wchar_t *buf, const wchar_t *prefix, int i) {
    size_t n = wcslen(prefix);
    wcscpy(buf, prefix);
    if (i < 10) {
        buf[n] = (wchar_t)(L'0' + i);
        buf[n + 1] = 0;
    } else {
        buf[n] = (wchar_t)(L'0' + i / 10);
        buf[n + 1] = (wchar_t)(L'0' + i % 10);
        buf[n + 2] = 0;
    }
}

static int clamp_speed(int speed) {
    return (speed < 0 || speed > 2) ? AUTOAPP_SPEED_HIGH : speed;
}

static void load_settings(void) {
    wchar_t key[16];
    cfg.enabled = GetPrivateProfileIntW(L"Auto", L"Enabled", 0, ini_path) ? 1 : 0;
    cfg.count = 0;

    /* Older files had one shared "Speed"; it becomes the default for each app. */
    int def_speed = clamp_speed((int)GetPrivateProfileIntW(L"Auto", L"Speed", AUTOAPP_SPEED_HIGH, ini_path));

    int n = (int)GetPrivateProfileIntW(L"Auto", L"Count", -1, ini_path);
    if (n < 0) {
        /* Oldest format: a single "App" */
        GetPrivateProfileStringW(L"Auto", L"App", L"", cfg.apps[0].name, MAX_PATH, ini_path);
        if (cfg.apps[0].name[0] != 0) {
            cfg.apps[0].speed = def_speed;
            cfg.count = 1;
        }
        return;
    }
    if (n > MAX_APPS) n = MAX_APPS;
    for (int i = 0; i < n; ++i) {
        make_key(key, L"App", i);
        GetPrivateProfileStringW(L"Auto", key, L"", cfg.apps[cfg.count].name, MAX_PATH, ini_path);
        if (cfg.apps[cfg.count].name[0] == 0) {
            continue;
        }
        make_key(key, L"Speed", i);
        cfg.apps[cfg.count].speed =
            clamp_speed((int)GetPrivateProfileIntW(L"Auto", key, def_speed, ini_path));
        cfg.count++;
    }
}

static void save_settings(void) {
    wchar_t key[16];
    wchar_t num[4];
    WritePrivateProfileStringW(L"Auto", L"Enabled", cfg.enabled ? L"1" : L"0", ini_path);

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
        make_key(key, L"App", i);
        WritePrivateProfileStringW(L"Auto", key, i < cfg.count ? cfg.apps[i].name : NULL, ini_path);
        make_key(key, L"Speed", i);
        if (i < cfg.count) {
            num[0] = (wchar_t)(L'0' + cfg.apps[i].speed);
            num[1] = 0;
            WritePrivateProfileStringW(L"Auto", key, num, ini_path);
        } else {
            WritePrivateProfileStringW(L"Auto", key, NULL, ini_path);
        }
    }
    /* Drop the old single-app / shared-speed keys */
    WritePrivateProfileStringW(L"Auto", L"App", NULL, ini_path);
    WritePrivateProfileStringW(L"Auto", L"Speed", NULL, ini_path);
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

/* Returns the fan speed to use for the apps currently running, or -1 if none is running.
 * One process snapshot per check. */
static int find_target_speed(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return -1;
    }
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    int best = -1;
    BOOL ok = Process32FirstW(snap, &pe);
    while (ok) {
        for (int i = 0; i < cfg.count; ++i) {
            if (_wcsicmp(pe.szExeFile, cfg.apps[i].name) == 0) {
                if (best < 0 || speed_rank(cfg.apps[i].speed) > speed_rank(best)) {
                    best = cfg.apps[i].speed;
                }
            }
        }
        ok = Process32NextW(snap, &pe);
    }
    CloseHandle(snap);
    return best;
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
    int target = find_target_speed();
    if (target >= 0) {
        cooling = 0;
        if (target != last_target) {
            /* An app started, or the set of running apps now needs another speed. */
            fan_log("auto: listed app running, applying speed %d", target);
            last_target = target;
            applied = 1;
            do_apply(target);
        }
    } else if (last_target >= 0) {
        if (!cooling) {
            fan_log("auto: no listed app running, holding speed for 60 s");
            cooling = 1;
            cool_start = GetTickCount();
        } else if (GetTickCount() - cool_start >= COOLDOWN_MS) {
            fan_log("auto: cool-down over, back to Normal (auto set it: %d)", applied);
            last_target = -1;
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
    last_target = -1;
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
    last_target = -1;
    cooling = 0;
}

static void apply_new_settings(int enable, const AppEntry *items, int count) {
    int was_enabled = cfg.enabled;

    if (was_enabled) {
        stop_monitoring();
    }
    cfg.enabled = enable;
    cfg.count = 0;
    for (int i = 0; i < count && i < MAX_APPS; ++i) {
        cfg.apps[i] = items[i];
        cfg.count++;
    }
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

/* Rebuild the list box from dlg_items ("game.exe   -   High Speed"). */
static void refresh_list(HWND dlg, int select) {
    wchar_t text[MAX_PATH + 64];
    SendDlgItemMessageW(dlg, IDC_LIST, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < dlg_count; ++i) {
        wcscpy(text, dlg_items[i].name);
        wcscat(text, L"   -   ");
        wcscat(text, speed_name(dlg_items[i].speed));
        SendDlgItemMessageW(dlg, IDC_LIST, LB_ADDSTRING, 0, (LPARAM)text);
    }
    if (select >= 0 && select < dlg_count) {
        SendDlgItemMessageW(dlg, IDC_LIST, LB_SETCURSEL, (WPARAM)select, 0);
    }
}

static int combo_speed(HWND dlg) {
    int sel = (int)SendDlgItemMessageW(dlg, IDC_SPEED, CB_GETCURSEL, 0, 0);
    return clamp_speed(sel);
}

/* Add an app (normalized) with the speed currently chosen in the speed box.
 * If it is already in the list, only its speed is updated. */
static void add_app_to_list(HWND dlg, const wchar_t *name) {
    wchar_t app[MAX_PATH];
    wcsncpy(app, name, MAX_PATH - 1);
    app[MAX_PATH - 1] = 0;
    normalize_app(app);
    if (app[0] == 0) {
        return;
    }
    int speed = combo_speed(dlg);
    for (int i = 0; i < dlg_count; ++i) {
        if (_wcsicmp(dlg_items[i].name, app) == 0) {
            dlg_items[i].speed = speed;
            refresh_list(dlg, -1);
            return;
        }
    }
    if (dlg_count >= MAX_APPS) {
        return;
    }
    wcscpy(dlg_items[dlg_count].name, app);
    dlg_items[dlg_count].speed = speed;
    dlg_count++;
    refresh_list(dlg, -1);
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
            make_ctrl(h, L"LISTBOX", L"",
                      WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
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

            /* Speed options: the order matches the AUTOAPP_SPEED_* values. */
            SendDlgItemMessageW(h, IDC_SPEED, CB_ADDSTRING, 0, (LPARAM)S->speed_high);
            SendDlgItemMessageW(h, IDC_SPEED, CB_ADDSTRING, 0, (LPARAM)S->speed_low);
            SendDlgItemMessageW(h, IDC_SPEED, CB_ADDSTRING, 0, (LPARAM)S->speed_normal);
            SendDlgItemMessageW(h, IDC_SPEED, CB_SETCURSEL, (WPARAM)AUTOAPP_SPEED_HIGH, 0);

            dlg_count = cfg.count;
            for (int i = 0; i < cfg.count; ++i) {
                dlg_items[i] = cfg.apps[i];
            }
            refresh_list(h, -1);

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
                    if (sel != LB_ERR && sel < dlg_count) {
                        for (int i = sel; i < dlg_count - 1; ++i) {
                            dlg_items[i] = dlg_items[i + 1];
                        }
                        dlg_count--;
                        refresh_list(h, -1);
                    }
                    return 0;
                }

                case IDC_LIST:
                    if (HIWORD(wp) == LBN_SELCHANGE) {
                        /* Show the selected app's speed in the speed box. */
                        int sel = (int)SendDlgItemMessageW(h, IDC_LIST, LB_GETCURSEL, 0, 0);
                        if (sel != LB_ERR && sel < dlg_count) {
                            SendDlgItemMessageW(h, IDC_SPEED, CB_SETCURSEL,
                                                (WPARAM)dlg_items[sel].speed, 0);
                        }
                    }
                    return 0;

                case IDC_SPEED:
                    if (HIWORD(wp) == CBN_SELCHANGE) {
                        /* With an app selected in the list, change that app's speed. */
                        int sel = (int)SendDlgItemMessageW(h, IDC_LIST, LB_GETCURSEL, 0, 0);
                        if (sel != LB_ERR && sel < dlg_count) {
                            dlg_items[sel].speed = combo_speed(h);
                            refresh_list(h, sel);
                        }
                    }
                    return 0;

                case IDOK: {
                    add_typed_app(h);  /* include a name that was typed but not added yet */
                    int enable = (IsDlgButtonChecked(h, IDC_ENABLE) == BST_CHECKED);
                    if (enable && dlg_count == 0) {
                        MessageBoxW(h, S->need_app, S->title, MB_OK | MB_ICONINFORMATION);
                        return 0;
                    }
                    apply_new_settings(enable, dlg_items, dlg_count);
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
    /* The user took control: don't force Normal when the apps close. */
    applied = 0;
}

int autoapp_is_enabled(void) {
    return cfg.enabled;
}

const wchar_t* autoapp_menu_text(void) {
    pick_strings();
    return S->menu;
}
