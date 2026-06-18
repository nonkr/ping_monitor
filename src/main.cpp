#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <uxtheme.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cwctype>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "resource.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

namespace {

constexpr wchar_t kAppName[] = L"Ping Monitor";
constexpr wchar_t kCalVer[] = L"2026.06.18";
constexpr wchar_t kWindowTitle[] = L"Ping Monitor 2026.06.18 - Powered by nonkr";
constexpr wchar_t kSettingsClass[] = L"PingMonitorSettingsWindow";
constexpr wchar_t kMonitorClass[] = L"PingMonitorCompactWindow";

constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT WM_PING_UPDATED = WM_APP + 2;
constexpr UINT_PTR TIMER_REPAINT = 101;

constexpr int IDC_IP_EDIT = 1001;
constexpr int IDC_INTERVAL_EDIT = 1002;
constexpr int IDC_TIMEOUT_EDIT = 1003;
constexpr int IDC_START_BUTTON = 1004;
constexpr int IDC_STOP_BUTTON = 1005;
constexpr int IDC_HIDE_BUTTON = 1006;

constexpr int IDM_SHOW_SETTINGS = 2001;
constexpr int IDM_START = 2002;
constexpr int IDM_STOP = 2003;
constexpr int IDM_EXIT = 2004;
constexpr int IDM_ALWAYS_ON_TOP = 2005;

constexpr COLORREF kWindowBg = RGB(243, 243, 243);
constexpr COLORREF kSurfaceBg = RGB(255, 255, 255);
constexpr COLORREF kBorder = RGB(226, 226, 226);
constexpr COLORREF kText = RGB(32, 32, 32);
constexpr COLORREF kSubtleText = RGB(96, 96, 96);
constexpr COLORREF kGreen = RGB(16, 185, 97);
constexpr COLORREF kRed = RGB(220, 56, 56);
constexpr COLORREF kYellow = RGB(232, 176, 36);
constexpr COLORREF kGray = RGB(150, 150, 150);
constexpr COLORREF kFlash = RGB(255, 255, 255);
constexpr unsigned int kHistoryMask = 0x3F;
constexpr int kHistoryWindow = 6;
constexpr int kFlapAlertTransitions = 2;
constexpr DWORD kPingFlashMs = 160;

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif

struct Target {
    std::wstring address;
    bool online = false;
    bool timeoutAlert = false;
    unsigned int historyBits = 0;
    int historyCount = 0;
    DWORD flashUntil = 0;
    DWORD rtt = 0;
};

enum class PingResult {
    Online,
    Timeout,
    Failed
};

bool IsFlapping(unsigned int historyBits, int historyCount) {
    if (historyCount < 3) {
        return false;
    }

    int count = historyCount < kHistoryWindow ? historyCount : kHistoryWindow;
    bool sawOnline = false;
    bool sawOffline = false;
    int transitions = 0;
    int previous = historyBits & 1;

    for (int i = 0; i < count; ++i) {
        int current = (historyBits >> i) & 1;
        sawOnline = sawOnline || current != 0;
        sawOffline = sawOffline || current == 0;
        if (i > 0 && current != previous) {
            ++transitions;
        }
        previous = current;
    }

    return sawOnline && sawOffline && transitions >= kFlapAlertTransitions;
}

struct AppState {
    HINSTANCE instance = nullptr;
    HWND settings = nullptr;
    HWND monitor = nullptr;
    HWND ipEdit = nullptr;
    HWND intervalEdit = nullptr;
    HWND timeoutEdit = nullptr;
    HWND startButton = nullptr;
    HWND stopButton = nullptr;
    HWND hideButton = nullptr;
    NOTIFYICONDATAW tray = {};
    HICON appIcon = nullptr;
    HICON smallIcon = nullptr;
    HFONT uiFont = nullptr;
    HFONT smallFont = nullptr;
    HBRUSH windowBrush = nullptr;
    HBRUSH surfaceBrush = nullptr;
    WNDPROC ipEditProc = nullptr;

