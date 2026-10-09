// main.cpp — інтерфейс програми "Монітор стану дисків"
//
// Головне вікно показує три таблиці:
//   1) фізичні накопичувачі (тип, температура, напрацювання, висновок про стан);
//   2) розділи (скільки місця зайнято);
//   3) активність дисків у реальному часі (завантаженість, швидкість читання/запису).
// Дані збираються у фоновому потоці, щоб інтерфейс не "підвисав" під час запитів WMI.
// Програма може працювати згорнутою в трей і попереджати про проблеми з дисками.
// Раз на добу вона перевіряє, чи вийшла нова версія, і пропонує оновитися (див. Updater.cpp).

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX      // щоб макроси min/max з windows.h не заважали std::min/std::max
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <objbase.h>

#include <string>
#include <vector>
#include <map>
#include <set>
#include <thread>
#include <atomic>
#include <memory>
#include <ctime>
#include <algorithm>

#include "DiskInfo.h"
#include "Updater.h"
#include "resource.h"

#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#endif

// ---------------------------------------------------------------------------
// Налаштування
// ---------------------------------------------------------------------------

static const wchar_t* APP_TITLE  = L"Монітор стану дисків";
static const wchar_t* APP_AUTHOR = L"Кирилюк Микола, група 1-КТ-23";

static const wchar_t* kMutexName   = L"DiskMonitorKursova_Mutex";   // та сама назва вказана в setup.iss (AppMutex)
static const wchar_t* kMainClass   = L"DiskMonitorKursova_Main";
static const wchar_t* kDetailsClass = L"DiskMonitorKursova_Details";

static const DWORD LOAD_INTERVAL_MS = 2000;     // як часто оновлювати активність дисків
static const DWORD FULL_SCAN_MS     = 60000;    // як часто повністю перечитувати стан дисків

static const wchar_t* kSettingsKey = L"Software\\DiskMonitorKursova";   // налаштування в реєстрі (HKCU)
static const UINT UPDATE_FIRST_MS    = 15 * 1000;             // перша перевірка оновлень — через 15 с після запуску
static const UINT UPDATE_INTERVAL_MS = 24 * 60 * 60 * 1000;   // далі — раз на добу
static const UINT_PTR IDT_UPDATE = 1;

enum {
    WM_APP_SNAPSHOT = WM_APP + 1,   // фоновий потік передав новий знімок стану дисків
    WM_APP_LOAD,                    // фоновий потік передав дані про активність
    WM_APP_TRAY,                    // подія від значка в треї
    WM_APP_SHOWME,                  // інша копія програми просить показати вікно
    WM_APP_UPDATE_INFO,             // перевірка оновлень завершилась
    WM_APP_UPDATE_FILE              // завантаження інсталятора нової версії завершилось
};

enum {
    IDC_BTN_REFRESH = 101, IDC_BTN_DETAILS, IDC_BTN_REPORT, IDC_BTN_LOG, IDC_BTN_ADMIN, IDC_BTN_ABOUT,
    IDC_LIST_DISKS = 201, IDC_LIST_VOLS, IDC_LIST_LOAD, IDC_LIST_SMART, IDC_LIST_NVME,
    IDC_STATUS = 301,
    ID_TRAY_OPEN = 401, ID_TRAY_REFRESH, ID_TRAY_EXIT, ID_TRAY_UPDATE_CHECK, ID_TRAY_UPDATE_AUTO,
    IDB_UPDATE_NOW = 501, IDB_UPDATE_PAGE, IDB_ABOUT_UPDATE   // кнопки у вікнах TaskDialog
};

// Номери колонок, які малюються особливо
static const int COL_DISK_STATE = 9;
static const int COL_VOL_BAR    = 6;
static const int COL_LOAD_BAR   = 1;
static const int COL_SMART_STATE = 6;
static const int COL_NVME_STATE = 2;

// ---------------------------------------------------------------------------
// Глобальний стан програми
// ---------------------------------------------------------------------------

struct AppState {
    HINSTANCE inst = nullptr;
    HWND main = nullptr, status = nullptr, details = nullptr;
    HWND lvDisks = nullptr, lvVols = nullptr, lvLoad = nullptr;
    HWND lblDisks = nullptr, lblVols = nullptr, lblLoad = nullptr;
    std::vector<HWND> buttons;
    HWND btnAdmin = nullptr;
    HFONT font = nullptr, fontBold = nullptr, fontMono = nullptr;
    HICON icon = nullptr, iconSmall = nullptr;
    HANDLE mutex = nullptr;
    int dpi = 96;
    UINT msgTaskbarCreated = 0;

    Snapshot snap;
    bool haveSnap = false;
    bool isAdmin = false;
    bool reallyExit = false;
    bool trayHintShown = false;

    std::map<std::wstring, Health> lastVerdict;   // для відстеження змін стану дисків
    std::set<std::wstring> lowSpaceAlerted;       // розділи, про які вже попередили

    // Оновлення програми
    bool autoUpdate = true;          // перевіряти оновлення раз на добу
    bool updateBusy = false;         // зараз іде перевірка або завантаження
    bool updateWantManual = false;   // користувач попросив перевірку, поки йшла автоматична
    bool updateWantInstall = false;  // користувач натиснув «Оновити зараз», поки йшла перевірка
    bool updateDialogOpen = false;
    bool balloonIsUpdate = false;    // останнє сповіщення в треї — про нову версію
    bool hasUpdate = false;          // знайдено новішу версію
    UpdateInfo update;
    std::wstring updateLogged;       // про яку версію вже записано в журнал
} g;

static HANDLE g_evStop = nullptr;      // зупинити фоновий потік
static HANDLE g_evRefresh = nullptr;   // зробити повне оновлення зараз
static HANDLE g_evWake = nullptr;      // просто "розбудити" потік (вікно знову показали)
static std::atomic<bool> g_visible{ false };
static std::thread g_worker;

static int S(int v) { return MulDiv(v, g.dpi, 96); }   // масштабування під DPI екрана

// ---------------------------------------------------------------------------
// Журнал подій (%LOCALAPPDATA%\DiskMonitor\events.log)
// ---------------------------------------------------------------------------

static std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring NowText(bool withDate) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t buf[64];
    if (withDate)
        swprintf(buf, 64, L"%04d-%02d-%02d %02d:%02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    else
        swprintf(buf, 64, L"%02d:%02d:%02d", t.wHour, t.wMinute, t.wSecond);
    return buf;
}

static std::wstring LogPath() {
    wchar_t dir[MAX_PATH] = L"";
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) return L"";
    std::wstring d = std::wstring(dir) + L"\\DiskMonitor";
    CreateDirectoryW(d.c_str(), nullptr);
    return d + L"\\events.log";
}

static void LogEvent(const std::wstring& text) {
    std::wstring path = LogPath();
    if (path.empty()) return;
    HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(f, &size) && size.QuadPart == 0)
        WriteFile(f, "\xEF\xBB\xBF", 3, &written, nullptr);   // BOM, щоб Блокнот правильно показав кирилицю
    std::string line = ToUtf8(L"[" + NowText(true) + L"] " + text + L"\r\n");
    WriteFile(f, line.data(), (DWORD)line.size(), &written, nullptr);
    CloseHandle(f);
}

// ---------------------------------------------------------------------------
// Фоновий потік: збирає дані через WMI і передає їх у головне вікно
// ---------------------------------------------------------------------------

static void WorkerThread(HWND target) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        WmiReader wmi;
        wmi.Connect();

        HANDLE events[3] = { g_evStop, g_evRefresh, g_evWake };
        bool needFull = true;
        DWORD lastFull = 0;

        for (;;) {
            if (needFull || GetTickCount() - lastFull >= FULL_SCAN_MS) {
                auto* snap = new Snapshot(wmi.ReadAll());
                if (!PostMessageW(target, WM_APP_SNAPSHOT, 0, (LPARAM)snap)) delete snap;
                lastFull = GetTickCount();
                needFull = false;
            }

            DWORD wait;
            if (g_visible) {
                // Вікно відкрите — показуємо активність у реальному часі
                auto* load = new std::vector<DiskLoad>(wmi.ReadLoad());
                if (!PostMessageW(target, WM_APP_LOAD, 0, (LPARAM)load)) delete load;
                wait = LOAD_INTERVAL_MS;
            } else {
                // Програма в треї — лише періодична перевірка стану, щоб не навантажувати систему
                DWORD passed = GetTickCount() - lastFull;
                wait = passed < FULL_SCAN_MS ? FULL_SCAN_MS - passed : 0;
            }

            DWORD r = WaitForMultipleObjects(3, events, FALSE, wait);
            if (r == WAIT_OBJECT_0) break;
            if (r == WAIT_OBJECT_0 + 1) needFull = true;
        }
    }
    CoUninitialize();
}

