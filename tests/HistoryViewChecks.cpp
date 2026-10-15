#include <Windows.h>
#include <CommCtrl.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Inspect lifecycle and row mappings without exposing test APIs in the plugin.
#define private public
#include "../ProcessTrafficPlugin/TrafficDetailWindow.h"
#undef private

namespace
{
DWORD uiThread = GetCurrentThreadId();
HANDLE iconGate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
std::atomic<unsigned> shellCalls{}, uiShellCalls{}, iconsCreated{}, iconsDestroyed{};
bool realIcons = false;

DWORD_PTR WINAPI TestShellIcon(LPCWSTR path, DWORD attributes, SHFILEINFOW* info, UINT size, UINT flags)
{
    if (realIcons) return SHGetFileInfoW(path, attributes, info, size, flags);
    if ((flags & SHGFI_USEFILEATTRIBUTES) == 0)
    {
        ++shellCalls;
        if (GetCurrentThreadId() == uiThread) ++uiShellCalls;
        else WaitForSingleObject(iconGate, 5000);
    }
    info->hIcon = CopyIcon(LoadIconW(nullptr, IDI_APPLICATION));
    if (info->hIcon) ++iconsCreated;
    return info->hIcon != nullptr;
}

BOOL WINAPI TestDestroyIcon(HICON icon)
{
    if (!realIcons && icon) ++iconsDestroyed;
    return DestroyIcon(icon);
}
}

#define SHGetFileInfoW TestShellIcon
#define DestroyIcon TestDestroyIcon
#include "../ProcessTrafficPlugin/TrafficDetailWindow.cpp"
#undef DestroyIcon
#undef SHGetFileInfoW

namespace
{
void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

template<class Predicate>
void PumpUntil(Predicate done, ULONGLONG timeout_ms = 15000)
{
    const auto deadline = GetTickCount64() + timeout_ms;
    while (!done() && GetTickCount64() < deadline)
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(1);
    }
    Check(done(), "Timed out waiting for icon results");
}

HWND CreateHiddenWindow(CTrafficDetailWindow& view)
{
    view.EnsureWindowClassRegistered();
    view.EnsureCommonControlsInitialized();
    HWND hwnd = CreateWindowExW(0, kWindowClassName, L"History view checks", WS_OVERLAPPEDWINDOW,
        0, 0, 900, 740, nullptr, nullptr, GetModuleHandleW(nullptr), &view);
    Check(hwnd != nullptr, "Could not create hidden test window");
    KillTimer(hwnd, kRefreshTimerId);
    return hwnd;
}

int RowIcon(HWND list, int row)
{
    LVITEMW item{};
    item.mask = LVIF_IMAGE;
    item.iItem = row;
    Check(ListView_GetItem(list, &item) != FALSE, "Missing test row");
    return item.iImage;
}