    std::mutex mutex;
    std::vector<Target> targets;
    std::wstring addressText;
    std::thread worker;
    std::atomic_bool running{false};
    DWORD intervalMs = 1000;
    DWORD timeoutMs = 2000;
    POINT monitorPos = {80, 80};
    bool alwaysOnTop = true;
    bool monitorCollapsed = false;
};

AppState g;

std::wstring Trim(const std::wstring& text) {
    size_t first = 0;
    while (first < text.size() && std::iswspace(text[first])) {
        ++first;
    }
    size_t last = text.size();
    while (last > first && std::iswspace(text[last - 1])) {
        --last;
    }
    return text.substr(first, last - first);
}

std::vector<std::wstring> SplitAddresses(const std::wstring& text) {
    std::vector<std::wstring> addresses;
    std::wstring current;
    bool inComment = false;

    for (wchar_t ch : text) {
        if (ch == L'#') {
            inComment = true;
            continue;
        }

        if (ch == L'\r' || ch == L'\n') {
            current = Trim(current);
            if (!current.empty()) {
                addresses.push_back(current);
            }
            current.clear();
            inComment = false;
        } else if (inComment) {
            continue;
        } else if (ch == L',' || ch == L';') {
            current = Trim(current);
            if (!current.empty()) {
                addresses.push_back(current);
            }
            current.clear();
        } else {
            current.push_back(ch);
        }
    }

    current = Trim(current);
    if (!current.empty()) {
        addresses.push_back(current);
    }
    return addresses;
}

std::wstring JoinAddresses(const std::vector<Target>& targets) {
    std::wstring text;
    for (const auto& target : targets) {
        if (!text.empty()) {
            text += L"\r\n";
        }
        text += target.address;
    }
    return text;
}

std::wstring EscapeConfigValue(const std::wstring& text) {
    std::wstring result;
    for (wchar_t ch : text) {
        if (ch == L'\\') {
            result += L"\\\\";
        } else if (ch == L'\r') {
            result += L"\\r";
        } else if (ch == L'\n') {
            result += L"\\n";
        } else {
            result.push_back(ch);
        }
    }
    return result;
}

std::wstring UnescapeConfigValue(const std::wstring& text) {
    std::wstring result;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\\' && i + 1 < text.size()) {
            wchar_t next = text[++i];
            if (next == L'r') {
                result += L'\r';
            } else if (next == L'n') {
                result += L'\n';
            } else {
                result.push_back(next);
            }
        } else {
            result.push_back(text[i]);
        }
    }
    return result;
}

std::wstring GetWindowTextString(HWND hwnd) {
    int length = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    if (length > 0) {
        GetWindowTextW(hwnd, text.data(), length + 1);
    }
    text.resize(static_cast<size_t>(length));
    return text;
}

HMENU ControlId(int id) {
    return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
}

DWORD GetEditDword(HWND hwnd, DWORD fallback, DWORD minValue, DWORD maxValue) {
    wchar_t buffer[32] = {};
    GetWindowTextW(hwnd, buffer, ARRAYSIZE(buffer));
    wchar_t* end = nullptr;
    unsigned long value = wcstoul(buffer, &end, 10);
    if (end == buffer || value < minValue || value > maxValue) {
        return fallback;
    }
    return static_cast<DWORD>(value);
}

void SetChildFont(HWND hwnd) {
    if (g.uiFont) {
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(g.uiFont), TRUE);
    }
}

void SetControlTheme(HWND hwnd) {
    SetChildFont(hwnd);
    SetWindowTheme(hwnd, L"Explorer", nullptr);
}

void UpdateButtonState() {
    if (g.startButton) {
        EnableWindow(g.startButton, !g.running);
    }
    if (g.stopButton) {
        EnableWindow(g.stopButton, g.running);
    }
}

void ApplyRoundedCorners(HWND hwnd) {
    const DWORD preference = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &preference, sizeof(preference));
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }

    int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }

    int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::wstring GetConfigPath() {
    DWORD needed = GetEnvironmentVariableW(L"APPDATA", nullptr, 0);
    if (needed == 0) {
        return L"config.txt";
    }

    std::wstring dir(static_cast<size_t>(needed), L'\0');
    GetEnvironmentVariableW(L"APPDATA", dir.data(), needed);
    while (!dir.empty() && dir.back() == L'\0') {
        dir.pop_back();
    }

    dir += L"\\PingMonitor";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\config.txt";
}

void SaveConfig() {
    std::vector<Target> targets;
    std::wstring addressText;
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        targets = g.targets;
        addressText = g.addressText.empty() ? JoinAddresses(targets) : g.addressText;
    }

    std::wstring data;
    data += L"intervalMs=" + std::to_wstring(g.intervalMs) + L"\n";
    data += L"timeoutMs=" + std::to_wstring(g.timeoutMs) + L"\n";
    data += L"monitorX=" + std::to_wstring(g.monitorPos.x) + L"\n";
    data += L"monitorY=" + std::to_wstring(g.monitorPos.y) + L"\n";
    data += L"alwaysOnTop=" + std::to_wstring(g.alwaysOnTop ? 1 : 0) + L"\n";
    data += L"addressText=" + EscapeConfigValue(addressText) + L"\n";

    std::string utf8 = WideToUtf8(data);
    HANDLE file = CreateFileW(GetConfigPath().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    DWORD written = 0;
    WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(file);
}

