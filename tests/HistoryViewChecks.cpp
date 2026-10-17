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
#include <exception>
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

void CheckSummaryBorder(CTrafficDetailWindow& view)
{
    RECT area{};
    GetClientRect(view.m_summary, &area);
    const auto dc = GetDC(view.m_summary);
    const auto memory = CreateCompatibleDC(dc);
    const auto bitmap = CreateCompatibleBitmap(dc, area.right, area.bottom);
    Check(memory && bitmap, "Cannot create summary border test surface");
    const auto old_bitmap = SelectObject(memory, bitmap);
    DRAWITEMSTRUCT item{};
    item.hDC = memory;
    item.rcItem = area;
    view.HandleMessage(view.m_hwnd, WM_DRAWITEM, kSummaryId, reinterpret_cast<LPARAM>(&item));
    bool valid = true;
    const auto border = GetSysColor(COLOR_3DSHADOW);
    const auto background = GetSysColor(COLOR_WINDOW);
    for (int x = 0; x < area.right; ++x)
    {
        valid &= GetPixel(memory, x, 0) == border && GetPixel(memory, x, area.bottom - 1) == border;
        if (x > 0 && x < area.right - 1)
            valid &= GetPixel(memory, x, 1) == background && GetPixel(memory, x, area.bottom - 2) == background;
    }
    for (int y = 1; y < area.bottom - 1; ++y)
    {
        valid &= GetPixel(memory, 0, y) == border && GetPixel(memory, area.right - 1, y) == border &&
            GetPixel(memory, 1, y) == background && GetPixel(memory, area.right - 2, y) == background;
    }
    SelectObject(memory, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(view.m_summary, dc);
    Check(valid, "Summary border is not a flat, one-pixel system control outline");
}

void CheckCompactSummary(CTrafficDetailWindow& view)
{
    HDC dc = GetDC(view.m_summary);
    const auto old_font = SelectObject(dc, view.m_font);
    const auto language = view.GetSelectedLanguage();
    RECT area{};
    GetClientRect(view.m_summary, &area);
    const auto layout = SummaryLayout(area, dc, view.m_lineHeight, language);
    Check((GetWindowLongW(view.m_summary, GWL_EXSTYLE) & (WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_DLGMODALFRAME)) == 0 &&
        (GetWindowLongW(view.m_summary, GWL_STYLE) & WS_BORDER) == 0,
        "The summary still has a native inset or thick border");
    Check(area.bottom < (std::max)(216, view.m_lineHeight * 10 + 56),
        "The summary did not return vertical space to the list");
    RECT list{}, summary{};
    GetWindowRect(view.m_list, &list); GetWindowRect(view.m_summary, &summary);
    Check(list.bottom + kSummaryGap == summary.top, "The list/summary separation changed unexpectedly");
    const int values_left = layout.left + layout.labelWidth, values_width = layout.right - values_left;
    RECT value_cell{ values_left + 2, layout.top + layout.rowHeight * 2,
        values_left + values_width / 4 - 2, layout.top + layout.rowHeight * 2 + layout.dataHeight };
    RECT short_value = value_cell;
    DrawTextW(dc, L"1024.00 MB", -1, &short_value, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    if (short_value.right <= value_cell.right)
        Check(layout.dataHeight <= view.m_lineHeight * 2 + 4, "Single-line values reserve excessive blank space");
    for (const auto value : { 1048575ULL, 1073741823ULL, 1099511627775ULL, 1ULL << 50, UINT64_MAX })
    {
        const auto text = Utils::FormatBytes(value);
        auto measured = value_cell;
        DrawTextW(dc, text.c_str(), -1, &measured, DT_CALCRECT | DT_WORDBREAK | DT_CENTER | DT_NOPREFIX);
        Check(measured.right <= value_cell.right && measured.bottom <= value_cell.bottom,
            "A formatted byte value does not fit a table cell");
    }
    const auto total = [](const CHistoryTrafficStore::TrafficAmount& amount) { return Utils::FormatBytes(amount.rxBytes + amount.txBytes); };
    const std::wstring texts[] = {
        GetLocalizedText(language, { L"Process range total: ", L"\u8fdb\u7a0b\u533a\u95f4\u603b\u91cf: " }) + total(view.m_totals.processRange),
        GetLocalizedText(language, { L"All-time total: ", L"\u7d2f\u8ba1\u603b\u91cf: " }) + total(view.m_totals.processAll),
        GetLocalizedText(language, { L"Network range total: ", L"\u7f51\u5361\u533a\u95f4\u603b\u91cf: " }) + total(view.m_totals.networkRange),
        GetLocalizedText(language, { L"All-time total: ", L"\u7d2f\u8ba1\u603b\u91cf: " }) + total(view.m_totals.networkAll)
    };
    for (int i = 0; i < 4; ++i)
    {
        const int half = (layout.right - layout.left) / 2;
        const int top = layout.top + layout.rowHeight * 2 + layout.dataHeight * (2 + i / 2) + kTotalsGap;
        const RECT cell{ layout.left + (i % 2) * half, top, layout.left + (i % 2 + 1) * half - 4,
            top + layout.dataHeight };
        auto measured = cell;
        DrawTextW(dc, texts[i].c_str(), -1, &measured, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        Check(measured.bottom <= cell.bottom && cell.bottom <= area.bottom,
            "A compact total wraps beyond its available height");
    }
    SelectObject(dc, old_font);
    ReleaseDC(view.m_summary, dc);
    CheckSummaryBorder(view);
}

std::wstring SummaryContents(HWND hwnd)
{
    GETTEXTLENGTHEX length{ GTL_PRECISE | GTL_NUMCHARS, 1200 };
    std::wstring text(static_cast<size_t>(SendMessageW(hwnd, EM_GETTEXTLENGTHEX,
        reinterpret_cast<WPARAM>(&length), 0)) + 1, L'\0');
    GETTEXTEX read{};
    read.cb = static_cast<DWORD>(text.size() * sizeof(wchar_t)); read.flags = GT_RAWTEXT; read.codepage = 1200;
    text.resize(static_cast<size_t>(SendMessageW(hwnd, EM_GETTEXTEX, reinterpret_cast<WPARAM>(&read),
        reinterpret_cast<LPARAM>(text.data()))));
    return text;
}

void CheckSelectableSummary(CTrafficDetailWindow& view)
{
    const auto hwnd = view.m_summaryContent;
    Check(hwnd && (GetWindowLongW(hwnd, GWL_STYLE) & ES_READONLY), "Summary text is not a native read-only control");
    const auto text = SummaryContents(hwnd);
    const auto language = view.GetSelectedLanguage();
    const auto network = text.find(GetLocalizedText(language, { L"Network", L"网卡" }));
    const auto process = text.find(GetLocalizedText(language, { L"Process", L"进程" }));
    const auto process_total = text.find(GetLocalizedText(language, { L"Process range total:", L"进程区间总量:" }));
    const auto network_total = text.find(GetLocalizedText(language, { L"Network range total:", L"网卡区间总量:" }));
    Check(network < process && process < process_total && process_total < network_total && network_total < text.size(),
        "Selectable summary lost text or reordered the footer incorrectly");
    const auto position = [&](size_t at) {
        POINT point{};
        SendMessageW(hwnd, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&point), static_cast<LPARAM>(at));
        return point;
    };
    const auto network_point = position(network), process_point = position(process);
    const auto process_total_point = position(process_total), network_total_point = position(network_total);
    Check(std::abs((network_total_point.y - process_total_point.y) - (process_point.y - network_point.y)) <= 1,
        "Native footer line spacing differs from the network/process rows");
    RECT area{};
    GetClientRect(hwnd, &area);
    if (network_total_point.y < 0 || network_total_point.y + view.m_lineHeight > area.bottom)
    {
        CaptureWindow(view.m_hwnd, L"build/summary-copy-debug.bmp");
        std::wcerr << L"summary height=" << area.bottom << L" footer=" << network_total_point.y
            << L" line=" << view.m_lineHeight << L" network=" << network_point.y << L" process=" << process_point.y
            << L" processTotal=" << process_total_point.y << L" text=" << text << std::endl;
    }
    Check(network_total_point.y >= 0 && network_total_point.y + view.m_lineHeight <= area.bottom,
        "Native summary footer is clipped below the panel");
    const auto end = process_total + 8;
    SendMessageW(hwnd, EM_SETSEL, process_total, static_cast<LPARAM>(end));
    wchar_t selected[32]{};
    Check(SendMessageW(hwnd, EM_GETSELTEXT, 0, reinterpret_cast<LPARAM>(selected)) == 8 &&
        std::wstring(selected) == text.substr(process_total, 8), "Selected summary text cannot be read/copied exactly");
    const auto totals = view.m_totals;
    view.m_totals.networkRange.rxBytes += 1024;
    view.UpdateSummaryContent();
    Check(SummaryContents(hwnd) == text, "A live update replaced text during selection");
    SendMessageW(hwnd, WM_CHAR, L'x', 0);
    Check(SummaryContents(hwnd) == text, "The read-only summary accepted edits");
    SendMessageW(hwnd, WM_KILLFOCUS, 0, 0);
    view.UpdateSummaryContent();
    Check(SummaryContents(hwnd) != text, "Summary did not resume updating after selection lost focus");
    view.m_totals = totals;
    view.UpdateSummaryContent();
}

HWND detailsParent{};
std::exception_ptr detailsError;
bool detailsInspected{};
bool detailsDiagnostic{};
ULONGLONG detailsDeadline{};
std::wstring detailsExpected;

void CALLBACK InspectDetails(HWND, UINT, UINT_PTR timer, DWORD)
{
    const auto dialog = GetLastActivePopup(detailsParent);
    if (dialog == detailsParent) return;
    const auto edit = GetDlgItem(dialog, kDetailsTextId);
    if (!edit && GetTickCount64() < detailsDeadline) return;
    KillTimer(nullptr, timer);
    try
    {
        Check(edit && (GetWindowLongW(edit, GWL_STYLE) & ES_READONLY) &&
            (GetWindowLongW(edit, GWL_STYLE) & WS_VSCROLL), "Details is not a scrollable read-only view");
        std::wstring text(GetWindowTextLengthW(edit) + 1, L'\0');
        GetWindowTextW(edit, text.data(), static_cast<int>(text.size()));
        text.resize(wcslen(text.c_str()));
        Check(text == detailsExpected, "Details did not preserve every original summary field");
        SetWindowPos(dialog, nullptr, 0, 0, 500, 320, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        RECT edit_rect{}, button_rect{}, area{};
        GetWindowRect(edit, &edit_rect); GetWindowRect(GetDlgItem(dialog, IDOK), &button_rect);
        GetClientRect(dialog, &area);
        MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&edit_rect), 2);
        MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&button_rect), 2);
        Check(edit_rect.bottom < button_rect.top && button_rect.right <= area.right && button_rect.bottom <= area.bottom,
            "Details controls overlap after resize");
        CaptureWindow(dialog, std::filesystem::path(L"build") / (detailsDiagnostic ?
            (detailsExpected.find(L"Ledger:") == std::wstring::npos ? L"traffic-diagnostics-zh.bmp" : L"traffic-diagnostics-en.bmp") :
            (detailsExpected.find(L"Start:") == std::wstring::npos ? L"traffic-details-zh.bmp" : L"traffic-details-en.bmp")));
        detailsInspected = true;
    }
    catch (...) { detailsError = std::current_exception(); }
    PostMessageW(dialog, WM_COMMAND, IDOK, 0);
}