// ---------------------------------------------------------------------------
// Допоміжні функції для ListView
// ---------------------------------------------------------------------------

static HWND CreateList(HWND parent, int id, int rowHeight) {
    HWND lv = CreateWindowExW(0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, g.inst, nullptr);
    SetWindowTheme(lv, L"Explorer", nullptr);
    ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    SendMessageW(lv, WM_SETFONT, (WPARAM)g.font, TRUE);
    // Невеликий трюк: список зображень задає висоту рядка, так таблиця читається легше
    HIMAGELIST il = ImageList_Create(1, rowHeight, ILC_COLOR32, 1, 0);
    ListView_SetImageList(lv, il, LVSIL_SMALL);
    return lv;
}

static void AddColumn(HWND lv, int index, const wchar_t* title, int width, int fmt = LVCFMT_LEFT) {
    LVCOLUMNW c{};
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    c.fmt = fmt;
    c.cx = S(width);
    c.pszText = (LPWSTR)title;
    ListView_InsertColumn(lv, index, &c);
}

// Оновлює рядок таблиці або додає новий (так зберігається виділення і прокрутка)
static void SetRow(HWND lv, int row, const std::vector<std::wstring>& cells, LPARAM param) {
    LVITEMW it{};
    it.mask = LVIF_TEXT | LVIF_PARAM;
    it.iItem = row;
    it.pszText = (LPWSTR)cells[0].c_str();
    it.lParam = param;
    if (row < ListView_GetItemCount(lv)) ListView_SetItem(lv, &it);
    else ListView_InsertItem(lv, &it);
    for (size_t i = 1; i < cells.size(); ++i)
        ListView_SetItemText(lv, row, (int)i, (LPWSTR)cells[i].c_str());
}

static void TrimRows(HWND lv, int count) {
    while (ListView_GetItemCount(lv) > count) ListView_DeleteItem(lv, ListView_GetItemCount(lv) - 1);
}

static void StateColors(Health h, COLORREF& bk, COLORREF& text) {
    switch (h) {
    case Health::Good:     bk = RGB(220, 242, 220); text = RGB(25, 100, 35);  break;
    case Health::Warning:  bk = RGB(255, 238, 186); text = RGB(125, 80, 0);   break;
    case Health::Critical: bk = RGB(255, 208, 208); text = RGB(165, 0, 0);    break;
    default:               bk = RGB(235, 235, 235); text = RGB(90, 90, 90);   break;
    }
}

// Смуга-індикатор у клітинці таблиці (заповненість розділу, завантаженість диска)
static void DrawBar(HDC dc, RECT rc, int pct, bool isLoad) {
    FillRect(dc, &rc, GetSysColorBrush(COLOR_WINDOW));
    if (pct < 0) return;                 // немає даних — клітинка порожня
    RECT r = rc;
    InflateRect(&r, -S(6), -S(4));
    if (r.right - r.left < 10) return;

    HBRUSH frame = CreateSolidBrush(RGB(190, 190, 190));
    FrameRect(dc, &r, frame);
    DeleteObject(frame);

    RECT fill = r;
    InflateRect(&fill, -1, -1);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    fill.right = fill.left + (fill.right - fill.left) * pct / 100;
    COLORREF c = isLoad ? RGB(110, 160, 225)
               : pct >= 90 ? RGB(230, 95, 85)
               : pct >= 75 ? RGB(245, 180, 70)
               : RGB(95, 180, 115);
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &fill, b);
    DeleteObject(b);

    wchar_t txt[16];
    swprintf(txt, 16, L"%d%%", pct);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(30, 30, 30));
    DrawTextW(dc, txt, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// Спільна обробка NM_CUSTOMDRAW: кольорова клітинка стану та смуги-індикатори
static LRESULT OnCustomDraw(NMLVCUSTOMDRAW* cd) {
    static COLORREF defText, defBk;
    HWND lv = cd->nmcd.hdr.hwndFrom;
    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT:
        defText = cd->clrText;
        defBk = cd->clrTextBk;
        return CDRF_NOTIFYSUBITEMDRAW;
    case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
        int item = (int)cd->nmcd.dwItemSpec;
        int sub = cd->iSubItem;
        int ctrl = GetDlgCtrlID(lv);

        bool bar = (ctrl == IDC_LIST_VOLS && sub == COL_VOL_BAR) || (ctrl == IDC_LIST_LOAD && sub == COL_LOAD_BAR);
        if (bar) {
            RECT rc{};
            ListView_GetSubItemRect(lv, item, sub, LVIR_BOUNDS, &rc);
            DrawBar(cd->nmcd.hdc, rc, (int)cd->nmcd.lItemlParam, ctrl == IDC_LIST_LOAD);
            return CDRF_SKIPDEFAULT;
        }
        bool state = (ctrl == IDC_LIST_DISKS && sub == COL_DISK_STATE) || (ctrl == IDC_LIST_SMART && sub == COL_SMART_STATE) ||
                     (ctrl == IDC_LIST_NVME && sub == COL_NVME_STATE);
        if (state) {
            StateColors((Health)cd->nmcd.lItemlParam, cd->clrTextBk, cd->clrText);
            SelectObject(cd->nmcd.hdc, g.fontBold);
            return CDRF_NEWFONT;
        }
        cd->clrText = defText;
        cd->clrTextBk = defBk;
        SelectObject(cd->nmcd.hdc, g.font);
        return CDRF_NEWFONT;
    }
    }
    return CDRF_DODEFAULT;
}

// ---------------------------------------------------------------------------
// Трей
// ---------------------------------------------------------------------------

static void TrayAdd() {
    NOTIFYICONDATAW n{};
    n.cbSize = sizeof(n);
    n.hWnd = g.main;
    n.uID = 1;
    n.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    n.uCallbackMessage = WM_APP_TRAY;
    n.hIcon = g.iconSmall;
    lstrcpynW(n.szTip, APP_TITLE, ARRAYSIZE(n.szTip));
    Shell_NotifyIconW(NIM_ADD, &n);
}

static void TrayRemove() {
    NOTIFYICONDATAW n{};
    n.cbSize = sizeof(n);
    n.hWnd = g.main;
    n.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &n);
}