void LoadConfig() {
    HANDLE file = CreateFileW(GetConfigPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    LARGE_INTEGER fileSize = {};
    if (!GetFileSizeEx(file, &fileSize) || fileSize.QuadPart <= 0 || fileSize.QuadPart > 1024 * 1024) {
        CloseHandle(file);
        return;
    }

    std::string bytes(static_cast<size_t>(fileSize.QuadPart), '\0');
    DWORD read = 0;
    ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
    CloseHandle(file);
    bytes.resize(read);

    std::wstring text = Utf8ToWide(bytes);
    std::vector<Target> loadedTargets;
    std::vector<std::wstring> legacyAddresses;
    std::wstring loadedAddressText;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(L'\n', start);
        std::wstring line = text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        line = Trim(line);

        if (line.rfind(L"intervalMs=", 0) == 0) {
            g.intervalMs = wcstoul(line.c_str() + 11, nullptr, 10);
        } else if (line.rfind(L"timeoutMs=", 0) == 0) {
            g.timeoutMs = wcstoul(line.c_str() + 10, nullptr, 10);
        } else if (line.rfind(L"monitorX=", 0) == 0) {
            g.monitorPos.x = static_cast<LONG>(wcstol(line.c_str() + 9, nullptr, 10));
        } else if (line.rfind(L"monitorY=", 0) == 0) {
            g.monitorPos.y = static_cast<LONG>(wcstol(line.c_str() + 9, nullptr, 10));
        } else if (line.rfind(L"alwaysOnTop=", 0) == 0) {
            g.alwaysOnTop = wcstoul(line.c_str() + 12, nullptr, 10) != 0;
        } else if (line.rfind(L"addressText=", 0) == 0) {
            loadedAddressText = UnescapeConfigValue(line.substr(12));
        } else if (line.rfind(L"address=", 0) == 0) {
            std::wstring address = Trim(line.substr(8));
            if (!address.empty()) {
                legacyAddresses.push_back(address);
            }
        }

        if (end == std::wstring::npos) {
            break;
        }
        start = end + 1;
    }

    if (!loadedAddressText.empty()) {
        auto parsed = SplitAddresses(loadedAddressText);
        for (const auto& address : parsed) {
            loadedTargets.push_back({address});
        }
    } else {
        for (const auto& address : legacyAddresses) {
            loadedTargets.push_back({address});
            if (!loadedAddressText.empty()) {
                loadedAddressText += L"\r\n";
            }
            loadedAddressText += address;
        }
    }

    {
        std::lock_guard<std::mutex> lock(g.mutex);
        g.targets = loadedTargets;
        g.addressText = loadedAddressText;
    }

    if (g.intervalMs < 500 || g.intervalMs > 600000) {
        g.intervalMs = 1000;
    }
    if (g.timeoutMs < 100 || g.timeoutMs > 60000) {
        g.timeoutMs = 2000;
    }
}

PingResult PingIpv4(const std::wstring& address, DWORD timeoutMs, DWORD* rtt) {
    IN_ADDR addr = {};
    if (InetPtonW(AF_INET, address.c_str(), &addr) != 1) {
        return PingResult::Failed;
    }

    HANDLE icmp = IcmpCreateFile();
    if (icmp == INVALID_HANDLE_VALUE) {
        return PingResult::Failed;
    }

    char payload[] = "ping-monitor";
    DWORD replySize = sizeof(ICMP_ECHO_REPLY) + sizeof(payload) + 8;
    std::vector<BYTE> reply(replySize);

    DWORD result = IcmpSendEcho(icmp, addr.S_un.S_addr, payload, sizeof(payload), nullptr,
                                reply.data(), replySize, timeoutMs);
    IcmpCloseHandle(icmp);

    if (result == 0) {
        return PingResult::Timeout;
    }

    auto* echo = reinterpret_cast<PICMP_ECHO_REPLY>(reply.data());
    if (echo->Status == IP_SUCCESS) {
        if (rtt) {
            *rtt = echo->RoundTripTime;
        }
        return PingResult::Online;
    }
    if (echo->Status == IP_REQ_TIMED_OUT) {
        return PingResult::Timeout;
    }
    return PingResult::Failed;
}

void UpdateTrayMenuState(HMENU menu) {
    EnableMenuItem(menu, IDM_START, MF_BYCOMMAND | (g.running ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(menu, IDM_STOP, MF_BYCOMMAND | (g.running ? MF_ENABLED : MF_GRAYED));
    CheckMenuItem(menu, IDM_ALWAYS_ON_TOP, MF_BYCOMMAND | (g.alwaysOnTop ? MF_CHECKED : MF_UNCHECKED));
}

void ShowTrayMenu(HWND owner) {
    HMENU menu = CreatePopupMenu();
    const bool settingsVisible = g.settings && IsWindowVisible(g.settings);
    AppendMenuW(menu, MF_STRING, IDM_SHOW_SETTINGS, settingsVisible ? L"隐藏设置" : L"显示设置");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_ALWAYS_ON_TOP, L"置顶显示");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_START, L"开始监控");
    AppendMenuW(menu, MF_STRING, IDM_STOP, L"停止监控");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出");
    UpdateTrayMenuState(menu);

    POINT pt = {};
    GetCursorPos(&pt);
    SetForegroundWindow(owner);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, owner, nullptr);
    DestroyMenu(menu);
}