void CheckDetails(CTrafficDetailWindow& view, bool diagnostics = false)
{
    detailsParent = view.m_hwnd;
    detailsDiagnostic = diagnostics;
    detailsExpected = diagnostics ? view.m_plugin.BuildDiagnosticsText(view.GetSelectedLanguage()) : view.BuildDetailsText();
    detailsDeadline = GetTickCount64() + 5000;
    detailsError = nullptr;
    detailsInspected = false;
    const auto timer = SetTimer(nullptr, 0, 10, InspectDetails);
    Check(timer != 0, "Cannot inspect modal details dialog");
    view.HandleCommand(diagnostics ? kDiagnosticsId : kDetailsId, BN_CLICKED);
    KillTimer(nullptr, timer);
    if (detailsError) std::rethrow_exception(detailsError);
    Check(detailsInspected && IsWindowEnabled(view.m_hwnd), "Details did not close and restore the parent window");
}

void CheckNetworkView(CProcNetPlugin& plugin, const std::filesystem::path& fixture)
{
    plugin.m_networkStore.Initialize(fixture.wstring());
    std::vector<CHistoryTrafficStore::AppTotalEntry> applications;
    for (int i = 0; i < 40; ++i)
        applications.push_back({ L"test-" + std::to_wstring(i) + L".exe", L"C:\\Windows\\System32\\notepad.exe",
            static_cast<std::uint64_t>(i + 1) * 8192, static_cast<std::uint64_t>(i + 1) * 1024 });
    plugin.m_historyStore.Update(applications);
    // A historical baseline exercises TB display without inflating today's rows.
    plugin.m_historyStore.GetAllTimeTotal();
    plugin.m_historyStore.m_cachedAllTimeTotal = { 3ULL << 40, 2ULL << 40 };
    plugin.m_historyStore.m_allTimeCacheValid = true;
    const auto now = CNetworkTrafficCollector::UtcNowMs();
    Check(plugin.m_networkStore.Add({ now - 86401000, now - 86400000, 3ULL << 30, 2ULL << 30, L"A", false }),
        "Cannot seed an older network cumulative baseline");
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
        for (const auto size : { std::pair{ 900, 680 }, std::pair{ 900, 740 }, std::pair{ 1200, 740 } })
        {
            SetWindowPos(hwnd, nullptr, 0, 0, size.first, size.second, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
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
            const auto details = view.BuildDetailsText();
            Check(details.find(summary) != std::wstring::npos &&
                details.find(language == CHistoryTrafficStore::DisplayLanguage::English ? L"Start:" : L"\u5f00\u59cb\u65f6\u95f4:") != std::wstring::npos,
                "Details lost the original summary or selected time range");
            Check(view.m_totals.networkRange.rxBytes == 8192 && view.m_totals.networkRange.txBytes == 1024 &&
                view.m_totals.processRange.rxBytes == plugin.m_historyStore.GetRangeTotal(view.GetSelectedRange()).rxBytes,
                "Compact cells differ from the original source totals");
            RECT details_button{}, export_button{}, follow{}, quick{};
            GetWindowRect(view.m_detailsButton, &details_button);
            GetWindowRect(view.m_exportButton, &export_button);
            GetWindowRect(view.m_followRangeCheck, &follow);
            GetWindowRect(view.m_yearRangeButton, &quick);
            Check(details_button.top == export_button.top && details_button.right < export_button.left &&
                export_button.bottom > quick.bottom && details_button.top > follow.bottom,
                "Details/export are not separated at the bottom of the controls");
            CheckCompactSummary(view);
            CheckSelectableSummary(view);
        }
        SetWindowPos(hwnd, nullptr, 0, 0, 900, 740, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        CaptureWindow(hwnd, std::filesystem::path(L"build") /
            (language == CHistoryTrafficStore::DisplayLanguage::English ? L"network-history-en.bmp" : L"network-history-zh.bmp"));
        CheckDetails(view);
        CheckDetails(view, true);

        const auto original_font = view.m_font;
        const auto original_height = view.m_lineHeight;
        view.m_font = CreateFontW(-20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        Check(view.m_font != nullptr, "Cannot create larger text test font");
        HDC dc = GetDC(hwnd);
        const auto previous = SelectObject(dc, view.m_font);
        TEXTMETRICW metrics{};
        GetTextMetricsW(dc, &metrics);
        view.m_lineHeight = metrics.tmHeight;
        SelectObject(dc, previous); ReleaseDC(hwnd, dc);
        view.SetAllControlFonts();
        view.ResizeChildren(884, 641);
        CheckCompactSummary(view);
        CheckSelectableSummary(view);
        CaptureWindow(hwnd, std::filesystem::path(L"build") /
            (language == CHistoryTrafficStore::DisplayLanguage::English ? L"network-history-large-en.bmp" : L"network-history-large-zh.bmp"));
        const auto large_font = view.m_font;
        view.m_font = original_font; view.m_lineHeight = original_height;
        view.SetAllControlFonts();
        DeleteObject(large_font);
        RECT restored{}; GetClientRect(hwnd, &restored);
        view.ResizeChildren(restored.right, restored.bottom);
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
    CheckSummaryBorder(view);
    CaptureWindow(hwnd, L"build/network-realtime-en.bmp");
    Check(!plugin.SetNetworkAdapter(L"bad\tidentifier"), "Invalid adapter identifier was accepted");
    Check(plugin.SetNetworkAdapter(L"A") && plugin.GetNetworkSnapshot().preferredId == L"A", "Fixed adapter selection did not persist");
    Check(plugin.SetNetworkAdapter(L"") && plugin.GetNetworkSnapshot().preferredId.empty(), "Automatic adapter selection did not persist");
    DestroyWindow(hwnd);
    std::cout << "PASS: bilingual footer order/spacing, exact native selection ranges, read-only text, stable selection during refresh, compact layouts, larger fonts and rendered screenshots" << std::endl;
}
void CheckSorting(CProcNetPlugin& plugin)
{
    CTrafficDetailWindow view(plugin);
    std::vector<CProcNetPlugin::AppTrafficEntry> entries(3);
    entries[0].exeName = L"z.exe"; entries[0].rxTotalBytes = 90; entries[0].txTotalBytes = 10;
    entries[1].exeName = L"a.exe"; entries[1].rxTotalBytes = 20; entries[1].txTotalBytes = 100;
    entries[2].exeName = L"b.exe"; entries[2].rxTotalBytes = 10; entries[2].txTotalBytes = 20;
    view.m_viewMode = CTrafficDetailWindow::ViewMode::Total;
    view.m_sortDirection = CTrafficDetailWindow::SortDirection::Descending;
    view.m_sortColumn = 2; view.SortApps(entries);
    Check(entries[0].exeName == L"z.exe", "Historical download sorting regressed");
    view.m_sortColumn = 3; view.SortApps(entries);
    Check(entries[0].exeName == L"a.exe", "Historical upload sorting regressed");
    view.m_sortColumn = 4; view.SortApps(entries);
    Check(entries[0].exeName == L"a.exe", "Historical total sorting regressed");
    view.m_sortColumn = 1; view.m_sortDirection = CTrafficDetailWindow::SortDirection::Ascending; view.SortApps(entries);
    Check(entries[0].exeName == L"a.exe" && entries[2].exeName == L"z.exe", "Program-name sorting regressed");
    entries[0].rxBytesPerSec = 1; entries[0].txBytesPerSec = 9;
    entries[1].rxBytesPerSec = 8; entries[1].txBytesPerSec = 2;
    view.m_viewMode = CTrafficDetailWindow::ViewMode::Realtime;
    view.m_sortDirection = CTrafficDetailWindow::SortDirection::Descending;
    view.m_sortColumn = 2; view.SortApps(entries);
    Check(entries[0].exeName == L"b.exe", "Live download-speed sorting regressed");
    view.m_sortColumn = 3; view.SortApps(entries);
    Check(entries[0].exeName == L"a.exe", "Live upload-speed sorting regressed");
    std::cout << "PASS: program names, both live speeds, both history directions and total sorting\n";
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
void CheckIntegerFormatting()
{
    const std::pair<std::uint64_t, const wchar_t*> values[] = {
        { 0, L"0 B" }, { 1023, L"1023 B" }, { 1024, L"1.0 KB" }, { 1280, L"1.2 KB" },
        { 1792, L"1.8 KB" }, { 1048575, L"1024.0 KB" }, { 1048576, L"1.00 MB" },
        { 1179648, L"1.12 MB" }, { 1441792, L"1.38 MB" }, { 1073741824ULL, L"1.00 GB" },
        { 1207959552ULL, L"1.12 GB" }, { 1ULL << 40, L"1.00 TB" }, { 1ULL << 50, L"1.00 PB" },
        { 1ULL << 60, L"1.00 EB" }, { (1ULL << 60) + (1ULL << 57), L"1.12 EB" },
        { (1ULL << 60) + (3ULL << 57), L"1.38 EB" }, { UINT64_MAX, L"16.00 EB" }
    };
    for (const auto& value : values)
    {
        Check(Utils::FormatBytes(value.first) == value.second, "Integer byte formatter changed a unit or nearest-even tie");
        Check(Utils::FormatRate(value.first) == std::wstring(value.second) + L"/s", "Rate units differ from byte units");
    }
    Check(Utils::FormatPercent(0, 0) == L"0.00%" && Utils::FormatPercent(1, 3) == L"33.33%" &&
        Utils::FormatPercent(2, 3) == L"66.67%" && Utils::FormatPercent(1, 20000) == L"0.00%" &&
        Utils::FormatPercent(3, 20000) == L"0.02%" && Utils::FormatPercent(UINT64_MAX, UINT64_MAX) == L"100.00%" &&
        Utils::FormatPercent(UINT64_MAX, UINT64_MAX / 2) == L"200.00%" && Utils::FormatPercent(UINT64_MAX, 1) == L"n/a",
        "Coverage formatter rounded incorrectly, overflowed, or masked coverage above 100 percent");
    std::cout << "PASS: integer byte/rate units, nearest-even ties, full-width counters and overflow-safe coverage formatting\n";
}
void CheckPausedCheckpoint(CProcNetPlugin& plugin, const std::filesystem::path& fixture)
{
    CHistoryTrafficStore::DateTimeRange verificationRange;
    GetLocalTime(&verificationRange.start);
    verificationRange.start.wHour = verificationRange.start.wMinute = 0;
    plugin.m_historyStore.Initialize(fixture.wstring());
    plugin.m_historyStore.GetAllTimeTotal();
    plugin.m_historyInitialized = true;
    CTrafficDetailWindow view(plugin);
    const auto window = CreateHiddenWindow(view);
    view.ToggleRefreshPaused();
    Check(view.m_refreshPaused, "UI did not pause");
    plugin.m_collector.Start();
    const auto stop = [&] { plugin.m_collector.Stop(); DestroyWindow(window); };
    try
    {
        Traffic::NetworkEvent event;
        event.key.pid = 4294967293u; event.key.family = 4; event.key.protocol = 17;
        event.timestamp = Traffic::FileTimeNow(); event.bytes = 1234;
        plugin.m_collector.m_etw.m_eventSink(plugin.m_collector.m_etw.m_eventContext, event);
        PumpUntil([&] {
            std::lock_guard<std::mutex> lock(plugin.m_historyMutex);
            return plugin.m_historyStore.GetAllTimeTotal().rxBytes >= 1234;
        }, 5000);
        const auto checkpoint = fixture / L"app_traffic_history/current.tmp";
        Check(std::filesystem::exists(checkpoint), "Paused UI did not checkpoint without host callbacks");
        const auto original = std::filesystem::last_write_time(checkpoint);
        ULONGLONG originalTick{};
        {
            std::lock_guard<std::mutex> lock(plugin.m_historyMutex);
            originalTick = plugin.m_historyStore.m_lastCheckpointTick;
        }
        event.timestamp = Traffic::FileTimeNow(); event.send = true; event.bytes = 5678;
        plugin.m_collector.m_etw.m_eventSink(plugin.m_collector.m_etw.m_eventContext, event);
        ULONGLONG previousTick = originalTick, maximumGap{};
        PumpUntil([&] {
            std::lock_guard<std::mutex> lock(plugin.m_historyMutex);
            const auto tick = plugin.m_historyStore.m_lastCheckpointTick;
            if (tick > previousTick) { maximumGap = (std::max)(maximumGap, tick - previousTick); previousTick = tick; }
            return plugin.m_historyStore.GetAllTimeTotal().txBytes >= 5678 &&
                !plugin.m_historyStore.m_checkpointDirty && std::filesystem::last_write_time(checkpoint) != original;
        }, 12000);
        {
            std::lock_guard<std::mutex> lock(plugin.m_historyMutex);
            Check(maximumGap <= 10000, "Independent checkpoint exceeded its ten-second target");
            std::cout << "checkpoint_gap_ms=" << maximumGap << '\n';
        }
        stop();
        CHistoryTrafficStore reopened;
        reopened.Initialize(fixture.wstring());
        // A deliberately paused UI range can end in the previous minute/day.
        // Verify persisted collection with an explicit range through test end.
        GetLocalTime(&verificationRange.end);
        const auto apps = reopened.GetRangeAppTotals(verificationRange);
        const auto target = std::find_if(apps.begin(), apps.end(), [](const auto& app) { return app.appName == L"PID4294967293"; });
        Check(target != apps.end() && target->rxTotalBytes == 1234 && target->txTotalBytes == 5678,
            "Paused/no-host checkpoint did not recover exact increments");
    }
    catch (...) { stop(); throw; }
    std::cout << "PASS: paused UI and no host calls retain exact traffic with independently refreshed durable checkpoints\n";
}
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        auto& plugin = CProcNetPlugin::Instance();
        const bool ui_only = argc == 2 && std::wstring(argv[1]) == L"--ui-only";
        if (argc == 2 && !ui_only)
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
            CheckSorting(plugin);
            CheckSummaries(fixture / L"summaries");
            CheckNetworkView(plugin, fixture);
            CheckUnicodeState(fixture / L"unicode");
            CheckIntegerFormatting();
            if (!ui_only) CheckPausedCheckpoint(plugin, fixture / L"paused-checkpoint");
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