static void TrayTip(const std::wstring& tip) {
    NOTIFYICONDATAW n{};
    n.cbSize = sizeof(n);
    n.hWnd = g.main;
    n.uID = 1;
    n.uFlags = NIF_TIP;
    lstrcpynW(n.szTip, tip.c_str(), ARRAYSIZE(n.szTip));
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

static void TrayBalloon(const std::wstring& title, const std::wstring& text, DWORD icon) {
    g.balloonIsUpdate = false;
    NOTIFYICONDATAW n{};
    n.cbSize = sizeof(n);
    n.hWnd = g.main;
    n.uID = 1;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = icon;
    lstrcpynW(n.szInfoTitle, title.c_str(), ARRAYSIZE(n.szInfoTitle));
    lstrcpynW(n.szInfo, text.c_str(), ARRAYSIZE(n.szInfo));
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

static void ShowMain() {
    ShowWindow(g.main, SW_SHOW);
    if (IsIconic(g.main)) ShowWindow(g.main, SW_RESTORE);
    SetForegroundWindow(g.main);
    g_visible = true;
    SetEvent(g_evWake);
}

static void HideToTray() {
    ShowWindow(g.main, SW_HIDE);
    g_visible = false;
    if (!g.trayHintShown) {
        g.trayHintShown = true;
        TrayBalloon(APP_TITLE,
            L"Програма продовжує стежити за дисками у фоні. Щоб закрити її повністю, "
            L"клацніть правою кнопкою миші на цьому значку і виберіть «Вихід».", NIIF_INFO);
    }
}

// ---------------------------------------------------------------------------
// Заповнення таблиць
// ---------------------------------------------------------------------------

static std::wstring DiskTitle(int index) {
    for (auto& d : g.snap.disks)
        if (d.index == index) return L"Диск " + std::to_wstring(index) + L" — " + d.model;
    return L"Диск " + std::to_wstring(index);
}

static void FillDisks() {
    const auto& disks = g.snap.disks;
    for (size_t i = 0; i < disks.size(); ++i) {
        const PhysicalDisk& d = disks[i];
        std::wstring temp = d.temperature >= 0 ? std::to_wstring(d.temperature) + L" °C" : L"—";
        std::wstring hours = d.powerOnHours >= 0 ? FormatNumber(d.powerOnHours) + L" год" : L"—";
        // «Здоров’я»: для SSD — залишок ресурсу, для HDD — індекс механічного стану (оцінка програми).
        // Прочерк — якщо даних немає (без прав адміністратора, USB-накопичувачі, SSD без даних про знос).
        std::wstring life = L"—";
        if (d.isSsd && d.wear >= 0)  life = std::to_wstring(std::max(0, 100 - d.wear)) + L"%";
        else if (d.hddHealth >= 0)   life = std::to_wstring(d.hddHealth) + L"%";
        SetRow(g.lvDisks, (int)i, {
            std::to_wstring(d.index), d.model, d.type, d.bus.empty() ? L"—" : d.bus,
            FormatSize(d.size), d.letters.empty() ? L"—" : d.letters,
            temp, hours, life, HealthName(d.verdict)
        }, (LPARAM)d.verdict);
    }
    TrimRows(g.lvDisks, (int)disks.size());
    if (ListView_GetSelectedCount(g.lvDisks) == 0 && !disks.empty())
        ListView_SetItemState(g.lvDisks, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
}

static void FillVolumes() {
    const auto& vols = g.snap.volumes;
    for (size_t i = 0; i < vols.size(); ++i) {
        const Volume& v = vols[i];
        int used = v.total ? (int)((v.total - v.free) * 100 / v.total) : 0;
        std::wstring label = v.label.empty() ? (v.removable ? L"Знімний диск" : L"Локальний диск") : v.label;
        SetRow(g.lvVols, (int)i, {
            v.letter, label, v.fs.empty() ? L"—" : v.fs,
            v.disk >= 0 ? std::to_wstring(v.disk) : L"—",
            FormatSize(v.total), FormatSize(v.free), L""
        }, (LPARAM)used);
    }
    TrimRows(g.lvVols, (int)vols.size());
}

static void FillLoad(const std::vector<DiskLoad>& load) {
    if (load.empty()) {
        SetRow(g.lvLoad, 0, { L"Лічильники продуктивності дисків недоступні в цій системі", L"", L"", L"", L"" }, -1);
        TrimRows(g.lvLoad, 1);
        return;
    }
    for (size_t i = 0; i < load.size(); ++i) {
        const DiskLoad& l = load[i];
        // У лічильниках назва має вигляд "0 C: D:" — номер диска і літери розділів
        int index = _wtoi(l.name.c_str());
        size_t sp = l.name.find(L' ');
        std::wstring letters = sp != std::wstring::npos ? l.name.substr(sp + 1) : L"";
        std::wstring title = DiskTitle(index) + (letters.empty() ? L"" : L"  (" + letters + L")");

        wchar_t rd[32], wr[32];
        swprintf(rd, 32, L"%.1f МБ/с", l.readMB);
        swprintf(wr, 32, L"%.1f МБ/с", l.writeMB);
        SetRow(g.lvLoad, (int)i, { title, L"", rd, wr, std::to_wstring(l.queue) }, (LPARAM)l.busy);
    }
    TrimRows(g.lvLoad, (int)load.size());
}

static void UpdateStatus() {
    int good = 0, warn = 0, crit = 0;
    for (auto& d : g.snap.disks) {
        if (d.verdict == Health::Good) ++good;
        if (d.verdict == Health::Warning) ++warn;
        if (d.verdict == Health::Critical) ++crit;
    }

    std::wstring p0 = g.snap.wmiOk ? L"Оновлено о " + NowText(false)
                                   : L"Помилка: не вдалося підключитися до WMI";
    std::wstring p1 = g.isAdmin ? L"Права адміністратора: так — доступні всі дані SMART"
                                : L"Без прав адміністратора — частина даних SMART недоступна";
    std::wstring p2;
    if (crit)      p2 = L"Критичний стан: " + std::to_wstring(crit) + L" диск(и)!";
    else if (warn) p2 = L"Потребують уваги: " + std::to_wstring(warn) + L" диск(и)";
    else if (good) p2 = L"Усі диски в нормі";
    else           p2 = L"Диски не повідомляють свій стан";

    SendMessageW(g.status, SB_SETTEXTW, 0, (LPARAM)p0.c_str());
    SendMessageW(g.status, SB_SETTEXTW, 1, (LPARAM)p1.c_str());
    SendMessageW(g.status, SB_SETTEXTW, 2, (LPARAM)p2.c_str());
    TrayTip(std::wstring(APP_TITLE) + L"\n" + p2);
}

// Перевіряє, чи з'явилися нові проблеми, і повідомляє про них
static void CheckAlerts() {
    for (auto& d : g.snap.disks) {
        std::wstring key = d.serial.empty() ? d.model + L"#" + std::to_wstring(d.index) : d.serial;
        auto it = g.lastVerdict.find(key);
        bool first = (it == g.lastVerdict.end());
        std::wstring name = L"Диск " + std::to_wstring(d.index) + L" (" + d.model + L")";

        if (first)
            LogEvent(name + L": стан — " + HealthName(d.verdict));
        else if (it->second != d.verdict)
            LogEvent(name + L": стан змінився з «" + HealthName(it->second) + L"» на «" + HealthName(d.verdict) + L"»");

        bool bad = d.verdict == Health::Warning || d.verdict == Health::Critical;
        if (bad && (first || it->second != d.verdict)) {
            for (auto& n : d.notes) LogEvent(L"    " + n);
            std::wstring title = d.verdict == Health::Critical ? L"Критичний стан диска" : L"Диск потребує уваги";
            TrayBalloon(title, name + L"\n" + (d.notes.empty() ? L"" : d.notes[0]),
                        d.verdict == Health::Critical ? NIIF_ERROR : NIIF_WARNING);
        }
        g.lastVerdict[key] = d.verdict;
    }

    for (auto& v : g.snap.volumes) {
        if (!v.total || v.removable) continue;
        int freePct = (int)(v.free * 100 / v.total);
        if (freePct < 10 && !g.lowSpaceAlerted.count(v.letter)) {
            g.lowSpaceAlerted.insert(v.letter);
            std::wstring msg = L"На розділі " + v.letter + L" залишилось " + FormatSize(v.free) +
                               L" (" + std::to_wstring(freePct) + L"%).";
            LogEvent(L"Мало вільного місця. " + msg);
            TrayBalloon(L"Мало вільного місця", msg, NIIF_WARNING);
        } else if (freePct >= 15) {
            g.lowSpaceAlerted.erase(v.letter);
        }
    }
}

// ---------------------------------------------------------------------------
// Вікно "Детальна інформація про диск"
// ---------------------------------------------------------------------------

struct DetailsData {
    PhysicalDisk disk;
    HWND edit = nullptr, label = nullptr, list = nullptr;
};

static void FillSmartList(HWND lv, const PhysicalDisk& d) {
    int row = 0;
    for (auto& a : d.smart) {
        Health h = Health::Good;
        std::wstring mark = L"OK";
        if (a.threshold > 0 && a.current > 0 && a.current <= a.threshold) {
            if (a.flags & 0x01) { h = Health::Critical; mark = L"Нижче порогу"; }   // критичний атрибут
            else                { h = Health::Warning;  mark = L"Увага"; }          // атрибут старіння
        } else if ((a.id == 5 || a.id == 197 || a.id == 198) && (a.raw & 0xFFFFFFFF) > 0) {
            h = Health::Warning; mark = L"Увага";
        }
        // Атрибут, з якого взято ресурс SSD: оцінка така сама, як у висновку про знос
        if (a.id == d.wearAttr && d.wear >= 0) {
            Health wh = d.wear >= 90 ? Health::Critical : d.wear >= 70 ? Health::Warning : Health::Good;
            h = std::max(h, wh);
            mark = L"Ресурс " + std::to_wstring(std::max(0, 100 - d.wear)) + L"%";
        }

        wchar_t hex[8];
        swprintf(hex, 8, L"%02X", a.id);
        SetRow(lv, row++, {
            std::wstring(hex) + L" (" + std::to_wstring(a.id) + L")",
            SmartAttrName(d, a.id), std::to_wstring(a.current), std::to_wstring(a.worst),
            a.threshold >= 0 ? std::to_wstring(a.threshold) : L"—", FormatSmartRaw(d, a), mark
        }, (LPARAM)h);
    }
}

// Таблиця для NVMe: журнал стану (SMART / Health Information Log)
static void FillNvmeList(HWND lv, const PhysicalDisk& d) {
    int row = 0;
    auto add = [&](const std::wstring& name, const std::wstring& value, Health h, const wchar_t* mark) {
        SetRow(lv, row++, { name, value, mark }, (LPARAM)h);
    };
    auto info = [&](const std::wstring& name, const std::wstring& value) {
        add(name, value, Health::Unknown, L"інформація");
    };

    int cw = d.nvmeCriticalWarning;
    std::wstring warnText;
    auto addWarn = [&](const wchar_t* t) { if (!warnText.empty()) warnText += L", "; warnText += t; };
    if (cw & 0x01) addWarn(L"мало резерву");
    if (cw & 0x02) addWarn(L"температура");
    if (cw & 0x04) addWarn(L"знижена надійність");
    if (cw & 0x08) addWarn(L"лише читання");
    if (cw & 0x10) addWarn(L"резервне живлення");
    if (cw == 0) add(L"Критичні попередження", L"немає", Health::Good, L"OK");
    else add(L"Критичні попередження", warnText, (cw & 0x0D) ? Health::Critical : Health::Warning,
             (cw & 0x0D) ? L"Критично" : L"Увага");

    if (d.temperature >= 0)
        add(L"Температура", std::to_wstring(d.temperature) + L" °C",
            d.temperature >= 70 ? Health::Warning : Health::Good, d.temperature >= 70 ? L"Увага" : L"OK");

    Health wh = d.wear >= 90 ? Health::Critical : d.wear >= 70 ? Health::Warning : Health::Good;
    add(L"Використано ресурсу (Percentage Used)",
        std::to_wstring(d.wear) + L"%  (залишилось " + std::to_wstring(std::max(0, 100 - d.wear)) + L"%)",
        wh, wh == Health::Critical ? L"Критично" : wh == Health::Warning ? L"Увага" : L"OK");

    bool spareLow = d.nvmeSpareThreshold > 0 && d.nvmeSpare < d.nvmeSpareThreshold;
    add(L"Доступний резерв (Available Spare)",
        std::to_wstring(d.nvmeSpare) + L"%  (поріг " + std::to_wstring(d.nvmeSpareThreshold) + L"%)",
        spareLow ? Health::Critical : Health::Good, spareLow ? L"Критично" : L"OK");

    add(L"Помилки цілісності даних", FormatNumber(d.nvmeMediaErrors),
        d.nvmeMediaErrors > 0 ? Health::Warning : Health::Good, d.nvmeMediaErrors > 0 ? L"Увага" : L"OK");

    info(L"Всього записано", FormatSize(d.nvmeDataWritten));
    info(L"Всього прочитано", FormatSize(d.nvmeDataRead));
    info(L"Години роботи", FormatNumber(d.powerOnHours) + L" год");
    info(L"Кількість увімкнень", FormatNumber(d.powerCycles));
    info(L"Небезпечні вимкнення (без коректного завершення)", FormatNumber(d.nvmeUnsafeShutdowns));
    info(L"Записів у журналі помилок контролера", FormatNumber(d.nvmeErrorLogEntries));
    info(L"Час роботи з підвищеною температурою", FormatNumber(d.nvmeWarnTempMinutes) + L" хв");
    info(L"Час роботи з критичною температурою", FormatNumber(d.nvmeCritTempMinutes) + L" хв");
}

static LRESULT CALLBACK DetailsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* dd = (DetailsData*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: {
        dd = (DetailsData*)((CREATESTRUCTW*)lp)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)dd);

        dd->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            0, 0, 0, 0, hwnd, nullptr, g.inst, nullptr);
        SendMessageW(dd->edit, WM_SETFONT, (WPARAM)g.fontMono, TRUE);
        SetWindowTextW(dd->edit, DiskDescription(dd->disk).c_str());

        const PhysicalDisk& disk = dd->disk;
        std::wstring cap = L"SMART-атрибути (самодіагностика диска)";
        if (disk.hasNvmeLog)
            cap = L"Журнал стану NVMe (аналог SMART для NVMe-накопичувачів)";
        else if (disk.bus == L"NVMe" && !g.isAdmin)
            cap = L"Дані NVMe недоступні: натисніть «Перезапустити як адміністратор» у головному вікні.";
        else if (disk.smart.empty())
            cap = g.isAdmin
                ? L"SMART-атрибути: цей диск не надає їх через WMI (так буває з NVMe та USB). Основні показники — вище."
                : L"SMART-атрибути недоступні: натисніть «Перезапустити як адміністратор» у головному вікні.";
        dd->label = CreateWindowExW(0, L"STATIC", cap.c_str(), WS_CHILD | WS_VISIBLE,
            0, 0, 0, 0, hwnd, nullptr, g.inst, nullptr);
        SendMessageW(dd->label, WM_SETFONT, (WPARAM)g.fontBold, TRUE);

        if (disk.hasNvmeLog) {
            dd->list = CreateList(hwnd, IDC_LIST_NVME, S(22));
            AddColumn(dd->list, 0, L"Показник", 380);
            AddColumn(dd->list, 1, L"Значення", 230);
            AddColumn(dd->list, 2, L"Оцінка", 110, LVCFMT_CENTER);
            FillNvmeList(dd->list, disk);
        } else {
            dd->list = CreateList(hwnd, IDC_LIST_SMART, S(22));
            AddColumn(dd->list, 0, L"ID", 70);
            AddColumn(dd->list, 1, L"Атрибут", 250);
            AddColumn(dd->list, 2, L"Поточне", 70, LVCFMT_RIGHT);
            AddColumn(dd->list, 3, L"Найгірше", 75, LVCFMT_RIGHT);
            AddColumn(dd->list, 4, L"Поріг", 60, LVCFMT_RIGHT);
            AddColumn(dd->list, 5, L"Значення", 110, LVCFMT_RIGHT);
            AddColumn(dd->list, 6, L"Оцінка", 100);
            FillSmartList(dd->list, disk);
        }
        return 0;
    }
    case WM_SIZE: {
        if (!dd) break;
        int w = LOWORD(lp), h = HIWORD(lp), m = S(10);
        int editH = (h - 3 * m - S(22)) * 45 / 100;
        MoveWindow(dd->edit, m, m, w - 2 * m, editH, TRUE);
        MoveWindow(dd->label, m, m + editH + m, w - 2 * m, S(20), TRUE);
        int ly = m + editH + m + S(24);
        MoveWindow(dd->list, m, ly, w - 2 * m, h - ly - m, TRUE);
        return 0;
    }
    case WM_NOTIFY: {
        auto* hdr = (NMHDR*)lp;
        if ((hdr->idFrom == IDC_LIST_SMART || hdr->idFrom == IDC_LIST_NVME) && hdr->code == NM_CUSTOMDRAW)
            return OnCustomDraw((NMLVCUSTOMDRAW*)lp);
        break;
    }
    case WM_CTLCOLORSTATIC:
        // поле з описом тільки для читання, але хай буде білим — так легше читати
        if (dd && (HWND)lp == dd->edit) {
            SetBkColor((HDC)wp, GetSysColor(COLOR_WINDOW));
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }
        break;
    case WM_DESTROY:
        if (g.details == hwnd) g.details = nullptr;
        delete dd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void OpenDetails() {
    int row = ListView_GetNextItem(g.lvDisks, -1, LVNI_SELECTED);
    if (!g.haveSnap || row < 0 || row >= (int)g.snap.disks.size()) {
        MessageBoxW(g.main, L"Спочатку виберіть диск у верхній таблиці.", APP_TITLE, MB_ICONINFORMATION);
        return;
    }
    if (g.details) DestroyWindow(g.details);

    auto* dd = new DetailsData();
    dd->disk = g.snap.disks[(size_t)row];
    std::wstring title = L"Детальна інформація — Диск " + std::to_wstring(dd->disk.index) + L": " + dd->disk.model;
    g.details = CreateWindowExW(0, kDetailsClass, title.c_str(), WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, S(820), S(720), g.main, nullptr, g.inst, dd);
    if (g.details) ShowWindow(g.details, SW_SHOW);
    else delete dd;
}

// ---------------------------------------------------------------------------
// Звіт у текстовий файл
// ---------------------------------------------------------------------------

static std::wstring BuildReport() {
    const std::wstring nl = L"\r\n";
    const std::wstring line(70, L'=');
    wchar_t comp[MAX_COMPUTERNAME_LENGTH + 1] = L"";
    DWORD n = ARRAYSIZE(comp);
    GetComputerNameW(comp, &n);

    std::wstring r;
    r += L"ЗВІТ ПРО СТАН НАКОПИЧУВАЧІВ ІНФОРМАЦІЇ" + nl;
    r += line + nl;
    r += L"Дата і час:   " + NowText(true) + nl;
    r += L"Комп'ютер:    " + std::wstring(comp) + nl;
    r += L"Режим:        " + std::wstring(g.isAdmin ? L"адміністратор (повні дані)" : L"звичайний користувач (дані SMART обмежені)") + nl;
    r += nl;

    for (auto& d : g.snap.disks) {
        r += line + nl;
        r += L"ДИСК " + std::to_wstring(d.index) + nl;
        r += line + nl;
        r += DiskDescription(d);
        if (!d.smart.empty()) {
            r += nl + L"SMART-атрибути:" + nl;
            r += L"  ID   Атрибут                              Поточне Найгірше Поріг  Значення" + nl;
            for (auto& a : d.smart) {
                wchar_t buf[256];
                std::wstring name = SmartAttrName(d, a.id);
                if (name.size() > 36) name = name.substr(0, 36);
                swprintf(buf, 256, L"  %3d  %-36ls %7d %8d %5ls  %ls", a.id, name.c_str(), a.current, a.worst,
                         a.threshold >= 0 ? std::to_wstring(a.threshold).c_str() : L"—",
                         FormatSmartRaw(d, a).c_str());
                r += buf + nl;
            }
        }
        r += nl;
    }

    r += line + nl + L"РОЗДІЛИ" + nl + line + nl;
    for (auto& v : g.snap.volumes) {
        int used = v.total ? (int)((v.total - v.free) * 100 / v.total) : 0;
        r += v.letter + L"  " + (v.label.empty() ? L"(без мітки)" : v.label) + L", " + v.fs +
             L", диск " + (v.disk >= 0 ? std::to_wstring(v.disk) : L"?") +
             L": обсяг " + FormatSize(v.total) + L", вільно " + FormatSize(v.free) +
             L", зайнято " + std::to_wstring(used) + L"%" + nl;
    }
    r += nl + L"Звіт сформовано програмою «" + std::wstring(APP_TITLE) + L"»." + nl;
    r += L"Програма лише зчитує інформацію і не виконує жодних операцій запису на диски." + nl;
    return r;
}

static void SaveReport() {
    if (!g.haveSnap) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t file[MAX_PATH];
    swprintf(file, MAX_PATH, L"Звіт про диски %04d-%02d-%02d.txt", t.wYear, t.wMonth, t.wDay);

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.main;
    ofn.lpstrFilter = L"Текстовий файл (*.txt)\0*.txt\0Усі файли\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"txt";
    ofn.lpstrTitle = L"Зберегти звіт";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;

    std::string data = "\xEF\xBB\xBF" + ToUtf8(BuildReport());
    HANDLE f = CreateFileW(file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        MessageBoxW(g.main, L"Не вдалося зберегти файл.", APP_TITLE, MB_ICONERROR);
        return;
    }
    DWORD written = 0;
    WriteFile(f, data.data(), (DWORD)data.size(), &written, nullptr);
    CloseHandle(f);
    LogEvent(L"Збережено звіт: " + std::wstring(file));

    if (MessageBoxW(g.main, L"Звіт збережено. Відкрити його зараз?", APP_TITLE, MB_ICONINFORMATION | MB_YESNO) == IDYES)
        ShellExecuteW(g.main, L"open", file, nullptr, nullptr, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------------------
// Інші дії
// ---------------------------------------------------------------------------

static void OpenLog() {
    std::wstring path = LogPath();
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        LogEvent(L"Журнал створено");
    ShellExecuteW(g.main, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// Запускає нову копію програми з правами адміністратора (Windows показує вікно UAC).
// Повертає true, якщо нову копію запущено; тоді поточна копія має завершитися.
static bool LaunchElevated(HWND owner) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);

    // Звільняємо "замок" однієї копії, щоб нова копія (з правами адміністратора) змогла запуститися
    if (g.mutex) { CloseHandle(g.mutex); g.mutex = nullptr; }

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = path;
    // Нова копія вже не проситиме права вдруге (захист від нескінченних перезапусків,
    // якщо UAC вимкнено і Windows запустить її знову без прав адміністратора)
    sei.lpParameters = L"/noelevate";
    sei.hwnd = owner;
    sei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei)) return true;

    g.mutex = CreateMutexW(nullptr, FALSE, kMutexName);   // користувач натиснув "Ні" у вікні UAC
    return false;
}

static void RestartAsAdmin() {
    if (LaunchElevated(g.main)) {
        g.reallyExit = true;
        DestroyWindow(g.main);
    }
}

// ---------------------------------------------------------------------------
// Оновлення програми (мережева частина — в Updater.cpp)
// ---------------------------------------------------------------------------

static bool LoadAutoUpdate() {
    DWORD value = 1, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"CheckUpdates", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return true;   // за замовчуванням перевірка увімкнена
    return value != 0;
}

static void SaveAutoUpdate(bool on) {
    DWORD value = on ? 1 : 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"CheckUpdates", REG_DWORD, &value, sizeof(value));
}