void AddTrayIcon(HWND owner) {
    g.tray = {};
    g.tray.cbSize = sizeof(g.tray);
    g.tray.hWnd = owner;
    g.tray.uID = 1;
    g.tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g.tray.uCallbackMessage = WM_TRAYICON;
    g.tray.hIcon = g.smallIcon ? g.smallIcon : g.appIcon;
    wcscpy_s(g.tray.szTip, kAppName);
    Shell_NotifyIconW(NIM_ADD, &g.tray);
}

void RemoveTrayIcon() {
    if (g.tray.cbSize) {
        Shell_NotifyIconW(NIM_DELETE, &g.tray);
    }
}

SIZE CalculateMonitorSize(bool collapsed = false) {
    std::vector<Target> targets;
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        targets = g.targets;
    }

    int rows = static_cast<int>(targets.size());
    if (rows < 1) {
        rows = 1;
    }

    int height = 8 + rows * 24 + 8;
    if (collapsed) {
        return {36, height};
    }

    int textWidth = 72;
    HDC hdc = GetDC(nullptr);
    if (hdc) {
        HGDIOBJ oldFont = nullptr;
        if (g.uiFont) {
            oldFont = SelectObject(hdc, g.uiFont);
        }

        if (targets.empty()) {
            SIZE size = {};
            GetTextExtentPoint32W(hdc, L"未设置 IP", lstrlenW(L"未设置 IP"), &size);
            textWidth = size.cx;
        } else {
            for (const auto& target : targets) {
                SIZE size = {};
                GetTextExtentPoint32W(hdc, target.address.c_str(), static_cast<int>(target.address.size()), &size);
                if (size.cx > textWidth) {
                    textWidth = size.cx;
                }
            }
        }

        if (oldFont) {
            SelectObject(hdc, oldFont);
        }
        ReleaseDC(nullptr, hdc);
    }

    int width = textWidth + 48;
    if (width < 112) {
        width = 112;
    }
    if (width > 320) {
        width = 320;
    }
    return {width, height};
}

RECT GetNearestWorkArea(const RECT& rect) {
    HMONITOR monitor = MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    if (monitor && GetMonitorInfoW(monitor, &info)) {
        return info.rcWork;
    }

    RECT fallback = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &fallback, 0);
    return fallback;
}

void ClampRectToWorkArea(RECT* rect) {
    if (!rect) {
        return;
    }

    RECT work = GetNearestWorkArea(*rect);
    int width = rect->right - rect->left;
    int height = rect->bottom - rect->top;

    if (width > work.right - work.left) {
        rect->left = work.left;
        rect->right = work.right;
    } else {
        if (rect->left < work.left) {
            rect->left = work.left;
            rect->right = rect->left + width;
        }
        if (rect->right > work.right) {
            rect->right = work.right;
            rect->left = rect->right - width;
        }
    }

    if (height > work.bottom - work.top) {
        rect->top = work.top;
        rect->bottom = work.bottom;
    } else {
        if (rect->top < work.top) {
            rect->top = work.top;
            rect->bottom = rect->top + height;
        }
        if (rect->bottom > work.bottom) {
            rect->bottom = work.bottom;
            rect->top = rect->bottom - height;
        }
    }
}

POINT ClampPointToWorkArea(POINT point, SIZE size) {
    RECT rect = {point.x, point.y, point.x + size.cx, point.y + size.cy};
    ClampRectToWorkArea(&rect);
    return {rect.left, rect.top};
}

int GetWorkAreaEdgeFlags(const RECT& rect, const RECT& work) {
    int flags = 0;
    if (rect.left <= work.left) {
        flags |= 1;
    }
    if (rect.right >= work.right) {
        flags |= 2;
    }
    if (rect.top <= work.top) {
        flags |= 4;
    }
    if (rect.bottom >= work.bottom) {
        flags |= 8;
    }
    return flags;
}

