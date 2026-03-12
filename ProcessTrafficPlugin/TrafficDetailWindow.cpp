#include "TrafficDetailWindow.h"

#include "ProcessFinder.h"
#include "ProcNetPlugin.h"
#include "Utils.h"

#include <Windows.h>
#include <Windowsx.h>
#include <CommCtrl.h>
#include <Shellapi.h>

#include <algorithm>
#include <cwctype>
#include <vector>

#pragma comment(lib, "comctl32.lib")

namespace
{
constexpr wchar_t kWindowClassName[] = L"TMProcessTrafficDetailWindow";
constexpr UINT_PTR kRefreshTimerId = 1;
constexpr int kToggleViewButtonId = 1001;
constexpr int kPauseRefreshButtonId = 1002;
constexpr int kLanguageLabelId = 1007;
constexpr int kLanguageComboId = 1008;
constexpr int kSummaryId = 1003;
constexpr int kListId = 1004;
constexpr int kPeriodLabelId = 1005;
constexpr int kPeriodComboId = 1006;
constexpr int kMargin = 10;
constexpr int kButtonWidth = 160;
constexpr int kButtonHeight = 28;
constexpr int kLanguageLabelWidth = 76;
constexpr int kLanguageComboWidth = 110;
constexpr int kComboWidth = 120;
constexpr int kLabelWidth = 80;
constexpr int kSummaryWidth = 280;
constexpr int kSummaryHeight = 150;

void AddColumn(HWND list, int index, int width, const wchar_t* title, int format = LVCFMT_LEFT)
{
    LVCOLUMNW column{};
    column.mask = LVCF_FMT | LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.fmt = format;
    column.pszText = const_cast<LPWSTR>(title);
    column.cx = width;
    column.iSubItem = index;
    ListView_InsertColumn(list, index, &column);
}

std::wstring NormalizeDisplayName(const std::wstring& name)
{
    size_t pos = 0;
    while (pos < name.size() && std::iswdigit(name[pos]) != 0)
    {
        ++pos;
    }

    if (pos > 0 && pos + 1 < name.size() && name[pos] == L'.' && name[pos + 1] == L' ')
    {
        return name.substr(pos + 2);
    }

    return name;
}

bool IsEnglish(CHistoryTrafficStore::DisplayLanguage language)
{
    return language == CHistoryTrafficStore::DisplayLanguage::English;
}
}

CTrafficDetailWindow::CTrafficDetailWindow(CProcNetPlugin& plugin)
    : m_plugin(plugin),
      m_hwnd(nullptr),
      m_list(nullptr),
      m_toggleViewButton(nullptr),
      m_languageLabel(nullptr),
      m_languageCombo(nullptr),
      m_pauseRefreshButton(nullptr),
      m_periodLabel(nullptr),
      m_periodCombo(nullptr),
      m_summary(nullptr),
      m_smallImageList(nullptr),
      m_viewMode(ViewMode::Realtime),
      m_lastBuiltView(ViewMode::Realtime),
      m_lastBuiltLanguage(CHistoryTrafficStore::DisplayLanguage::English),
      m_refreshPaused(false),
      m_realtimeColumnWidths{ 26, 214, 130, 130, 130, 130 },
      m_totalColumnWidths{ 26, 234, 150, 150, 150 },
      m_defaultIconIndex(-1)
{
}

void CTrafficDetailWindow::Show(HWND parent)
{
    CreateOrActivate(parent);
}

void CTrafficDetailWindow::EnsureWindowClassRegistered()
{
    static bool registered = false;
    if (registered)
    {
        return;
    }

    WNDCLASSW window_class{};
    window_class.lpfnWndProc = &CTrafficDetailWindow::StaticWndProc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = kWindowClassName;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&window_class);
    registered = true;
}

void CTrafficDetailWindow::EnsureCommonControlsInitialized()
{
    static bool initialized = false;
    if (initialized)
    {
        return;
    }

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);
    initialized = true;
}