// Вікно-власник для повідомлень: головне вікно, якщо воно на екрані
static HWND DialogOwner() {
    return IsWindowVisible(g.main) && !IsIconic(g.main) ? g.main : nullptr;
}

// Відкриває сторінку в браузері. Копія з правами адміністратора передає адресу Провіднику,
// щоб браузер запустився зі звичайними правами.
static void OpenUrl(const std::wstring& url) {
    if (g.isAdmin) ShellExecuteW(nullptr, L"open", L"explorer.exe", url.c_str(), nullptr, SW_SHOWNORMAL);
    else           ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

struct UpdateCheckResult {
    bool ok = false;
    bool manual = false;    // перевірку запустив користувач — показати результат, навіть якщо оновлень немає
    UpdateInfo info;
    std::wstring error;
};

struct UpdateFileResult {
    HANDLE file = INVALID_HANDLE_VALUE;
    std::wstring path, error;
};

// Запит до GitHub виконується у фоновому потоці, щоб вікно не чекало на відповідь сервера
static void StartUpdateCheck(bool manual) {
    if (g.updateBusy) {               // результат поточної перевірки покажемо як для ручної
        if (manual) g.updateWantManual = true;
        return;
    }
    g.updateBusy = true;
    if (manual) SendMessageW(g.status, SB_SETTEXTW, 0, (LPARAM)L"Перевірка оновлень…");
    HWND target = g.main;
    std::thread([target, manual] {
        auto* r = new UpdateCheckResult();
        r->manual = manual;
        r->ok = FetchLatestRelease(r->info, r->error);
        if (!PostMessageW(target, WM_APP_UPDATE_INFO, 0, (LPARAM)r)) delete r;
    }).detach();
}

static void StartUpdateDownload() {
    if (!g.hasUpdate) return;
    if (g.updateBusy) {               // завантажимо, щойно завершиться поточна перевірка
        g.updateWantInstall = true;
        return;
    }
    g.updateBusy = true;
    SendMessageW(g.status, SB_SETTEXTW, 0, (LPARAM)L"Завантаження оновлення…");
    HWND target = g.main;
    UpdateInfo info = g.update;
    std::thread([target, info] {
        auto* r = new UpdateFileResult();
        r->file = DownloadSetup(info, r->path, r->error);
        if (!PostMessageW(target, WM_APP_UPDATE_FILE, 0, (LPARAM)r)) {
            if (r->file != INVALID_HANDLE_VALUE) CloseHandle(r->file);
            delete r;
        }
    }).detach();
}

static void ShowUpdateDialog() {
    if (!g.hasUpdate || g.updateDialogOpen) return;
    const UpdateInfo& u = g.update;
    bool canInstall = !u.setupUrl.empty() && !u.sha256.empty();

    std::wstring instruction = L"Вийшла нова версія " + u.version;
    std::wstring content = L"Встановлена версія: " L"" APP_VERSION_STR L"\nНова версія: " + u.version;
    if (!u.notes.empty()) content += L"\n\n" + u.notes;
    content += canInstall
        ? L"\n\nПрограма завантажить інсталятор з GitHub, перевірить його контрольну суму і встановить "
          L"оновлення. Після цього вона запуститься знову, налаштування збережуться."
        : L"\n\nАвтоматично встановити цю версію не вийде: на GitHub немає інсталятора з контрольною сумою. "
          L"Її можна завантажити зі сторінки на GitHub.";

    TASKDIALOG_BUTTON buttons[] = {
        { IDB_UPDATE_NOW, L"Оновити зараз" },
        { IDB_UPDATE_PAGE, L"Сторінка на GitHub" },
        { IDCANCEL, L"Пізніше" },
    };
    TASKDIALOGCONFIG tc{};
    tc.cbSize = sizeof(tc);
    tc.hwndParent = DialogOwner();
    tc.hInstance = g.inst;
    tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    tc.pszWindowTitle = L"Оновлення програми";
    tc.pszMainIcon = TD_INFORMATION_ICON;
    tc.pszMainInstruction = instruction.c_str();
    tc.pszContent = content.c_str();
    tc.pButtons = canInstall ? buttons : buttons + 1;
    tc.cButtons = canInstall ? 3 : 2;
    tc.nDefaultButton = canInstall ? IDB_UPDATE_NOW : IDB_UPDATE_PAGE;

    int pressed = IDCANCEL;
    g.updateDialogOpen = true;
    if (FAILED(TaskDialogIndirect(&tc, &pressed, nullptr, nullptr))) pressed = IDCANCEL;
    g.updateDialogOpen = false;

    if (pressed == IDB_UPDATE_NOW) StartUpdateDownload();
    else if (pressed == IDB_UPDATE_PAGE) OpenUrl(u.pageUrl);
}

static void OnUpdateInfo(const UpdateCheckResult& r) {
    g.updateBusy = false;
    // Поки йшла перевірка, користувач міг попросити перевірку вручну або натиснути «Оновити зараз»
    bool manual = r.manual || g.updateWantManual;
    bool install = g.updateWantInstall;
    g.updateWantManual = g.updateWantInstall = false;
    if (manual && g.haveSnap) UpdateStatus();

    if (r.ok) {
        g.hasUpdate = CompareVersions(r.info.version, L"" APP_VERSION_STR) > 0;
        if (g.hasUpdate) {
            g.update = r.info;
            if (g.updateLogged != r.info.version) {
                g.updateLogged = r.info.version;
                LogEvent(L"Доступна нова версія програми: " + r.info.version);
            }
        }
    }
    if (install && g.hasUpdate) {
        StartUpdateDownload();
        return;
    }
    if (!r.ok) {
        // Автоматична перевірка без інтернету — звичайна ситуація, тож про неї не повідомляємо
        if (manual)
            MessageBoxW(DialogOwner(), (L"Не вдалося перевірити оновлення: " + r.error + L".").c_str(),
                        L"Оновлення програми", MB_ICONWARNING | MB_SETFOREGROUND);
        return;
    }
    if (!g.hasUpdate) {
        if (manual)
            MessageBoxW(DialogOwner(), L"У вас встановлена остання версія програми (" L"" APP_VERSION_STR L").",
                        L"Оновлення програми", MB_ICONINFORMATION | MB_SETFOREGROUND);
        return;
    }
    if (manual || DialogOwner()) {
        ShowUpdateDialog();
    } else {
        // Програма в треї — показуємо сповіщення; клік по ньому відкриває вікно оновлення
        TrayBalloon(L"Вийшла нова версія програми",
                    L"Версія " + r.info.version + L" вже доступна. Клацніть тут, щоб оновити програму.", NIIF_INFO);
        g.balloonIsUpdate = true;
    }
}

static void OnUpdateFile(UpdateFileResult& r) {
    g.updateBusy = false;
    g.updateWantManual = g.updateWantInstall = false;   // натискання під час завантаження не повторюємо
    if (g.haveSnap) UpdateStatus();
    if (r.file == INVALID_HANDLE_VALUE) {
        LogEvent(L"Не вдалося завантажити оновлення: " + r.error);
        std::wstring text = L"Не вдалося завантажити оновлення: " + r.error +
                            L".\n\nВідкрити сторінку програми на GitHub, щоб завантажити нову версію вручну?";
        if (MessageBoxW(DialogOwner(), text.c_str(), L"Оновлення програми", MB_ICONWARNING | MB_YESNO | MB_SETFOREGROUND) == IDYES)
            OpenUrl(g.update.pageUrl);
        return;
    }

    // Інсталятор працює в тихому режимі: показує лише смужку прогресу. Параметр /UPDATE каже йому
    // дочекатися, поки ця копія програми закриється, і після встановлення запустити нову версію.
    std::wstring params = L"/SILENT /SUPPRESSMSGBOXES /NORESTART /UPDATE";
    if (!IsWindowVisible(g.main)) params += L" /TRAY";
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOASYNC;
    sei.hwnd = DialogOwner();
    sei.lpVerb = L"open";
    sei.lpFile = r.path.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow = SW_SHOWNORMAL;
    BOOL started = ShellExecuteExW(&sei);
    DWORD err = GetLastError();
    CloseHandle(r.file);   // інсталятор уже запущений, файл більше не потрібно захищати від змін
    r.file = INVALID_HANDLE_VALUE;

    if (!started) {
        std::wstring text = err == ERROR_CANCELLED
            ? L"Оновлення скасовано: для встановлення потрібен дозвіл у вікні контролю облікових записів (UAC)."
            : L"Не вдалося запустити інсталятор (код помилки " + std::to_wstring(err) + L").";
        MessageBoxW(DialogOwner(), text.c_str(), L"Оновлення програми", MB_ICONWARNING | MB_SETFOREGROUND);
        return;
    }
    LogEvent(L"Встановлюється оновлення до версії " + g.update.version);
    g.reallyExit = true;
    DestroyWindow(g.main);   // закриваємося, щоб інсталятор міг замінити файли програми
}

static void ShowAbout() {
    std::wstring instruction = std::wstring(APP_TITLE) + L", версія " + L"" APP_VERSION_STR;
    std::wstring content =
        L"Курсовий проект на тему:\n"
        L"«Розробка програмного застосунку для програмного моніторингу накопичувачів»\n\n"
        L"Автор: " + std::wstring(APP_AUTHOR) + L"\n\n"
        L"Дані отримуються через Windows Management Instrumentation (WMI) та SMART.\n"
        L"Програма лише зчитує інформацію і нічого не записує на диски.\n\n"
        L"Щоб перевірити, чи вийшла нова версія, програма раз на добу звертається до GitHub. "
        L"Жодних даних про комп'ютер чи диски при цьому не надсилається.";

    TASKDIALOG_BUTTON buttons[] = { { IDB_ABOUT_UPDATE, L"Перевірити оновлення" } };
    TASKDIALOGCONFIG tc{};
    tc.cbSize = sizeof(tc);
    tc.hwndParent = g.main;
    tc.hInstance = g.inst;
    tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    tc.pszWindowTitle = L"Про програму";
    tc.pszMainIcon = MAKEINTRESOURCEW(IDI_APP);
    tc.pszMainInstruction = instruction.c_str();
    tc.pszContent = content.c_str();
    tc.pButtons = buttons;
    tc.cButtons = 1;
    tc.dwCommonButtons = TDCBF_OK_BUTTON;
    tc.nDefaultButton = IDOK;

    int pressed = IDOK;
    if (SUCCEEDED(TaskDialogIndirect(&tc, &pressed, nullptr, nullptr)) && pressed == IDB_ABOUT_UPDATE)
        StartUpdateCheck(true);
}

// ---------------------------------------------------------------------------
// Головне вікно
// ---------------------------------------------------------------------------

static HWND MakeButton(HWND parent, const wchar_t* text, int id) {
    HWND b = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                             0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, g.inst, nullptr);
    SendMessageW(b, WM_SETFONT, (WPARAM)g.font, TRUE);
    g.buttons.push_back(b);
    return b;
}