void CheckAsyncIcons(CProcNetPlugin& plugin)
{
    CTrafficDetailWindow view(plugin);
    HWND hwnd = CreateHiddenWindow(view);
    view.EnsureListItemCount(2);
    view.m_rowIconKeys = { L"second.exe", L"first.exe" };
    const auto before = shellCalls.load();
    for (int i = 0; i < 5000; ++i)
        Check(view.GetIconIndex(L"PID" + std::to_wstring(i), L"") == view.m_defaultIconIndex,
            "Anonymous PID must use the default icon");
    Check(shellCalls == before && view.m_iconIndexByKey.empty(), "PID lookup must not queue shell work");
    view.GetIconIndex(L"first.exe", L"first.exe");
    view.GetIconIndex(L"first.exe", L"first.exe");
    view.GetIconIndex(L"second.exe", L"second.exe");
    Check(uiShellCalls == 0, "Shell icon extraction ran on the UI thread");
    SetEvent(iconGate);
    PumpUntil([&] {
        return view.m_iconIndexByKey.at(L"first.exe") != view.m_defaultIconIndex &&
            view.m_iconIndexByKey.at(L"second.exe") != view.m_defaultIconIndex;
    });
    Check(shellCalls - before == 2, "Duplicate icon requests were not coalesced");
    Check(RowIcon(view.m_list, 0) == view.m_iconIndexByKey.at(L"second.exe") &&
        RowIcon(view.m_list, 1) == view.m_iconIndexByKey.at(L"first.exe"), "Sorted rows received wrong icons");

    ResetEvent(iconGate);
    view.GetIconIndex(L"late.exe", L"late.exe");
    view.EnsureListItemCount(1);
    view.m_rowIconKeys = { L"first.exe" };
    SetEvent(iconGate);
    PumpUntil([&] { return view.m_iconIndexByKey.at(L"late.exe") != view.m_defaultIconIndex; });
    Check(RowIcon(view.m_list, 0) == view.m_iconIndexByKey.at(L"first.exe"), "Stale result changed a replaced row");

    view.ApplyQuickRange(kYearRangeButtonId);
    const auto selected = view.GetSelectedRange();
    const auto preferred = plugin.GetPreferredRange();
    Check(selected.start.wMonth == 1 && selected.start.wDay == 1 && selected.start.wHour == 0 &&
        selected.start.wMinute == 0 && preferred.start.wMonth == 1 && preferred.start.wDay == 1 &&
        preferred.end.wDay == selected.end.wDay, "Picker notifications overwrote a quick range");

    ResetEvent(iconGate);
    for (int i = 0; i < 100; ++i)
        view.GetIconIndex(L"closing" + std::to_wstring(i) + L".exe", L"test.exe" + std::to_wstring(i));
    SetEvent(iconGate);
    DestroyWindow(hwnd);
    Check(!view.m_iconWorker.joinable() && view.m_iconResults.empty() && view.m_iconRequests.empty(),
        "Window destruction left icon work running");
    Check(iconsCreated == iconsDestroyed, "Owned HICON handles leaked");
    hwnd = CreateHiddenWindow(view);
    view.GetIconIndex(L"reopened.exe", L"reopened.exe");
    PumpUntil([&] { return view.m_iconIndexByKey.at(L"reopened.exe") != view.m_defaultIconIndex; });
    DestroyWindow(hwnd);
    Check(iconsCreated == iconsDestroyed, "Reopening leaked icon handles");
    std::cout << "PASS: nonblocking icons, PID skip, deduplication, stale rows, quick ranges, close/reopen, HICON ownership" << std::endl;
}

void CheckSummaries(const std::filesystem::path& fixture)
{
    const auto root = fixture / L"app_traffic_history";
    std::filesystem::create_directories(root);
    std::ofstream(root / L"apps.tsv") << "version\t4\napp\t1\tcheck.exe\n";
    const int daysInMonth[] = { 31, 29, 31 };
    for (int month = 1; month <= 3; ++month)
    {
        const auto directory = root / L"2024" / (L"0" + std::to_wstring(month));
        std::filesystem::create_directories(directory);
        for (int day = 1; day <= daysInMonth[month - 1]; ++day)
        {
            wchar_t name[32]{};
            swprintf_s(name, L"2024-%02d-%02d.tsv", month, day);
            std::ofstream(directory / name) << "12:00\t1\ta\tb\n";
        }
    }
    // A year containing summaries only must still participate in quarter rebuilding.
    for (int month = 10; month <= 12; ++month)
    {
        const auto directory = root / L"2023" / std::to_wstring(month);
        std::filesystem::create_directories(directory);
        std::ofstream output(directory / (L"2023-" + std::to_wstring(month) + L".month.tsv"));
        output << "version\t1\ntype\tmonth\nstart\t2023-" << month << "-01\nend\t2023-" << month
            << (month == 11 ? "-30" : "-31") << "\ncomplete\t1\nsource_count\t1\nsource_generation\t1\n"
            << "app\t1\ta\tb\ntotal\ta\tb\n";
    }
    CHistoryTrafficStore store;
    store.Initialize(fixture.wstring());
    CHistoryTrafficStore::DateTimeRange quarter{};
    quarter.start.wYear = quarter.end.wYear = 2024;
    quarter.start.wMonth = 1;
    quarter.start.wDay = 1;
    quarter.end.wMonth = 3;
    quarter.end.wDay = 31;
    quarter.end.wHour = 23;
    quarter.end.wMinute = 59;
    const auto checkTotal = [&](const CHistoryTrafficStore::DateTimeRange& range, unsigned days) {
        const auto apps = store.GetRangeAppTotals(range);
        Check(apps.size() == 1 && apps[0].appName == L"check.exe" && apps[0].rxTotalBytes == 10ULL * days &&
            apps[0].txTotalBytes == 11ULL * days, "Summary query changed traffic totals");
    };
    checkTotal(quarter, 91);
    Check(std::filesystem::exists(root / L"2024" / L"2024-Q1.quarter.tsv"), "Quarter summary was not generated");
    auto older = quarter;
    older.start.wYear = older.end.wYear = 2023;
    older.start.wMonth = 10;
    older.end.wMonth = 12;
    checkTotal(older, 3);
    Check(std::filesystem::exists(root / L"2023" / L"2023-Q4.quarter.tsv"), "Summary-only year was skipped");

    auto day = quarter;
    day.end.wMonth = 1;
    day.end.wDay = 1;
    std::ofstream(root / L"2024" / L"2024-Q1.quarter.tsv", std::ios::trunc) << "corrupted\n";
    checkTotal(day, 1);
    checkTotal(quarter, 91);
    std::ofstream(root / L"2024" / L"01" / L"2024-01-01.day.tsv", std::ios::trunc) << "corrupted\n";
    checkTotal(day, 1);
    std::cout << "PASS: leap-quarter totals, summary-only years, corrupt-quarter fallback, corrupt-day fallback" << std::endl;
}