void PositionMonitorWindow() {
    if (!g.monitor) {
        return;
    }

    SIZE expandedSize = CalculateMonitorSize(false);
    RECT probe = {g.monitorPos.x, g.monitorPos.y, g.monitorPos.x + expandedSize.cx, g.monitorPos.y + expandedSize.cy};
    if (IsWindowVisible(g.monitor)) {
        GetWindowRect(g.monitor, &probe);
    }

    RECT work = GetNearestWorkArea(probe);
    int edgeFlags = GetWorkAreaEdgeFlags(probe, work);
    bool collapsed = edgeFlags != 0;
    SIZE size = CalculateMonitorSize(collapsed);

    RECT target = {probe.left, probe.top, probe.left + size.cx, probe.top + size.cy};
    if (edgeFlags & 1) {
        target.left = work.left;
        target.right = target.left + size.cx;
    } else if (edgeFlags & 2) {
        target.right = work.right;
        target.left = target.right - size.cx;
    }
    if (edgeFlags & 4) {
        target.top = work.top;
        target.bottom = target.top + size.cy;
    } else if (edgeFlags & 8) {
        target.bottom = work.bottom;
        target.top = target.bottom - size.cy;
    }
    ClampRectToWorkArea(&target);

    g.monitorCollapsed = collapsed;
    g.monitorPos = {target.left, target.top};
    HWND insertAfter = g.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST;
    SetWindowPos(g.monitor, insertAfter, target.left, target.top, target.right - target.left, target.bottom - target.top,
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);
    RedrawWindow(g.monitor, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
}

void RefreshMonitorWindow() {
    if (g.monitor) {
        PositionMonitorWindow();
        RedrawWindow(g.monitor, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    }
}

void WorkerLoop() {
    while (g.running) {
        std::vector<std::wstring> addresses;
        {
            std::lock_guard<std::mutex> lock(g.mutex);
            for (const auto& target : g.targets) {
                addresses.push_back(target.address);
            }
        }

        for (const auto& address : addresses) {
            if (!g.running) {
                break;
            }

            {
                std::lock_guard<std::mutex> lock(g.mutex);
                for (auto& target : g.targets) {
                    if (target.address == address) {
                        target.flashUntil = GetTickCount() + kPingFlashMs;
                        break;
                    }
                }
            }
            if (g.monitor) {
                PostMessageW(g.monitor, WM_PING_UPDATED, 0, 0);
            }

            DWORD rtt = 0;
            PingResult result = PingIpv4(address, g.timeoutMs, &rtt);
            {
                std::lock_guard<std::mutex> lock(g.mutex);
                for (auto& target : g.targets) {
                    if (target.address == address) {
                        bool isOnline = result == PingResult::Online;
                        target.historyBits = ((target.historyBits << 1) | (isOnline ? 1u : 0u)) & kHistoryMask;
                        if (target.historyCount < kHistoryWindow) {
                            ++target.historyCount;
                        }
                        target.timeoutAlert = IsFlapping(target.historyBits, target.historyCount);

                        if (result == PingResult::Online) {
                            target.online = true;
                            target.rtt = rtt;
                        } else {
                            target.online = false;
                            target.rtt = 0;
                        }
                        break;
                    }
                }
            }

            if (g.monitor) {
                PostMessageW(g.monitor, WM_PING_UPDATED, 0, 0);
            }
        }

        DWORD slept = 0;
        while (g.running && slept < g.intervalMs) {
            Sleep(100);
            slept += 100;
        }
    }
}

void StopMonitoring() {
    if (!g.running.exchange(false)) {
        return;
    }
    if (g.worker.joinable()) {
        g.worker.join();
    }
    UpdateButtonState();
    RefreshMonitorWindow();
    SaveConfig();
}

void StartMonitoring() {
    if (g.running) {
        return;
    }

    if (!g.monitor) {
        DWORD exStyle = WS_EX_TOOLWINDOW | (g.alwaysOnTop ? WS_EX_TOPMOST : 0);
        g.monitor = CreateWindowExW(exStyle, kMonitorClass, kWindowTitle,
                                   WS_POPUP, g.monitorPos.x, g.monitorPos.y, 260, 100,
                                   nullptr, nullptr, g.instance, nullptr);
        ApplyRoundedCorners(g.monitor);
    }

    PositionMonitorWindow();
    ShowWindow(g.monitor, SW_SHOWNOACTIVATE);
    UpdateWindow(g.monitor);

    g.running = true;
    g.worker = std::thread(WorkerLoop);
    UpdateButtonState();
}

bool ApplySettingsFromUi() {
    std::wstring text = GetWindowTextString(g.ipEdit);
    auto addresses = SplitAddresses(text);
    if (addresses.empty()) {
        MessageBoxW(g.settings, L"请至少输入一个 IPv4 地址。", kAppName, MB_OK | MB_ICONINFORMATION);
        return false;
    }

    DWORD intervalMs = GetEditDword(g.intervalEdit, 1000, 500, 600000);
    DWORD timeoutMs = GetEditDword(g.timeoutEdit, 2000, 100, 60000);

    {
        std::lock_guard<std::mutex> lock(g.mutex);
        g.targets.clear();
        for (const auto& address : addresses) {
            g.targets.push_back({address});
        }
        g.addressText = text;
    }
    g.intervalMs = intervalMs;
    g.timeoutMs = timeoutMs;

    SaveConfig();
    return true;
}

void FillSettingsUi() {
    std::vector<Target> targets;
    std::wstring addressText;
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        targets = g.targets;
        addressText = g.addressText;
    }

    std::wstring addresses = addressText.empty() ? JoinAddresses(targets) : addressText;
    SetWindowTextW(g.ipEdit, addresses.c_str());

    wchar_t number[32] = {};
    swprintf_s(number, L"%lu", g.intervalMs);
    SetWindowTextW(g.intervalEdit, number);
    swprintf_s(number, L"%lu", g.timeoutMs);
    SetWindowTextW(g.timeoutEdit, number);
}

void ShowSettingsWindow() {
    if (!g.settings) {
        g.settings = CreateWindowExW(0, kSettingsClass, kWindowTitle,
                                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                    CW_USEDEFAULT, CW_USEDEFAULT, 430, 335,
                                    nullptr, nullptr, g.instance, nullptr);
        ApplyRoundedCorners(g.settings);
    }
    FillSettingsUi();
    UpdateButtonState();
    ShowWindow(g.settings, SW_SHOWNORMAL);
    SetForegroundWindow(g.settings);
}

void StopMonitorAndShowSettings() {
    StopMonitoring();
    if (g.monitor) {
        ShowWindow(g.monitor, SW_HIDE);
    }
    ShowSettingsWindow();
}

void ExitApplication() {
    StopMonitoring();
    RemoveTrayIcon();
    PostQuitMessage(0);
}

void ToggleAlwaysOnTop() {
    g.alwaysOnTop = !g.alwaysOnTop;
    RefreshMonitorWindow();
    SaveConfig();
}

void HandleCommand(HWND hwnd, int id) {
    switch (id) {
    case IDC_START_BUTTON:
    case IDM_START:
        if (id == IDC_START_BUTTON && !ApplySettingsFromUi()) {
            return;
        }
        if (id == IDM_START && !g.targets.empty()) {
            SaveConfig();
        }
        if (g.targets.empty()) {
            ShowSettingsWindow();
            return;
        }
        StartMonitoring();
        ShowWindow(g.settings, SW_HIDE);
        UpdateButtonState();
        break;
    case IDC_STOP_BUTTON:
    case IDM_STOP:
        StopMonitoring();
        UpdateButtonState();
        break;
    case IDM_ALWAYS_ON_TOP:
        ToggleAlwaysOnTop();
        break;
    case IDC_HIDE_BUTTON:
        ShowWindow(g.settings, SW_HIDE);
        break;
    case IDM_SHOW_SETTINGS:
        if (g.settings && IsWindowVisible(g.settings)) {
            ShowWindow(g.settings, SW_HIDE);
        } else {
            ShowSettingsWindow();
        }
        break;
    case IDM_EXIT:
        DestroyWindow(hwnd);
        ExitApplication();
        break;
    default:
        break;
    }
}

LRESULT CALLBACK IpEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_GETDLGCODE) {
        LRESULT code = CallWindowProcW(g.ipEditProc, hwnd, msg, wParam, lParam);
        MSG* keyMsg = reinterpret_cast<MSG*>(lParam);
        if (keyMsg && keyMsg->message == WM_KEYDOWN &&
            (keyMsg->wParam == VK_RETURN || (keyMsg->wParam == L'A' && (GetKeyState(VK_CONTROL) & 0x8000)))) {
            return code | DLGC_WANTMESSAGE;
        }
        return code;
    }

    if (msg == WM_KEYDOWN) {
        if (wParam == L'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            SendMessageW(hwnd, EM_SETSEL, 0, -1);
            return 0;
        }
    }

    return CallWindowProcW(g.ipEditProc, hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g.appIcon));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g.smallIcon ? g.smallIcon : g.appIcon));

        HWND subtitle = CreateWindowW(L"STATIC", L"设置 IPv4 地址后开始监控", WS_CHILD | WS_VISIBLE,
                                      22, 16, 260, 22, hwnd, nullptr, g.instance, nullptr);
        SendMessageW(subtitle, WM_SETFONT, reinterpret_cast<WPARAM>(g.smallFont), TRUE);

        HWND label = CreateWindowW(L"STATIC", L"IP 地址", WS_CHILD | WS_VISIBLE,
                                  22, 44, 180, 22, hwnd, nullptr, g.instance, nullptr);
        SetChildFont(label);

        g.ipEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL,
                                  22, 68, 372, 118, hwnd, ControlId(IDC_IP_EDIT), g.instance, nullptr);
        SetControlTheme(g.ipEdit);
        g.ipEditProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g.ipEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(IpEditProc)));

        label = CreateWindowW(L"STATIC", L"检测间隔 ms", WS_CHILD | WS_VISIBLE,
                              22, 204, 100, 22, hwnd, nullptr, g.instance, nullptr);
        SetChildFont(label);
        g.intervalEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"1000",
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
                                        122, 200, 82, 28, hwnd, ControlId(IDC_INTERVAL_EDIT), g.instance, nullptr);
        SetControlTheme(g.intervalEdit);

        label = CreateWindowW(L"STATIC", L"超时 ms", WS_CHILD | WS_VISIBLE,
                              226, 204, 70, 22, hwnd, nullptr, g.instance, nullptr);
        SetChildFont(label);
        g.timeoutEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"2000",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
                                       294, 200, 82, 28, hwnd, ControlId(IDC_TIMEOUT_EDIT), g.instance, nullptr);
        SetControlTheme(g.timeoutEdit);

        g.startButton = CreateWindowW(L"BUTTON", L"开始", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                      154, 248, 72, 30, hwnd, ControlId(IDC_START_BUTTON), g.instance, nullptr);
        SetControlTheme(g.startButton);
        g.stopButton = CreateWindowW(L"BUTTON", L"停止", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                     236, 248, 70, 30, hwnd, ControlId(IDC_STOP_BUTTON), g.instance, nullptr);
        SetControlTheme(g.stopButton);
        g.hideButton = CreateWindowW(L"BUTTON", L"隐藏", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                     316, 248, 72, 30, hwnd, ControlId(IDC_HIDE_BUTTON), g.instance, nullptr);
        SetControlTheme(g.hideButton);

        FillSettingsUi();
        UpdateButtonState();
        AddTrayIcon(hwnd);
        break;
    }
    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, kText);
        return reinterpret_cast<LRESULT>(g.windowBrush);
    }
    case WM_CTLCOLOREDIT: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkMode(hdc, OPAQUE);
        SetBkColor(hdc, kSurfaceBg);
        SetTextColor(hdc, kText);
        return reinterpret_cast<LRESULT>(g.surfaceBrush);
    }
    case WM_COMMAND:
        HandleCommand(hwnd, LOWORD(wParam));
        return 0;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
            ShowTrayMenu(hwnd);
        } else if (lParam == WM_LBUTTONDBLCLK) {
            ShowSettingsWindow();
        }
        return 0;
    case WM_DESTROY:
        if (hwnd == g.settings) {
            g.settings = nullptr;
        }
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