static HWND MakeLabel(HWND parent, const wchar_t* text) {
    HWND l = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, parent, nullptr, g.inst, nullptr);
    SendMessageW(l, WM_SETFONT, (WPARAM)g.fontBold, TRUE);
    return l;
}

static void Layout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    int m = S(10);

    // Рядок кнопок: ширина кожної — за довжиною тексту
    HDC dc = GetDC(hwnd);
    HGDIOBJ old = SelectObject(dc, g.font);
    int x = m;
    for (HWND b : g.buttons) {
        if (!IsWindowVisible(b)) continue;
        wchar_t text[128];
        GetWindowTextW(b, text, 128);
        SIZE sz{};
        GetTextExtentPoint32W(dc, text, lstrlenW(text), &sz);
        int bw = sz.cx + S(28);
        MoveWindow(b, x, m, bw, S(30), TRUE);
        x += bw + S(6);
    }
    SelectObject(dc, old);
    ReleaseDC(hwnd, dc);

    // Рядок стану
    SendMessageW(g.status, WM_SIZE, 0, 0);
    RECT sr;
    GetWindowRect(g.status, &sr);
    int sh = sr.bottom - sr.top;
    int parts[3] = { w * 24 / 100, w * 68 / 100, -1 };
    SendMessageW(g.status, SB_SETPARTS, 3, (LPARAM)parts);

    // Три таблиці одна під одною
    int y = m + S(30) + m;
    int labelH = S(20), gap = S(10);
    int listsH = h - y - sh - m - 3 * (labelH + S(4)) - 2 * gap;
    if (listsH < S(150)) listsH = S(150);
    int h1 = listsH * 38 / 100, h2 = listsH * 30 / 100, h3 = listsH - h1 - h2;

    MoveWindow(g.lblDisks, m, y, w - 2 * m, labelH, TRUE);   y += labelH + S(4);
    MoveWindow(g.lvDisks, m, y, w - 2 * m, h1, TRUE);        y += h1 + gap;
    MoveWindow(g.lblVols, m, y, w - 2 * m, labelH, TRUE);    y += labelH + S(4);
    MoveWindow(g.lvVols, m, y, w - 2 * m, h2, TRUE);         y += h2 + gap;
    MoveWindow(g.lblLoad, m, y, w - 2 * m, labelH, TRUE);    y += labelH + S(4);
    MoveWindow(g.lvLoad, m, y, w - 2 * m, h3, TRUE);
}