void CaptureWindow(HWND hwnd, const std::filesystem::path& path)
{
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_FRAME);
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    RECT bounds{};
    GetWindowRect(hwnd, &bounds);
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ old = SelectObject(memory, bitmap);
    Check(PrintWindow(hwnd, memory, 0) != FALSE, "Could not render test window");
    SelectObject(memory, old);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    Check(GetDIBits(memory, bitmap, 0, height, pixels.data(), &info, DIB_RGB_COLORS) != 0, "Could not capture test pixels");
    std::unordered_set<unsigned> colors;
    for (size_t i = 0; i < pixels.size(); i += 4)
        colors.insert(static_cast<unsigned>(pixels[i]) | static_cast<unsigned>(pixels[i + 1]) << 8 | static_cast<unsigned>(pixels[i + 2]) << 16);
    Check(colors.size() >= 8, "Window capture is blank");
    BITMAPFILEHEADER header{};
    header.bfType = 0x4d42;
    header.bfOffBits = sizeof(header) + sizeof(info.bmiHeader);
    header.bfSize = header.bfOffBits + static_cast<DWORD>(pixels.size());
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
    output.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
}

void CheckNetworkView(CProcNetPlugin& plugin, const std::filesystem::path& fixture)
{
    plugin.m_networkStore.Initialize(fixture.wstring());
    std::vector<CHistoryTrafficStore::AppTotalEntry> applications;
    for (int i = 0; i < 40; ++i)
        applications.push_back({ L"test-" + std::to_wstring(i) + L".exe", L"C:\\Windows\\System32\\notepad.exe",
            static_cast<std::uint64_t>(i + 1) * 8192, static_cast<std::uint64_t>(i + 1) * 1024 });
    plugin.m_historyStore.Update(applications);
    const auto now = CNetworkTrafficCollector::UtcNowMs();
    CNetworkTrafficCollector::Adapter adapter{ L"A", L"Ethernet (test adapter)", 1, 100000, 200000, true, true };
    plugin.m_networkCollector.Observe({ adapter }, 1000, now - 1000);
    adapter.rxBytes += 8192; adapter.txBytes += 1024;
    const auto interval = plugin.m_networkCollector.Observe({ adapter }, 2000, now);
    Check(interval && plugin.m_networkStore.Add(*interval), "Cannot seed independent network samples");
    CTrafficDetailWindow view(plugin);
    HWND hwnd = CreateHiddenWindow(view);
    view.ApplyQuickRange(kDayRangeButtonId);
    view.ToggleView();
    Check(Button_GetCheck(view.m_followRangeCheck) == BST_CHECKED, "Quick range did not follow current time");
    for (const auto language : { CHistoryTrafficStore::DisplayLanguage::Chinese, CHistoryTrafficStore::DisplayLanguage::English })
    {
        ComboBox_SetCurSel(view.m_languageCombo, static_cast<int>(language));
        plugin.SetPreferredLanguage(language);
        view.RefreshView();
        const auto summary = plugin.BuildTotalsText(view.GetSelectedRange(), language);
        Check(summary.find(language == CHistoryTrafficStore::DisplayLanguage::English ? L"Network range" : L"\u7f51\u5361\u533a\u95f4") != std::wstring::npos &&
            summary.find(language == CHistoryTrafficStore::DisplayLanguage::English ? L"Process range" : L"\u8fdb\u7a0b\u533a\u95f4") != std::wstring::npos,
            "Network and process totals are not distinguished");
        for (const int width : { 900, 1200 })
        {
            SetWindowPos(hwnd, nullptr, 0, 0, width, 740, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            RECT client{};
            GetClientRect(hwnd, &client);
            RECT previous{};
            for (const auto control : { view.m_summary, view.m_startLabel, view.m_startDatePicker, view.m_startTimePicker, view.m_dayRangeButton })
            {
                RECT bounds{};
                GetWindowRect(control, &bounds);
                MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&bounds), 2);
                Check(bounds.left >= 0 && bounds.right <= client.right && bounds.top >= 0 && bounds.bottom <= client.bottom,
                    "A control extends outside the window");
                Check(previous.right <= bounds.left, "Summary/date/quick-range controls overlap");
                previous = bounds;
            }
            HDC dc = GetDC(view.m_summary);
            HGDIOBJ font = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(view.m_summary, WM_GETFONT, 0, 0)));
            TEXTMETRICW metrics{};
            GetTextMetricsW(dc, &metrics);
            RECT area{};
            SendMessageW(view.m_summary, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&area));
            const auto lines = SendMessageW(view.m_summary, EM_GETLINECOUNT, 0, 0);
            Check(lines * metrics.tmHeight <= area.bottom - area.top, "Summary text is clipped vertically");
            SelectObject(dc, font);
            ReleaseDC(view.m_summary, dc);
        }
        SetWindowPos(hwnd, nullptr, 0, 0, 900, 740, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        CaptureWindow(hwnd, std::filesystem::path(L"build") /
            (language == CHistoryTrafficStore::DisplayLanguage::English ? L"network-history-en.bmp" : L"network-history-zh.bmp"));
    }
    SYSTEMTIME selected{};
    DateTime_GetSystemtime(view.m_endTimePicker, &selected);
    selected.wMinute = static_cast<WORD>((selected.wMinute + 59) % 60);
    DateTime_SetSystemtime(view.m_endTimePicker, GDT_VALID, &selected);
    NMHDR changed{ view.m_endTimePicker, kEndTimePickerId, DTN_DATETIMECHANGE };
    SendMessageW(hwnd, WM_NOTIFY, kEndTimePickerId, reinterpret_cast<LPARAM>(&changed));
    Check(Button_GetCheck(view.m_followRangeCheck) == BST_UNCHECKED, "Editing time did not stop following");
    const auto manual = view.GetSelectedRange();
    view.HandleRefreshTimer(kRefreshTimerId);
    Check(view.GetSelectedRange().end.wMinute == manual.end.wMinute, "Timer changed a fixed history range");
    Button_SetCheck(view.m_followRangeCheck, BST_CHECKED);
    SetFocus(nullptr);
    view.HandleRefreshTimer(kRefreshTimerId);
    Check(ListView_GetItemCount(view.m_list) >= 31, "Following did not restore current-minute process rows");
    ListView_EnsureVisible(view.m_list, 30, FALSE);
    ListView_SetItemState(view.m_list, 30, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    const auto top = ListView_GetTopIndex(view.m_list);
    view.HandleRefreshTimer(kRefreshTimerId);
    SYSTEMTIME current{};
    GetLocalTime(&current);
    Check(view.GetSelectedRange().end.wMinute == current.wMinute, "Following did not advance the end time");
    Check(ListView_GetTopIndex(view.m_list) == top && ListView_GetNextItem(view.m_list, -1, LVNI_SELECTED) == 30,
        "Live history refresh lost scrolling or selection");
    view.ToggleView();
    CaptureWindow(hwnd, L"build/network-realtime-en.bmp");
    Check(!plugin.SetNetworkAdapter(L"bad\tidentifier"), "Invalid adapter identifier was accepted");
    Check(plugin.SetNetworkAdapter(L"A") && plugin.GetNetworkSnapshot().preferredId == L"A", "Fixed adapter selection did not persist");
    Check(plugin.SetNetworkAdapter(L"") && plugin.GetNetworkSnapshot().preferredId.empty(), "Automatic adapter selection did not persist");
    DestroyWindow(hwnd);
    std::cout << "PASS: bilingual network/process totals, 900/1200 layouts, text fit, adapter preferences, live range, fixed range, rendered screenshots" << std::endl;
}