void DrawMonitor(HWND hwnd, HDC hdc) {
    RECT client = {};
    GetClientRect(hwnd, &client);

    HBRUSH background = CreateSolidBrush(kWindowBg);
    FillRect(hdc, &client, background);
    DeleteObject(background);

    HBRUSH surface = CreateSolidBrush(kSurfaceBg);
    HPEN border = CreatePen(PS_SOLID, 1, kBorder);
    HGDIOBJ oldPen = SelectObject(hdc, border);
    HGDIOBJ oldBrush = SelectObject(hdc, surface);
    RoundRect(hdc, client.left, client.top, client.right, client.bottom, 12, 12);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(surface);
    DeleteObject(border);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, kText);
    if (g.uiFont) {
        SelectObject(hdc, g.uiFont);
    }

    std::vector<Target> targets;
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        targets = g.targets;
    }

    if (targets.empty()) {
        RECT textRect = {12, 8, client.right - 12, client.bottom - 8};
        DrawTextW(hdc, L"未设置 IP", -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    int y = 8;
    const bool running = g.running.load();
    const bool collapsed = g.monitorCollapsed;
    const DWORD now = GetTickCount();
    for (const auto& target : targets) {
        COLORREF color = kGray;
        if (running) {
            color = target.timeoutAlert ? kYellow : (target.online ? kGreen : kRed);
        }
        bool flashing = running && target.flashUntil != 0 && static_cast<LONG>(target.flashUntil - now) > 0;
        COLORREF fillColor = flashing ? kFlash : color;
        HBRUSH dot = CreateSolidBrush(fillColor);
        HPEN dotPen = CreatePen(PS_SOLID, flashing ? 2 : 1, color);
        oldPen = SelectObject(hdc, dotPen);
        oldBrush = SelectObject(hdc, dot);
        Ellipse(hdc, 12, y + 7, 24, y + 19);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(dotPen);
        DeleteObject(dot);

        if (!collapsed) {
            RECT textRect = {34, y, client.right - 10, y + 24};
            DrawTextW(hdc, target.address.c_str(), -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        y += 24;
    }
}

void PaintMonitorBuffered(HWND hwnd, HDC paintDc) {
    RECT client = {};
    GetClientRect(hwnd, &client);
    int width = client.right - client.left;
    int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) {
        return;
    }

    HDC memoryDc = CreateCompatibleDC(paintDc);
    HBITMAP bitmap = CreateCompatibleBitmap(paintDc, width, height);
    HGDIOBJ oldBitmap = SelectObject(memoryDc, bitmap);

    DrawMonitor(hwnd, memoryDc);
    BitBlt(paintDc, 0, 0, width, height, memoryDc, 0, 0, SRCCOPY);

    SelectObject(memoryDc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memoryDc);
}

LRESULT CALLBACK MonitorProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps = {};
        HDC hdc = BeginPaint(hwnd, &ps);
        PaintMonitorBuffered(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PING_UPDATED:
        PositionMonitorWindow();
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
        SetTimer(hwnd, TIMER_REPAINT, kPingFlashMs, nullptr);
        return 0;
    case WM_TIMER:
        if (wParam == TIMER_REPAINT) {
            KillTimer(hwnd, TIMER_REPAINT);
            RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
        }
        return 0;
    case WM_LBUTTONDBLCLK:
        StopMonitorAndShowSettings();
        return 0;
    case WM_LBUTTONDOWN:
        ReleaseCapture();
        SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        return 0;
    case WM_MOVING: {
        RECT* rect = reinterpret_cast<RECT*>(lParam);
        ClampRectToWorkArea(rect);
        if (rect) {
            g.monitorPos = {rect->left, rect->top};
            RECT work = GetNearestWorkArea(*rect);
            g.monitorCollapsed = GetWorkAreaEdgeFlags(*rect, work) != 0;
        }
        return TRUE;
    }
    case WM_EXITSIZEMOVE:
        PositionMonitorWindow();
        SaveConfig();
        return 0;
    case WM_MOVE: {
        RECT rect = {};
        GetWindowRect(hwnd, &rect);
        g.monitorPos = {rect.left, rect.top};
        return 0;
    }
    case WM_RBUTTONUP:
    case WM_CONTEXTMENU:
        ShowTrayMenu(g.settings ? g.settings : hwnd);
        return 0;
    case WM_DESTROY:
        if (hwnd == g.monitor) {
            g.monitor = nullptr;
        }
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void CreateUiFont() {
    LOGFONTW lf = {};
    lf.lfHeight = -16;
    lf.lfWeight = FW_NORMAL;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    g.uiFont = CreateFontIndirectW(&lf);

    lf.lfHeight = -14;
    lf.lfWeight = FW_NORMAL;
    g.smallFont = CreateFontIndirectW(&lf);

    g.windowBrush = CreateSolidBrush(kWindowBg);
    g.surfaceBrush = CreateSolidBrush(kSurfaceBg);
}

bool RegisterWindowClasses() {
    WNDCLASSW settings = {};
    settings.lpfnWndProc = SettingsProc;
    settings.hInstance = g.instance;
    settings.hIcon = g.appIcon ? g.appIcon : LoadIconW(nullptr, IDI_APPLICATION);
    settings.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    settings.hbrBackground = g.windowBrush;
    settings.lpszClassName = kSettingsClass;

    WNDCLASSW monitor = {};
    monitor.lpfnWndProc = MonitorProc;
    monitor.hInstance = g.instance;
    monitor.hIcon = g.appIcon ? g.appIcon : LoadIconW(nullptr, IDI_APPLICATION);
    monitor.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    monitor.hbrBackground = g.windowBrush;
    monitor.lpszClassName = kMonitorClass;
    monitor.style = CS_DROPSHADOW | CS_DBLCLKS;

    return RegisterClassW(&settings) && RegisterClassW(&monitor);
}

} // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    g.instance = instance;

    INITCOMMONCONTROLSEX controls = {};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);

    g.appIcon = reinterpret_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
    g.smallIcon = reinterpret_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));

    CreateUiFont();
    LoadConfig();

    if (!RegisterWindowClasses()) {
        MessageBoxW(nullptr, L"窗口类注册失败。", kAppName, MB_OK | MB_ICONERROR);
        return 1;
    }

    ShowSettingsWindow();

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (g.settings && IsDialogMessageW(g.settings, &msg)) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    StopMonitoring();
    RemoveTrayIcon();
    if (g.uiFont) {
        DeleteObject(g.uiFont);
    }
    if (g.smallFont) {
        DeleteObject(g.smallFont);
    }
    if (g.windowBrush) {
        DeleteObject(g.windowBrush);
    }
    if (g.surfaceBrush) {
        DeleteObject(g.surfaceBrush);
    }
    if (g.appIcon) {
        DestroyIcon(g.appIcon);
    }
    if (g.smallIcon) {
        DestroyIcon(g.smallIcon);
    }
    return static_cast<int>(msg.wParam);
}