static void CreateMainControls(HWND hwnd) {
    MakeButton(hwnd, L"Оновити зараз", IDC_BTN_REFRESH);
    MakeButton(hwnd, L"Детальніше про диск", IDC_BTN_DETAILS);
    MakeButton(hwnd, L"Зберегти звіт", IDC_BTN_REPORT);
    MakeButton(hwnd, L"Журнал подій", IDC_BTN_LOG);
    g.btnAdmin = MakeButton(hwnd, L"Перезапустити як адміністратор", IDC_BTN_ADMIN);
    MakeButton(hwnd, L"Про програму", IDC_BTN_ABOUT);
    if (g.isAdmin) ShowWindow(g.btnAdmin, SW_HIDE);
    else SendMessageW(g.btnAdmin, BCM_SETSHIELD, 0, TRUE);   // значок щита, як у системних кнопках

    g.lblDisks = MakeLabel(hwnd, L"Фізичні накопичувачі   (двічі клацніть по диску, щоб побачити SMART та пояснення)");
    g.lblVols = MakeLabel(hwnd, L"Розділи та вільне місце");
    g.lblLoad = MakeLabel(hwnd, L"Активність дисків у реальному часі");

    int rowH = S(24);
    g.lvDisks = CreateList(hwnd, IDC_LIST_DISKS, rowH);
    AddColumn(g.lvDisks, 0, L"№", 40);
    AddColumn(g.lvDisks, 1, L"Модель", 250);
    AddColumn(g.lvDisks, 2, L"Тип", 90);
    AddColumn(g.lvDisks, 3, L"Інтерфейс", 80);
    AddColumn(g.lvDisks, 4, L"Обсяг", 85, LVCFMT_RIGHT);
    AddColumn(g.lvDisks, 5, L"Розділи", 75);
    AddColumn(g.lvDisks, 6, L"Температура", 95, LVCFMT_RIGHT);
    AddColumn(g.lvDisks, 7, L"Напрацювання", 110, LVCFMT_RIGHT);
    AddColumn(g.lvDisks, 8, L"Здоров’я", 85, LVCFMT_RIGHT);
    AddColumn(g.lvDisks, 9, L"Стан", 90, LVCFMT_CENTER);

    g.lvVols = CreateList(hwnd, IDC_LIST_VOLS, rowH);
    AddColumn(g.lvVols, 0, L"Розділ", 65);
    AddColumn(g.lvVols, 1, L"Мітка", 170);
    AddColumn(g.lvVols, 2, L"Файлова система", 120);
    AddColumn(g.lvVols, 3, L"Диск №", 65, LVCFMT_CENTER);
    AddColumn(g.lvVols, 4, L"Обсяг", 95, LVCFMT_RIGHT);
    AddColumn(g.lvVols, 5, L"Вільно", 95, LVCFMT_RIGHT);
    AddColumn(g.lvVols, 6, L"Заповненість", 260);

    g.lvLoad = CreateList(hwnd, IDC_LIST_LOAD, rowH);
    AddColumn(g.lvLoad, 0, L"Диск", 330);
    AddColumn(g.lvLoad, 1, L"Завантаженість", 220);
    AddColumn(g.lvLoad, 2, L"Читання", 110, LVCFMT_RIGHT);
    AddColumn(g.lvLoad, 3, L"Запис", 110, LVCFMT_RIGHT);
    AddColumn(g.lvLoad, 4, L"Черга запитів", 100, LVCFMT_RIGHT);

    g.status = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                               0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_STATUS, g.inst, nullptr);
    SendMessageW(g.status, WM_SETFONT, (WPARAM)g.font, TRUE);
}

