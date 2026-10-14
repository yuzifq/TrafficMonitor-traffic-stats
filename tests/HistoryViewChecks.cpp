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
void PumpUntil(Predicate done)
{
    const auto deadline = GetTickCount64() + 15000;
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
        0, 0, 800, 650, nullptr, nullptr, GetModuleHandleW(nullptr), &view);
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
            });
            DestroyWindow(hwnd);
        }
        else
        {
            const auto fixture = std::filesystem::path(L"build") / (L"history-view-checks-" + std::to_wstring(GetTickCount64()));
            plugin.m_historyStore.Initialize(fixture.wstring());
            CheckAsyncIcons(plugin);
            CheckSummaries(fixture / L"summaries");
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