void CheckUnicodeState(const std::filesystem::path& fixture)
{
    CHistoryTrafficStore store;
    store.Initialize(fixture.wstring());
    SYSTEMTIME now{};
    GetLocalTime(&now);
    CHistoryTrafficStore::DateTimeRange range{ now, now };
    store.SetPreferredRange(range);
    store.SetPreferredLanguage(CHistoryTrafficStore::DisplayLanguage::Chinese);
    CHistoryTrafficStore::AppTotalEntry app{ L"\u6d4b\u8bd5.exe", L"C:\\\u7a0b\u5e8f\\\u6d4b\u8bd5.exe", 123, 456 };
    store.Update({ app });
    CHistoryTrafficStore reopened;
    reopened.Initialize(fixture.wstring());
    const auto apps = reopened.GetRangeAppTotals(range);
    Check(reopened.GetPreferredLanguage() == CHistoryTrafficStore::DisplayLanguage::Chinese && apps.size() == 1 &&
        apps[0].appName == app.appName && apps[0].exePath == app.exePath && apps[0].rxTotalBytes == 123 && apps[0].txTotalBytes == 456,
        "UTF-8 state/checkpoint round trip changed process names, paths or totals");
    std::cout << "PASS: UTF-8 process state/checkpoint and byte totals after parser cleanup" << std::endl;
}
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        auto& plugin = CProcNetPlugin::Instance();
        if (argc == 2)
        {
            realIcons = true;
            plugin.m_historyStore.Initialize(argv[1]);
            auto init = GetTickCount64();
            plugin.GetPreferredRange();
            std::cout << "initialize_ms=" << GetTickCount64() - init << std::endl;
            CTrafficDetailWindow view(plugin);
            HWND hwnd = CreateHiddenWindow(view);
            const auto start = GetTickCount64();
            view.ToggleView();
            std::cout << "history_switch_ms=" << GetTickCount64() - start << " visible_rows="
                << ListView_GetItemCount(view.m_list) << std::endl;
            PumpUntil([&] {
                std::lock_guard<std::mutex> lock(view.m_iconMutex);
                return view.m_iconRequests.empty() && view.m_iconResults.empty();
            }, 60000);
            std::cout << "background_icons_ms=" << GetTickCount64() - start << std::endl;
            DestroyWindow(hwnd);
        }
        else
        {
            const auto fixture = std::filesystem::path(L"build") / (L"history-view-checks-" + std::to_wstring(GetTickCount64()));
            plugin.m_historyStore.Initialize(fixture.wstring());
            CheckAsyncIcons(plugin);
            CheckSummaries(fixture / L"summaries");
            CheckNetworkView(plugin, fixture);
            CheckUnicodeState(fixture / L"unicode");
        }
        CloseHandle(iconGate);
        return 0;
    }
    catch (const std::exception& error)
    {
        SetEvent(iconGate);
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