static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g.msgTaskbarCreated && g.msgTaskbarCreated) {   // Провідник перезапустився — повертаємо значок
        TrayAdd();
        return 0;
    }

    switch (msg) {
    case WM_CREATE:
        g.main = hwnd;
        CreateMainControls(hwnd);
        SendMessageW(g.status, SB_SETTEXTW, 0, (LPARAM)L"Збір інформації про диски…");
        TrayAdd();
        // Дозволяємо звичайній копії програми "достукатися" до копії з правами адміністратора
        ChangeWindowMessageFilterEx(hwnd, WM_APP_SHOWME, MSGFLT_ALLOW, nullptr);
        ChangeWindowMessageFilterEx(hwnd, g.msgTaskbarCreated, MSGFLT_ALLOW, nullptr);
        if (g.autoUpdate) SetTimer(hwnd, IDT_UPDATE, UPDATE_FIRST_MS, nullptr);
        return 0;

    case WM_TIMER:
        if (wp == IDT_UPDATE) {
            SetTimer(hwnd, IDT_UPDATE, UPDATE_INTERVAL_MS, nullptr);   // наступна перевірка — за добу
            StartUpdateCheck(false);
        }
        return 0;

    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) {
            g_visible = false;              // згорнуте вікно — не опитуємо активність дисків
        } else {
            if (!g_visible && IsWindowVisible(hwnd)) {
                g_visible = true;
                SetEvent(g_evWake);
            }
            Layout(hwnd);
        }
        return 0;

    case WM_GETMINMAXINFO: {
        auto* mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize.x = S(900);
        mm->ptMinTrackSize.y = S(560);
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BTN_REFRESH:
        case ID_TRAY_REFRESH:
            SendMessageW(g.status, SB_SETTEXTW, 0, (LPARAM)L"Оновлення…");
            SetEvent(g_evRefresh);
            break;
        case IDC_BTN_DETAILS: OpenDetails(); break;
        case IDC_BTN_REPORT:  SaveReport(); break;
        case IDC_BTN_LOG:     OpenLog(); break;
        case IDC_BTN_ADMIN:   RestartAsAdmin(); break;
        case IDC_BTN_ABOUT:   ShowAbout(); break;
        case ID_TRAY_OPEN:    ShowMain(); break;
        case ID_TRAY_UPDATE_CHECK:
            if (g.hasUpdate) ShowUpdateDialog();
            else StartUpdateCheck(true);
            break;
        case ID_TRAY_UPDATE_AUTO:
            g.autoUpdate = !g.autoUpdate;
            SaveAutoUpdate(g.autoUpdate);
            if (g.autoUpdate) SetTimer(hwnd, IDT_UPDATE, UPDATE_FIRST_MS, nullptr);
            else KillTimer(hwnd, IDT_UPDATE);
            LogEvent(g.autoUpdate ? L"Автоматичну перевірку оновлень увімкнено" : L"Автоматичну перевірку оновлень вимкнено");
            break;
        case ID_TRAY_EXIT:
            g.reallyExit = true;
            DestroyWindow(hwnd);
            break;
        }
        return 0;

    case WM_NOTIFY: {
        auto* hdr = (NMHDR*)lp;
        if (hdr->code == NM_CUSTOMDRAW &&
            (hdr->idFrom == IDC_LIST_DISKS || hdr->idFrom == IDC_LIST_VOLS || hdr->idFrom == IDC_LIST_LOAD))
            return OnCustomDraw((NMLVCUSTOMDRAW*)lp);
        if (hdr->idFrom == IDC_LIST_DISKS && hdr->code == LVN_ITEMACTIVATE)   // подвійний клік або Enter
            OpenDetails();
        break;
    }

    case WM_APP_SNAPSHOT: {
        std::unique_ptr<Snapshot> s((Snapshot*)lp);
        g.snap = std::move(*s);
        g.haveSnap = true;
        FillDisks();
        FillVolumes();
        UpdateStatus();
        CheckAlerts();
        return 0;
    }

    case WM_APP_LOAD: {
        std::unique_ptr<std::vector<DiskLoad>> l((std::vector<DiskLoad>*)lp);
        if (g.haveSnap) FillLoad(*l);
        return 0;
    }

    case WM_APP_TRAY:
        if (LOWORD(lp) == WM_LBUTTONDBLCLK || LOWORD(lp) == WM_LBUTTONUP) {
            ShowMain();
        } else if (LOWORD(lp) == NIN_BALLOONUSERCLICK && g.balloonIsUpdate) {
            ShowMain();
            ShowUpdateDialog();
        } else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, ID_TRAY_OPEN, L"Відкрити");
            AppendMenuW(m, MF_STRING, ID_TRAY_REFRESH, L"Перевірити диски зараз");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, ID_TRAY_UPDATE_CHECK,
                        g.hasUpdate ? L"Встановити нову версію…" : L"Перевірити оновлення");
            AppendMenuW(m, MF_STRING | (g.autoUpdate ? MF_CHECKED : 0), ID_TRAY_UPDATE_AUTO,
                        L"Перевіряти оновлення автоматично");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, ID_TRAY_EXIT, L"Вихід");
            SetMenuDefaultItem(m, ID_TRAY_OPEN, FALSE);
            POINT pt;
            GetCursorPos(&pt);
            SetForegroundWindow(hwnd);
            TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
            PostMessageW(hwnd, WM_NULL, 0, 0);
            DestroyMenu(m);
        }
        return 0;

    case WM_APP_SHOWME:
        ShowMain();
        return 0;

    case WM_APP_UPDATE_INFO: {
        std::unique_ptr<UpdateCheckResult> r((UpdateCheckResult*)lp);
        OnUpdateInfo(*r);
        return 0;
    }

    case WM_APP_UPDATE_FILE: {
        std::unique_ptr<UpdateFileResult> r((UpdateFileResult*)lp);
        OnUpdateFile(*r);
        return 0;
    }

    case WM_CLOSE:
        if (!g.reallyExit) {   // хрестик згортає програму в трей
            HideToTray();
            return 0;
        }
        break;

    case WM_ENDSESSION:
        if (wp) {
            g.reallyExit = true;
            if (lp & ENDSESSION_CLOSEAPP) {
                // Інсталятор (через Restart Manager) просить закрити програму, щоб замінити її файли
                LogEvent(L"Програму закрито на запит інсталятора");
                DestroyWindow(hwnd);
            } else {
                LogEvent(L"Завершення роботи Windows");   // вимкнення комп'ютера
            }
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, IDT_UPDATE);
        TrayRemove();
        SetEvent(g_evStop);
        // Фоновий потік зупиняється між запитами. Якщо запит до несправного диска «завис»,
        // не чекаємо безкінечно: інакше програма не закрилася б, тримала б mutex і не давала
        // інсталятору оновлення замінити свої файли.
        if (g_worker.joinable()) {
            if (WaitForSingleObject((HANDLE)g_worker.native_handle(), 10000) != WAIT_OBJECT_0) {
                LogEvent(L"Програму закрито примусово: запит до диска не відповідав");
                TerminateProcess(GetCurrentProcess(), 0);
            }
            g_worker.join();
        }
        LogEvent(L"Програму закрито");
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Точка входу
// ---------------------------------------------------------------------------

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int nShow) {
    bool startInTray = cmdLine && wcsstr(cmdLine, L"/tray") != nullptr;
    bool noElevate = cmdLine && wcsstr(cmdLine, L"/noelevate") != nullptr;

    // Дозволяємо лише одну копію програми
    // ERROR_ACCESS_DENIED означає, що копія вже працює з правами адміністратора (наприклад,
    // з автозапуску), а копія зі звичайними правами не має доступу до її mutex
    g.mutex = CreateMutexW(nullptr, FALSE, kMutexName);
    DWORD mutexErr = GetLastError();
    if (mutexErr == ERROR_ALREADY_EXISTS || mutexErr == ERROR_ACCESS_DENIED) {
        if (!startInTray) {
            HWND other = FindWindowW(kMainClass, nullptr);
            if (other) {
                AllowSetForegroundWindow(ASFW_ANY);
                PostMessageW(other, WM_APP_SHOWME, 0, 0);
            }
        }
        if (g.mutex) CloseHandle(g.mutex);
        return 0;
    }

    g.inst = inst;
    g.isAdmin = IsRunningAsAdmin();
    g.autoUpdate = LoadAutoUpdate();
    g.msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Без прав адміністратора Windows не дає прочитати SMART, температуру і знос, тому під час
    // звичайного запуску одразу просимо ці права. Якщо користувач відмовиться у вікні UAC,
    // програма працює у звичайному режимі. Автозапуск (/tray) нічого не питає: завдання
    // планувальника і так запускає програму з правами адміністратора.
    if (!g.isAdmin && !startInTray && !noElevate && LaunchElevated(nullptr)) {
        CoUninitialize();
        return 0;
    }

    CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT,
                         RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    HDC sdc = GetDC(nullptr);
    g.dpi = GetDeviceCaps(sdc, LOGPIXELSY);
    ReleaseDC(nullptr, sdc);

    // Шрифти: системний шрифт інтерфейсу (Segoe UI), напівжирний і моноширинний
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g.font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW bold = ncm.lfMessageFont;
    bold.lfWeight = FW_SEMIBOLD;
    g.fontBold = CreateFontIndirectW(&bold);
    g.fontMono = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");

    g.icon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                               GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    g.iconSmall = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.hIcon = g.icon;
    wc.hIconSm = g.iconSmall;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kMainClass;
    RegisterClassExW(&wc);

    wc.lpfnWndProc = DetailsProc;
    wc.lpszClassName = kDetailsClass;
    RegisterClassExW(&wc);

    std::wstring title = std::wstring(APP_TITLE) + (g.isAdmin ? L"  [адміністратор]" : L"");
    HWND hwnd = CreateWindowExW(0, kMainClass, title.c_str(), WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, S(1150), S(760), nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;

    LogEvent(std::wstring(L"Програму запущено") + (g.isAdmin ? L" (з правами адміністратора)" : L"") +
             (startInTray ? L", автозапуск у треї" : L""));

    g_evStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_evRefresh = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_evWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if (startInTray) {
        g_visible = false;
    } else {
        ShowWindow(hwnd, nShow);
        UpdateWindow(hwnd);
        g_visible = !IsIconic(hwnd);   // ярлик може запускати програму згорнутою
    }
    g_worker = std::thread(WorkerThread, hwnd);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    CloseHandle(g_evStop);
    CloseHandle(g_evRefresh);
    CloseHandle(g_evWake);
    DeleteObject(g.font);
    DeleteObject(g.fontBold);
    DeleteObject(g.fontMono);
    if (g.mutex) CloseHandle(g.mutex);
    CoUninitialize();
    return 0;
}