void CTrafficDetailWindow::EnsureImageList()
{
    if (m_list == nullptr || m_smallImageList != nullptr)
    {
        return;
    }

    m_smallImageList = ImageList_Create(16, 16, ILC_COLOR32 | ILC_MASK, 16, 16);
    ListView_SetImageList(m_list, m_smallImageList, LVSIL_SMALL);
    m_defaultIconIndex = AddIconToImageList(LoadDefaultExeIcon());
}

void CTrafficDetailWindow::CreateOrActivate(HWND parent)
{
    EnsureWindowClassRegistered();
    EnsureCommonControlsInitialized();

    if (m_hwnd != nullptr && IsWindow(m_hwnd))
    {
        ShowWindow(m_hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(m_hwnd);
        RefreshView();
        return;
    }

    m_hwnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        kWindowClassName,
        L"实时流量统计",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        920,
        620,
        parent,
        nullptr,
        GetModuleHandleW(nullptr),
        this);
}

void CTrafficDetailWindow::RefreshView()
{
    if (m_list == nullptr)
    {
        return;
    }

    const int top_index = ListView_GetTopIndex(m_list);
    const int selected_index = ListView_GetNextItem(m_list, -1, LVNI_SELECTED);

    RECT top_item_rect{};
    bool has_top_rect = false;
    if (top_index >= 0)
    {
        has_top_rect = ListView_GetItemRect(m_list, top_index, &top_item_rect, LVIR_BOUNDS) != FALSE;
    }

    SendMessageW(m_list, WM_SETREDRAW, FALSE, 0);

    ApplyLanguageTexts();
    EnsureColumnsForCurrentView();

    if (m_viewMode == ViewMode::Realtime)
    {
        FillRealtimeView();
        SetWindowTextW(m_summary, L"");
        ShowWindow(m_summary, SW_HIDE);
    }
    else
    {
        FillTotalView();
        SetWindowTextW(m_summary, m_plugin.BuildTotalsText(GetSelectedPeriodMode(), GetSelectedLanguage()).c_str());
        ShowWindow(m_summary, SW_SHOW);
    }

    ShowWindow(m_periodLabel, SW_SHOW);
    ShowWindow(m_periodCombo, SW_SHOW);

    const int item_count = ListView_GetItemCount(m_list);
    if (selected_index >= 0 && selected_index < item_count)
    {
        ListView_SetItemState(m_list, selected_index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }

    if (top_index >= 0 && top_index < item_count)
    {
        ListView_EnsureVisible(m_list, top_index, FALSE);

        if (has_top_rect)
        {
            RECT restored_top_item_rect{};
            if (ListView_GetItemRect(m_list, top_index, &restored_top_item_rect, LVIR_BOUNDS) != FALSE)
            {
                const int delta_y = restored_top_item_rect.top - top_item_rect.top;
                if (delta_y != 0)
                {
                    ListView_Scroll(m_list, 0, delta_y);
                }
            }
        }
    }

    SendMessageW(m_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(m_list, nullptr, TRUE);
    UpdateWindow(m_list);

    UpdateWindowTitle();
    UpdateButtonText();

    RECT rect{};
    GetClientRect(m_hwnd, &rect);
    ResizeChildren(rect.right - rect.left, rect.bottom - rect.top);
}

void CTrafficDetailWindow::ToggleView()
{
    SaveCurrentColumnWidths();
    m_viewMode = (m_viewMode == ViewMode::Realtime) ? ViewMode::Total : ViewMode::Realtime;
    RefreshView();
}

void CTrafficDetailWindow::ToggleRefreshPaused()
{
    m_refreshPaused = !m_refreshPaused;
    UpdateButtonText();
}

void CTrafficDetailWindow::ApplyLanguageTexts()
{
    const bool english = IsEnglish(GetSelectedLanguage());

    if (m_languageLabel != nullptr)
    {
        SetWindowTextW(m_languageLabel, english ? L"Language:" : L"语言:");
    }
    if (m_periodLabel != nullptr)
    {
        SetWindowTextW(m_periodLabel, english ? L"Period:" : L"统计范围:");
    }

    if (m_languageCombo != nullptr)
    {
        const int current = ComboBox_GetCurSel(m_languageCombo);
        ComboBox_ResetContent(m_languageCombo);
        ComboBox_AddString(m_languageCombo, L"English");
        ComboBox_AddString(m_languageCombo, L"中文");
        ComboBox_SetCurSel(m_languageCombo, current >= 0 ? current : 0);
    }

    if (m_periodCombo != nullptr)
    {
        const int current = ComboBox_GetCurSel(m_periodCombo);
        ComboBox_ResetContent(m_periodCombo);
        ComboBox_AddString(m_periodCombo, english ? L"Day" : L"日");
        ComboBox_AddString(m_periodCombo, english ? L"Month" : L"月");
        ComboBox_AddString(m_periodCombo, english ? L"Year" : L"年");
        ComboBox_SetCurSel(m_periodCombo, current >= 0 ? current : static_cast<int>(m_plugin.GetPreferredPeriodMode()));
    }
}

void CTrafficDetailWindow::UpdateWindowTitle()
{
    const bool english = IsEnglish(GetSelectedLanguage());
    SetWindowTextW(m_hwnd,
        m_viewMode == ViewMode::Realtime
            ? (english ? L"Real-Time Traffic" : L"实时流量统计")
            : (english ? L"Total Traffic" : L"总流量统计"));
}

void CTrafficDetailWindow::UpdateButtonText()
{
    const bool english = IsEnglish(GetSelectedLanguage());
    SetWindowTextW(m_toggleViewButton,
        m_viewMode == ViewMode::Realtime
            ? (english ? L"Show Total Traffic" : L"查看总流量")
            : (english ? L"Back To Real-Time" : L"返回实时流量"));
    SetWindowTextW(m_pauseRefreshButton,
        m_refreshPaused
            ? (english ? L"Resume Refresh" : L"继续刷新界面")
            : (english ? L"Pause Refresh" : L"暂停刷新界面"));
}

void CTrafficDetailWindow::EnsureColumnsForCurrentView()
{
    const auto language = GetSelectedLanguage();
    if (m_lastBuiltView == m_viewMode &&
        m_lastBuiltLanguage == language &&
        Header_GetItemCount(ListView_GetHeader(m_list)) > 0)
    {
        return;
    }

    RebuildColumnsForView(m_viewMode);
    m_lastBuiltView = m_viewMode;
    m_lastBuiltLanguage = language;
}

void CTrafficDetailWindow::RebuildColumnsForView(ViewMode view_mode)
{
    const bool english = IsEnglish(GetSelectedLanguage());
    while (Header_GetItemCount(ListView_GetHeader(m_list)) > 0)
    {
        ListView_DeleteColumn(m_list, 0);
    }

    if (view_mode == ViewMode::Realtime)
    {
        AddColumn(m_list, 0, m_realtimeColumnWidths[0], L" ", LVCFMT_CENTER);
        AddColumn(m_list, 1, m_realtimeColumnWidths[1], english ? L"Program" : L"程序");
        AddColumn(m_list, 2, m_realtimeColumnWidths[2], english ? L"Download" : L"实时下载");
        AddColumn(m_list, 3, m_realtimeColumnWidths[3], english ? L"Upload" : L"实时上传");
        AddColumn(m_list, 4, m_realtimeColumnWidths[4], english ? L"Total Down" : L"累计下载");
        AddColumn(m_list, 5, m_realtimeColumnWidths[5], english ? L"Total Up" : L"累计上传");
    }
    else
    {
        AddColumn(m_list, 0, m_totalColumnWidths[0], L" ", LVCFMT_CENTER);
        AddColumn(m_list, 1, m_totalColumnWidths[1], english ? L"Program" : L"程序");
        AddColumn(m_list, 2, m_totalColumnWidths[2], english ? L"Download" : L"下载");
        AddColumn(m_list, 3, m_totalColumnWidths[3], english ? L"Upload" : L"上传");
        AddColumn(m_list, 4, m_totalColumnWidths[4], english ? L"Total" : L"总流量");
    }
}

void CTrafficDetailWindow::SaveCurrentColumnWidths()
{
    if (m_list == nullptr)
    {
        return;
    }

    const int count = Header_GetItemCount(ListView_GetHeader(m_list));
    if (m_viewMode == ViewMode::Realtime && count == static_cast<int>(m_realtimeColumnWidths.size()))
    {
        for (int i = 0; i < count; ++i)
        {
            m_realtimeColumnWidths[static_cast<size_t>(i)] = ListView_GetColumnWidth(m_list, i);
        }
    }
    else if (m_viewMode == ViewMode::Total && count == static_cast<int>(m_totalColumnWidths.size()))
    {
        for (int i = 0; i < count; ++i)
        {
            m_totalColumnWidths[static_cast<size_t>(i)] = ListView_GetColumnWidth(m_list, i);
        }
    }
}

void CTrafficDetailWindow::FillRealtimeView()
{
    const auto apps = m_plugin.BuildAllApps();
    EnsureListItemCount(static_cast<int>(apps.size()));
    for (size_t i = 0; i < apps.size(); ++i)
    {
        std::vector<std::wstring> columns;
        columns.reserve(5);
        columns.push_back(NormalizeDisplayName(apps[i].exeName));
        columns.push_back(Utils::FormatRate(apps[i].rxBytesPerSec));
        columns.push_back(Utils::FormatRate(apps[i].txBytesPerSec));
        columns.push_back(Utils::FormatBytes(apps[i].rxTotalBytes));
        columns.push_back(Utils::FormatBytes(apps[i].txTotalBytes));
        UpsertListRow(static_cast<int>(i), GetIconIndex(apps[i].exeName, apps[i].exePath), columns);
    }
}

void CTrafficDetailWindow::FillTotalView()
{
    auto apps = m_plugin.BuildHistoryApps(GetSelectedPeriodMode());
    std::sort(apps.begin(), apps.end(), [](const CProcNetPlugin::AppTrafficEntry& left, const CProcNetPlugin::AppTrafficEntry& right) {
        return (left.rxTotalBytes + left.txTotalBytes) > (right.rxTotalBytes + right.txTotalBytes);
    });

    EnsureListItemCount(static_cast<int>(apps.size()));
    for (size_t i = 0; i < apps.size(); ++i)
    {
        std::vector<std::wstring> columns;
        columns.reserve(4);
        columns.push_back(NormalizeDisplayName(apps[i].exeName));
        columns.push_back(Utils::FormatBytes(apps[i].rxTotalBytes));
        columns.push_back(Utils::FormatBytes(apps[i].txTotalBytes));
        columns.push_back(Utils::FormatBytes(apps[i].rxTotalBytes + apps[i].txTotalBytes));
        UpsertListRow(static_cast<int>(i), GetIconIndex(apps[i].exeName, apps[i].exePath), columns);
    }
}

void CTrafficDetailWindow::EnsureListItemCount(int item_count)
{
    const int current_count = ListView_GetItemCount(m_list);
    if (current_count < item_count)
    {
        for (int i = current_count; i < item_count; ++i)
        {
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = i;
            item.pszText = const_cast<LPWSTR>(L"");
            ListView_InsertItem(m_list, &item);
        }
    }
    else if (current_count > item_count)
    {
        for (int i = current_count - 1; i >= item_count; --i)
        {
            ListView_DeleteItem(m_list, i);
        }
    }
}

void CTrafficDetailWindow::UpsertListRow(int row, int image_index, const std::vector<std::wstring>& columns)
{
    if (row < 0 || columns.empty())
    {
        return;
    }

    LVITEMW item{};
    item.mask = LVIF_IMAGE;
    item.iItem = row;
    item.iSubItem = 0;
    item.iImage = image_index >= 0 ? image_index : m_defaultIconIndex;
    ListView_SetItem(m_list, &item);
    SetListText(row, 0, L"");

    SetListText(row, 1, columns[0].c_str());
    for (size_t column = 1; column < columns.size(); ++column)
    {
        SetListText(row, static_cast<int>(column + 1), columns[column].c_str());
    }
}

int CTrafficDetailWindow::GetIconIndex(const std::wstring& exe_name, const std::wstring& exe_path)
{
    EnsureImageList();

    const std::wstring cache_key = exe_path.empty() ? exe_name : exe_path;
    const auto it = m_iconIndexByKey.find(cache_key);
    if (it != m_iconIndexByKey.end())
    {
        return it->second;
    }

    HICON icon = LoadSmallExeIcon(exe_path);
    if (icon == nullptr && !exe_name.empty())
    {
        icon = LoadSmallExeIcon(CProcessFinder::FindFirstProcessPathByExeName(exe_name));
    }

    int image_index = m_defaultIconIndex;
    if (icon != nullptr)
    {
        image_index = AddIconToImageList(icon);
    }

    m_iconIndexByKey.emplace(cache_key, image_index);
    return image_index;
}

int CTrafficDetailWindow::AddIconToImageList(HICON icon)
{
    if (icon == nullptr)
    {
        return -1;
    }

    const int image_index = m_smallImageList != nullptr ? ImageList_AddIcon(m_smallImageList, icon) : -1;
    DestroyIcon(icon);
    return image_index;
}

HICON CTrafficDetailWindow::LoadSmallExeIcon(const std::wstring& exe_path) const
{
    if (exe_path.empty())
    {
        return nullptr;
    }

    SHFILEINFOW file_info{};
    if (SHGetFileInfoW(exe_path.c_str(), FILE_ATTRIBUTE_NORMAL, &file_info, sizeof(file_info),
        SHGFI_ICON | SHGFI_SMALLICON) == 0)
    {
        return nullptr;
    }

    return file_info.hIcon;
}

HICON CTrafficDetailWindow::LoadDefaultExeIcon() const
{
    SHFILEINFOW file_info{};
    if (SHGetFileInfoW(L".exe", FILE_ATTRIBUTE_NORMAL, &file_info, sizeof(file_info),
        SHGFI_USEFILEATTRIBUTES | SHGFI_ICON | SHGFI_SMALLICON) == 0)
    {
        return nullptr;
    }

    return file_info.hIcon;
}

void CTrafficDetailWindow::ResizeChildren(int width, int height)
{
    const int right_button_x = width - kMargin - kButtonWidth;
    const int language_left = kMargin + kButtonWidth + 12;
    if (m_toggleViewButton != nullptr)
    {
        MoveWindow(m_toggleViewButton, kMargin, kMargin, kButtonWidth, kButtonHeight, TRUE);
    }
    if (m_languageLabel != nullptr)
    {
        MoveWindow(m_languageLabel, language_left, kMargin + 4, kLanguageLabelWidth, 20, TRUE);
    }
    if (m_languageCombo != nullptr)
    {
        MoveWindow(m_languageCombo, language_left + kLanguageLabelWidth, kMargin, kLanguageComboWidth, 300, TRUE);
    }
    if (m_pauseRefreshButton != nullptr)
    {
        MoveWindow(m_pauseRefreshButton, right_button_x, kMargin, kButtonWidth, kButtonHeight, TRUE);
    }
    if (m_periodLabel != nullptr)
    {
        MoveWindow(m_periodLabel, right_button_x - kLabelWidth - kComboWidth - 16, kMargin + 4, kLabelWidth, 20, TRUE);
    }
    if (m_periodCombo != nullptr)
    {
        MoveWindow(m_periodCombo, right_button_x - kComboWidth - 8, kMargin, kComboWidth, 400, TRUE);
    }

    const int top = kMargin + kButtonHeight + 10;
    if (m_viewMode == ViewMode::Realtime)
    {
        if (m_list != nullptr)
        {
            MoveWindow(m_list, kMargin, top, width - kMargin * 2, height - top - kMargin, TRUE);
        }
        return;
    }

    if (m_summary != nullptr)
    {
        MoveWindow(m_summary, kMargin, height - kMargin - kSummaryHeight, kSummaryWidth, kSummaryHeight, TRUE);
    }

    if (m_list != nullptr)
    {
        MoveWindow(m_list, kMargin, top, width - kMargin * 2, height - top - kSummaryHeight - kMargin * 2, TRUE);
    }
}

void CTrafficDetailWindow::ClearList()
{
    ListView_DeleteAllItems(m_list);
}

void CTrafficDetailWindow::SetListText(int row, int column, const wchar_t* text)
{
    ListView_SetItemText(m_list, row, column, const_cast<LPWSTR>(text));
}

CHistoryTrafficStore::PeriodMode CTrafficDetailWindow::GetSelectedPeriodMode() const
{
    if (m_periodCombo == nullptr)
    {
        return CHistoryTrafficStore::PeriodMode::Day;
    }

    switch (ComboBox_GetCurSel(m_periodCombo))
    {
    case 1:
        return CHistoryTrafficStore::PeriodMode::Month;
    case 2:
        return CHistoryTrafficStore::PeriodMode::Year;
    default:
        return CHistoryTrafficStore::PeriodMode::Day;
    }
}

CHistoryTrafficStore::DisplayLanguage CTrafficDetailWindow::GetSelectedLanguage() const
{
    if (m_languageCombo == nullptr)
    {
        return CHistoryTrafficStore::DisplayLanguage::English;
    }

    return ComboBox_GetCurSel(m_languageCombo) == 1
        ? CHistoryTrafficStore::DisplayLanguage::Chinese
        : CHistoryTrafficStore::DisplayLanguage::English;
}

LRESULT CTrafficDetailWindow::HandleMessage(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param)
{
    switch (message)
    {
    case WM_CREATE:
    {
        m_toggleViewButton = CreateWindowW(L"BUTTON", L"查看总流量", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            10, 10, 160, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kToggleViewButtonId)), GetModuleHandleW(nullptr), nullptr);

        m_languageLabel = CreateWindowW(L"STATIC", L"Language:", WS_CHILD | WS_VISIBLE,
            184, 14, kLanguageLabelWidth, 20, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLanguageLabelId)), GetModuleHandleW(nullptr), nullptr);

        m_languageCombo = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            184 + kLanguageLabelWidth, 10, kLanguageComboWidth, 200, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLanguageComboId)), GetModuleHandleW(nullptr), nullptr);
        ComboBox_AddString(m_languageCombo, L"English");
        ComboBox_AddString(m_languageCombo, L"中文");
        ComboBox_SetCurSel(m_languageCombo, static_cast<int>(m_plugin.GetPreferredLanguage()));

        m_pauseRefreshButton = CreateWindowW(L"BUTTON", L"暂停刷新界面", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            730, 10, 160, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPauseRefreshButtonId)), GetModuleHandleW(nullptr), nullptr);

        m_periodLabel = CreateWindowW(L"STATIC", L"统计范围:", WS_CHILD,
            520, 14, 80, 20, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPeriodLabelId)), GetModuleHandleW(nullptr), nullptr);

        m_periodCombo = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
            610, 10, 110, 200, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPeriodComboId)), GetModuleHandleW(nullptr), nullptr);
        ComboBox_AddString(m_periodCombo, L"日");
        ComboBox_AddString(m_periodCombo, L"月");
        ComboBox_AddString(m_periodCombo, L"年");
        ComboBox_SetCurSel(m_periodCombo, static_cast<int>(m_plugin.GetPreferredPeriodMode()));

        m_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            10, 48, 860, 500, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kListId)), GetModuleHandleW(nullptr), nullptr);
        ListView_SetExtendedListViewStyle(m_list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP);
        EnsureImageList();

        m_summary = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
            10, 450, 280, 150, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSummaryId)), GetModuleHandleW(nullptr), nullptr);

        SendMessageW(m_toggleViewButton, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        SendMessageW(m_languageLabel, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        SendMessageW(m_languageCombo, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        SendMessageW(m_pauseRefreshButton, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        SendMessageW(m_periodLabel, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        SendMessageW(m_periodCombo, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        SendMessageW(m_list, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        SendMessageW(m_summary, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);

        SetTimer(hwnd, kRefreshTimerId, 1000, nullptr);
        RefreshView();
        return 0;
    }
    case WM_SIZE:
        ResizeChildren(LOWORD(l_param), HIWORD(l_param));
        return 0;
    case WM_TIMER:
        if (w_param == kRefreshTimerId && !m_refreshPaused)
        {
            if ((m_periodCombo != nullptr && SendMessageW(m_periodCombo, CB_GETDROPPEDSTATE, 0, 0) != 0) ||
                (m_languageCombo != nullptr && SendMessageW(m_languageCombo, CB_GETDROPPEDSTATE, 0, 0) != 0))
            {
                return 0;
            }
            RefreshView();
            return 0;
        }
        break;
    case WM_COMMAND:
        if (LOWORD(w_param) == kToggleViewButtonId)
        {
            ToggleView();
            return 0;
        }
        if (LOWORD(w_param) == kPauseRefreshButtonId)
        {
            ToggleRefreshPaused();
            return 0;
        }
        if (LOWORD(w_param) == kLanguageComboId && HIWORD(w_param) == CBN_SELCHANGE)
        {
            m_plugin.SetPreferredLanguage(GetSelectedLanguage());
            RefreshView();
            return 0;
        }
        if (LOWORD(w_param) == kPeriodComboId && HIWORD(w_param) == CBN_SELCHANGE)
        {
            m_plugin.SetPreferredPeriodMode(GetSelectedPeriodMode());
            RefreshView();
            return 0;
        }
        break;
    case WM_NOTIFY:
        if (reinterpret_cast<NMHDR*>(l_param)->hwndFrom == ListView_GetHeader(m_list) &&
            reinterpret_cast<NMHDR*>(l_param)->code == HDN_ENDTRACKW)
        {
            SaveCurrentColumnWidths();
            return 0;
        }
        break;
    case WM_CLOSE:
        SaveCurrentColumnWidths();
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kRefreshTimerId);
        if (m_smallImageList != nullptr)
        {
            ImageList_Destroy(m_smallImageList);
            m_smallImageList = nullptr;
        }
        m_iconIndexByKey.clear();
        m_defaultIconIndex = -1;
        m_hwnd = nullptr;
        m_list = nullptr;
        m_toggleViewButton = nullptr;
        m_languageLabel = nullptr;
        m_languageCombo = nullptr;
        m_pauseRefreshButton = nullptr;
        m_periodLabel = nullptr;
        m_periodCombo = nullptr;
        m_summary = nullptr;
        return 0;
    default:
        break;
    }

    return DefWindowProcW(hwnd, message, w_param, l_param);
}

LRESULT CALLBACK CTrafficDetailWindow::StaticWndProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param)
{
    CTrafficDetailWindow* self = nullptr;
    if (message == WM_NCCREATE)
    {
        auto* create_struct = reinterpret_cast<CREATESTRUCTW*>(l_param);
        self = static_cast<CTrafficDetailWindow*>(create_struct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_hwnd = hwnd;
    }
    else
    {
        self = reinterpret_cast<CTrafficDetailWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self != nullptr)
    {
        return self->HandleMessage(hwnd, message, w_param, l_param);
    }

    return DefWindowProcW(hwnd, message, w_param, l_param);
}

