#include "TrafficDetailWindow.h"

#include "ProcessFinder.h"
#include "ProcNetPlugin.h"
#include "Utils.h"

#include <Windows.h>
#include <Windowsx.h>
#include <CommCtrl.h>
#include <Shellapi.h>

#include <algorithm>
#include <array>
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
constexpr int kStartLabelId = 1005;
constexpr int kStartDatePickerId = 1006;
constexpr int kStartTimePickerId = 1011;
constexpr int kEndLabelId = 1009;
constexpr int kEndDatePickerId = 1010;
constexpr int kEndTimePickerId = 1012;
constexpr int kDayRangeButtonId = 1013;
constexpr int kMonthRangeButtonId = 1014;
constexpr int kYearRangeButtonId = 1015;
constexpr int kSummaryId = 1003;
constexpr int kListId = 1004;
constexpr int kMargin = 10;
constexpr int kButtonWidth = 160;
constexpr int kButtonHeight = 28;
constexpr int kLanguageLabelWidth = 76;
constexpr int kLanguageComboWidth = 110;
constexpr int kRangeLabelWidth = 84;
constexpr int kRangeDateWidth = 130;
constexpr int kRangeTimeWidth = 88;
constexpr int kSummaryWidth = 350;
constexpr int kSummaryHeight = 150;
constexpr int kQuickButtonWidth = 58;
constexpr int kQuickButtonHeight = 28;
constexpr int kTopAreaHeight = kMargin + kButtonHeight + 10;
constexpr int kRangeRowSpacing = 36;
constexpr int kDateTimePickerHeight = 28;
constexpr int kLabelHeight = 20;
constexpr int kComboDropHeight = 200;
constexpr int kListInitialHeight = 500;

struct LocalizedText
{
    const wchar_t* english;
    const wchar_t* chinese;
};

struct ColumnDefinition
{
    LocalizedText title;
    int format;
};

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

const wchar_t* GetLocalizedText(CHistoryTrafficStore::DisplayLanguage language, const LocalizedText& text)
{
    return IsEnglish(language) ? text.english : text.chinese;
}

const wchar_t* GetLocalizedText(bool english, const LocalizedText& text)
{
    return english ? text.english : text.chinese;
}

std::vector<std::wstring> BuildRealtimeRow(const CProcNetPlugin::AppTrafficEntry& app)
{
    std::vector<std::wstring> columns;
    columns.reserve(5);
    columns.push_back(NormalizeDisplayName(app.exeName));
    columns.push_back(Utils::FormatRate(app.rxBytesPerSec));
    columns.push_back(Utils::FormatRate(app.txBytesPerSec));
    columns.push_back(Utils::FormatBytes(app.rxTotalBytes));
    columns.push_back(Utils::FormatBytes(app.txTotalBytes));
    return columns;
}

std::vector<std::wstring> BuildTotalRow(const CProcNetPlugin::AppTrafficEntry& app)
{
    std::vector<std::wstring> columns;
    columns.reserve(4);
    columns.push_back(NormalizeDisplayName(app.exeName));
    columns.push_back(Utils::FormatBytes(app.rxTotalBytes));
    columns.push_back(Utils::FormatBytes(app.txTotalBytes));
    columns.push_back(Utils::FormatBytes(app.rxTotalBytes + app.txTotalBytes));
    return columns;
}

void MoveControlIfPresent(HWND control, int x, int y, int width, int height)
{
    if (control != nullptr)
    {
        MoveWindow(control, x, y, width, height, TRUE);
    }
}

HWND CreateChildWindow(
    const wchar_t* class_name,
    const wchar_t* text,
    DWORD style,
    int x,
    int y,
    int width,
    int height,
    HWND parent,
    int control_id)
{
    return CreateWindowW(
        class_name,
        text,
        style,
        x,
        y,
        width,
        height,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(control_id)),
        GetModuleHandleW(nullptr),
        nullptr);
}

HWND CreateDateTimePickerControl(HWND parent, int control_id, DWORD style, int x, int y, int width)
{
    return CreateWindowExW(
        0,
        DATETIMEPICK_CLASSW,
        L"",
        WS_CHILD | WS_VISIBLE | style,
        x,
        y,
        width,
        kDateTimePickerHeight,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(control_id)),
        GetModuleHandleW(nullptr),
        nullptr);
}

void SetPickerTimeIfPresent(HWND picker, const SYSTEMTIME& time)
{
    if (picker != nullptr)
    {
        DateTime_SetSystemtime(picker, GDT_VALID, &time);
    }
}

void ApplyDateFromPicker(HWND picker, SYSTEMTIME& target)
{
    if (picker == nullptr)
    {
        return;
    }

    SYSTEMTIME value{};
    DateTime_GetSystemtime(picker, &value);
    target.wYear = value.wYear;
    target.wMonth = value.wMonth;
    target.wDay = value.wDay;
}

void ApplyTimeFromPicker(HWND picker, SYSTEMTIME& target)
{
    if (picker == nullptr)
    {
        return;
    }

    SYSTEMTIME value{};
    DateTime_GetSystemtime(picker, &value);
    target.wHour = value.wHour;
    target.wMinute = value.wMinute;
}

bool IsRangeControlId(UINT_PTR control_id)
{
    return control_id == kStartDatePickerId ||
        control_id == kStartTimePickerId ||
        control_id == kEndDatePickerId ||
        control_id == kEndTimePickerId;
}

const std::array<ColumnDefinition, 6> kRealtimeColumns{ {
    { { L" ", L" " }, LVCFMT_CENTER },
    { { L"Program", L"程序" }, LVCFMT_LEFT },
    { { L"Download", L"实时下载" }, LVCFMT_LEFT },
    { { L"Upload", L"实时上传" }, LVCFMT_LEFT },
    { { L"Total Down", L"累计下载" }, LVCFMT_LEFT },
    { { L"Total Up", L"累计上传" }, LVCFMT_LEFT },
} };

const std::array<ColumnDefinition, 5> kTotalColumns{ {
    { { L" ", L" " }, LVCFMT_CENTER },
    { { L"Program", L"程序" }, LVCFMT_LEFT },
    { { L"Download", L"下载" }, LVCFMT_LEFT },
    { { L"Upload", L"上传" }, LVCFMT_LEFT },
    { { L"Total", L"总流量" }, LVCFMT_LEFT },
} };

LocalizedText GetWindowTitle(bool realtime_view)
{
    return realtime_view
        ? LocalizedText{ L"Real-Time Traffic", L"实时流量统计" }
        : LocalizedText{ L"Total Traffic", L"总流量统计" };
}

LocalizedText GetToggleButtonText(bool realtime_view)
{
    return realtime_view
        ? LocalizedText{ L"Show Total Traffic", L"查看总流量" }
        : LocalizedText{ L"Back To Real-Time", L"返回实时流量" };
}

LocalizedText GetPauseButtonText(bool refresh_paused)
{
    return refresh_paused
        ? LocalizedText{ L"Resume Refresh", L"继续刷新界面" }
        : LocalizedText{ L"Pause Refresh", L"暂停刷新界面" };
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
      m_startLabel(nullptr),
      m_startDatePicker(nullptr),
      m_startTimePicker(nullptr),
      m_endLabel(nullptr),
      m_endDatePicker(nullptr),
      m_endTimePicker(nullptr),
      m_dayRangeButton(nullptr),
      m_monthRangeButton(nullptr),
      m_yearRangeButton(nullptr),
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
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_DATE_CLASSES;
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
        800,
        650,
        parent,
        nullptr,
        GetModuleHandleW(nullptr),
        this);
}

void CTrafficDetailWindow::CreateChildControls(HWND hwnd)
{
    const int language_left = kMargin + kButtonWidth + 14;
    const int start_left = 520;
    const int start_picker_left = start_left + 42;
    const int end_left = 720;
    const int end_picker_left = end_left + 40;
    const int quick_button_left = 980;

    m_toggleViewButton = CreateChildWindow(
        L"BUTTON", L"查看总流量", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        kMargin, kMargin, kButtonWidth, kButtonHeight, hwnd, kToggleViewButtonId);

    m_languageLabel = CreateChildWindow(
        L"STATIC", L"Language:", WS_CHILD | WS_VISIBLE,
        language_left, kMargin + 4, kLanguageLabelWidth, kLabelHeight, hwnd, kLanguageLabelId);

    m_languageCombo = CreateChildWindow(
        L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        language_left + kLanguageLabelWidth, kMargin, kLanguageComboWidth, kComboDropHeight, hwnd, kLanguageComboId);
    ComboBox_AddString(m_languageCombo, L"English");
    ComboBox_AddString(m_languageCombo, L"中文");
    ComboBox_SetCurSel(m_languageCombo, static_cast<int>(m_plugin.GetPreferredLanguage()));

    m_pauseRefreshButton = CreateChildWindow(
        L"BUTTON", L"暂停刷新界面", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        730, kMargin, kButtonWidth, kButtonHeight, hwnd, kPauseRefreshButtonId);

    m_startLabel = CreateChildWindow(
        L"STATIC", L"Start:", WS_CHILD | WS_VISIBLE,
        start_left, kMargin + 4, kRangeLabelWidth, kLabelHeight, hwnd, kStartLabelId);
    m_startDatePicker = CreateDateTimePickerControl(hwnd, kStartDatePickerId, DTS_SHORTDATECENTURYFORMAT, start_picker_left, kMargin, kRangeDateWidth);
    m_startTimePicker = CreateDateTimePickerControl(hwnd, kStartTimePickerId, DTS_TIMEFORMAT | DTS_UPDOWN, start_picker_left + kRangeDateWidth + 8, kMargin, kRangeTimeWidth);

    m_endLabel = CreateChildWindow(
        L"STATIC", L"End:", WS_CHILD | WS_VISIBLE,
        end_left, kMargin + 4, kRangeLabelWidth, kLabelHeight, hwnd, kEndLabelId);
    m_endDatePicker = CreateDateTimePickerControl(hwnd, kEndDatePickerId, DTS_SHORTDATECENTURYFORMAT, end_picker_left, kMargin, kRangeDateWidth);
    m_endTimePicker = CreateDateTimePickerControl(hwnd, kEndTimePickerId, DTS_TIMEFORMAT | DTS_UPDOWN, end_picker_left + kRangeDateWidth + 8, kMargin, kRangeTimeWidth);

    m_dayRangeButton = CreateChildWindow(
        L"BUTTON", L"日", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        quick_button_left, kMargin, kQuickButtonWidth, kQuickButtonHeight, hwnd, kDayRangeButtonId);
    m_monthRangeButton = CreateChildWindow(
        L"BUTTON", L"月", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        quick_button_left, kMargin + kRangeRowSpacing, kQuickButtonWidth, kQuickButtonHeight, hwnd, kMonthRangeButtonId);
    m_yearRangeButton = CreateChildWindow(
        L"BUTTON", L"年", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        quick_button_left, kMargin + kRangeRowSpacing * 2, kQuickButtonWidth, kQuickButtonHeight, hwnd, kYearRangeButtonId);

    m_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        kMargin, kTopAreaHeight, 860, kListInitialHeight, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kListId)), GetModuleHandleW(nullptr), nullptr);
    ListView_SetExtendedListViewStyle(m_list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP);
    EnsureImageList();

    m_summary = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
        kMargin, 450, 280, kSummaryHeight, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSummaryId)), GetModuleHandleW(nullptr), nullptr);

    ApplyCurrentRangeToControls();
    ApplyRangeControls();
    SetAllControlFonts();
}

void CTrafficDetailWindow::SetAllControlFonts() const
{
    const auto font = reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT));
    const HWND controls[] = {
        m_toggleViewButton, m_languageLabel, m_languageCombo, m_pauseRefreshButton,
        m_startLabel, m_startDatePicker, m_startTimePicker, m_endLabel, m_endDatePicker,
        m_endTimePicker, m_dayRangeButton, m_monthRangeButton, m_yearRangeButton, m_list, m_summary
    };

    for (HWND control : controls)
    {
        if (control != nullptr)
        {
            SendMessageW(control, WM_SETFONT, font, TRUE);
        }
    }
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
    UpdateViewSpecificControls();

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
    const auto language = GetSelectedLanguage();
    SetWindowTextIfPresent(m_languageLabel, GetLocalizedText(language, { L"Language:", L"语言:" }));
    SetWindowTextIfPresent(m_startLabel, GetLocalizedText(language, { L"Start:", L"开始时间:" }));
    SetWindowTextIfPresent(m_endLabel, GetLocalizedText(language, { L"End:", L"结束时间:" }));
    SetWindowTextIfPresent(m_dayRangeButton, GetLocalizedText(language, { L"Day", L"日" }));
    SetWindowTextIfPresent(m_monthRangeButton, GetLocalizedText(language, { L"Month", L"月" }));
    SetWindowTextIfPresent(m_yearRangeButton, GetLocalizedText(language, { L"Year", L"年" }));

    if (m_languageCombo != nullptr)
    {
        const int current = ComboBox_GetCurSel(m_languageCombo);
        ComboBox_ResetContent(m_languageCombo);
        ComboBox_AddString(m_languageCombo, L"English");
        ComboBox_AddString(m_languageCombo, L"中文");
        ComboBox_SetCurSel(m_languageCombo, current >= 0 ? current : 0);
    }

    ApplyRangeControls();
}

void CTrafficDetailWindow::ApplyQuickRange(int button_id)
{
    SYSTEMTIME now{};
    GetLocalTime(&now);

    CHistoryTrafficStore::DateTimeRange range{};
    range.start = now;
    range.end = now;
    range.start.wSecond = 0;
    range.start.wMilliseconds = 0;
    range.end.wSecond = 0;
    range.end.wMilliseconds = 0;

    if (button_id == kDayRangeButtonId)
    {
        range.start.wHour = 0;
        range.start.wMinute = 0;
    }
    else if (button_id == kMonthRangeButtonId)
    {
        range.start.wDay = 1;
        range.start.wHour = 0;
        range.start.wMinute = 0;
    }
    else if (button_id == kYearRangeButtonId)
    {
        range.start.wMonth = 1;
        range.start.wDay = 1;
        range.start.wHour = 0;
        range.start.wMinute = 0;
    }
    else
    {
        return;
    }

    m_plugin.SetPreferredRange(range);
    ApplyCurrentRangeToControls();
}

void CTrafficDetailWindow::ApplyCurrentRangeToControls()
{
    const auto range = m_plugin.GetPreferredRange();
    SetPickerTimeIfPresent(m_startDatePicker, range.start);
    SetPickerTimeIfPresent(m_startTimePicker, range.start);
    SetPickerTimeIfPresent(m_endDatePicker, range.end);
    SetPickerTimeIfPresent(m_endTimePicker, range.end);
}

void CTrafficDetailWindow::ShowTotalViewControls(bool show)
{
    const int command = show ? SW_SHOW : SW_HIDE;
    const HWND controls[] = {
        m_startLabel, m_startDatePicker, m_startTimePicker, m_endLabel,
        m_endDatePicker, m_endTimePicker, m_dayRangeButton, m_monthRangeButton, m_yearRangeButton
    };

    for (HWND control : controls)
    {
        if (control != nullptr)
        {
            ShowWindow(control, command);
        }
    }
}

void CTrafficDetailWindow::UpdateViewSpecificControls()
{
    if (m_viewMode == ViewMode::Realtime)
    {
        FillRealtimeView();
        SetWindowTextIfPresent(m_summary, L"");
        if (m_summary != nullptr)
        {
            ShowWindow(m_summary, SW_HIDE);
        }
        ShowTotalViewControls(false);
        return;
    }

    FillTotalView();
    SetWindowTextIfPresent(m_summary, m_plugin.BuildTotalsText(GetSelectedRange(), GetSelectedLanguage()).c_str());
    if (m_summary != nullptr)
    {
        ShowWindow(m_summary, SW_SHOW);
    }
    ShowTotalViewControls(true);
}

void CTrafficDetailWindow::UpdateWindowTitle()
{
    SetWindowTextIfPresent(m_hwnd, GetLocalizedText(GetSelectedLanguage(), GetWindowTitle(m_viewMode == ViewMode::Realtime)));
}

void CTrafficDetailWindow::UpdateButtonText()
{
    const auto language = GetSelectedLanguage();
    SetWindowTextIfPresent(m_toggleViewButton, GetLocalizedText(language, GetToggleButtonText(m_viewMode == ViewMode::Realtime)));
    SetWindowTextIfPresent(m_pauseRefreshButton, GetLocalizedText(language, GetPauseButtonText(m_refreshPaused)));
}

void CTrafficDetailWindow::ApplyRangeControls()
{
    if (m_startDatePicker != nullptr)
    {
        DateTime_SetFormat(m_startDatePicker, L"yyyy-MM-dd");
    }
    if (m_startTimePicker != nullptr)
    {
        DateTime_SetFormat(m_startTimePicker, L"HH:mm");
    }
    if (m_endDatePicker != nullptr)
    {
        DateTime_SetFormat(m_endDatePicker, L"yyyy-MM-dd");
    }
    if (m_endTimePicker != nullptr)
    {
        DateTime_SetFormat(m_endTimePicker, L"HH:mm");
    }
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
    const auto language = GetSelectedLanguage();
    while (Header_GetItemCount(ListView_GetHeader(m_list)) > 0)
    {
        ListView_DeleteColumn(m_list, 0);
    }

    if (view_mode == ViewMode::Realtime)
    {
        for (size_t i = 0; i < kRealtimeColumns.size(); ++i)
        {
            AddColumn(
                m_list,
                static_cast<int>(i),
                m_realtimeColumnWidths[i],
                GetLocalizedText(language, kRealtimeColumns[i].title),
                kRealtimeColumns[i].format);
        }
    }
    else
    {
        for (size_t i = 0; i < kTotalColumns.size(); ++i)
        {
            AddColumn(
                m_list,
                static_cast<int>(i),
                m_totalColumnWidths[i],
                GetLocalizedText(language, kTotalColumns[i].title),
                kTotalColumns[i].format);
        }
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
        UpsertListRow(static_cast<int>(i), GetIconIndex(apps[i].exeName, apps[i].exePath), BuildRealtimeRow(apps[i]));
    }
}

void CTrafficDetailWindow::FillTotalView()
{
    auto apps = m_plugin.BuildHistoryApps(GetSelectedRange());
    std::sort(apps.begin(), apps.end(), [](const CProcNetPlugin::AppTrafficEntry& left, const CProcNetPlugin::AppTrafficEntry& right) {
        return (left.rxTotalBytes + left.txTotalBytes) > (right.rxTotalBytes + right.txTotalBytes);
    });

    EnsureListItemCount(static_cast<int>(apps.size()));
    for (size_t i = 0; i < apps.size(); ++i)
    {
        UpsertListRow(static_cast<int>(i), GetIconIndex(apps[i].exeName, apps[i].exePath), BuildTotalRow(apps[i]));
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
    LayoutTopControls(width);
    LayoutBottomControls(width, height);
    LayoutListControl(width, height);
}

void CTrafficDetailWindow::ClearList()
{
    ListView_DeleteAllItems(m_list);
}

void CTrafficDetailWindow::SetListText(int row, int column, const wchar_t* text)
{
    ListView_SetItemText(m_list, row, column, const_cast<LPWSTR>(text));
}

void CTrafficDetailWindow::SetWindowTextIfPresent(HWND control, const wchar_t* text) const
{
    if (control != nullptr)
    {
        SetWindowTextW(control, text);
    }
}

void CTrafficDetailWindow::LayoutTopControls(int width)
{
    const int language_left = kMargin + kButtonWidth + 12;
    const int right_button_x = width - kMargin - kButtonWidth;
    MoveControlIfPresent(m_toggleViewButton, kMargin, kMargin, kButtonWidth, kButtonHeight);
    MoveControlIfPresent(m_languageLabel, language_left, kMargin + 4, kLanguageLabelWidth, 20);
    MoveControlIfPresent(m_languageCombo, language_left + kLanguageLabelWidth, kMargin, kLanguageComboWidth, 300);
    MoveControlIfPresent(m_pauseRefreshButton, right_button_x, kMargin, kButtonWidth, kButtonHeight);
}

void CTrafficDetailWindow::LayoutBottomControls(int width, int height)
{
    if (m_viewMode == ViewMode::Realtime)
    {
        return;
    }

    const int range_block_top = height - kMargin - kSummaryHeight;
    const int range_block_left = kMargin + kSummaryWidth + 16;
    const int picker_left = range_block_left + kRangeLabelWidth;
    const int quick_button_left = width - kMargin - kQuickButtonWidth;
    MoveControlIfPresent(m_summary, kMargin, range_block_top, kSummaryWidth, kSummaryHeight);
    MoveControlIfPresent(m_startLabel, range_block_left, range_block_top + 4, kRangeLabelWidth, 20);
    MoveControlIfPresent(m_startDatePicker, picker_left, range_block_top, kRangeDateWidth, 28);
    MoveControlIfPresent(m_startTimePicker, picker_left + kRangeDateWidth + 8, range_block_top, kRangeTimeWidth, 28);
    MoveControlIfPresent(m_endLabel, range_block_left, range_block_top + kRangeRowSpacing + 4, kRangeLabelWidth, 20);
    MoveControlIfPresent(m_endDatePicker, picker_left, range_block_top + kRangeRowSpacing, kRangeDateWidth, 28);
    MoveControlIfPresent(m_endTimePicker, picker_left + kRangeDateWidth + 8, range_block_top + kRangeRowSpacing, kRangeTimeWidth, 28);
    MoveControlIfPresent(m_dayRangeButton, quick_button_left, range_block_top, kQuickButtonWidth, kQuickButtonHeight);
    MoveControlIfPresent(m_monthRangeButton, quick_button_left, range_block_top + kRangeRowSpacing, kQuickButtonWidth, kQuickButtonHeight);
    MoveControlIfPresent(m_yearRangeButton, quick_button_left, range_block_top + kRangeRowSpacing * 2, kQuickButtonWidth, kQuickButtonHeight);
}

void CTrafficDetailWindow::LayoutListControl(int width, int height)
{
    const int list_width = width - kMargin * 2;
    if (m_viewMode == ViewMode::Realtime)
    {
        MoveControlIfPresent(m_list, kMargin, kTopAreaHeight, list_width, height - kTopAreaHeight - kMargin);
        return;
    }

    MoveControlIfPresent(m_list, kMargin, kTopAreaHeight, list_width, height - kTopAreaHeight - kSummaryHeight - kMargin * 2);
}

bool CTrafficDetailWindow::IsInteractiveControlActive() const
{
    return (m_languageCombo != nullptr && SendMessageW(m_languageCombo, CB_GETDROPPEDSTATE, 0, 0) != 0) ||
        GetFocus() == m_startDatePicker ||
        GetFocus() == m_startTimePicker ||
        GetFocus() == m_endDatePicker ||
        GetFocus() == m_endTimePicker;
}

CHistoryTrafficStore::DateTimeRange CTrafficDetailWindow::GetSelectedRange() const
{
    auto range = m_plugin.GetPreferredRange();
    ApplyDateFromPicker(m_startDatePicker, range.start);
    ApplyTimeFromPicker(m_startTimePicker, range.start);
    ApplyDateFromPicker(m_endDatePicker, range.end);
    ApplyTimeFromPicker(m_endTimePicker, range.end);
    range.start.wSecond = 0;
    range.start.wMilliseconds = 0;
    range.end.wSecond = 0;
    range.end.wMilliseconds = 0;
    return range;
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

bool CTrafficDetailWindow::HandleCommand(WORD command_id, WORD notify_code)
{
    TMPluginDetail::IgnoreUnused(notify_code);

    if (command_id == kToggleViewButtonId)
    {
        ToggleView();
        return true;
    }
    if (command_id == kPauseRefreshButtonId)
    {
        ToggleRefreshPaused();
        return true;
    }
    if (command_id == kDayRangeButtonId || command_id == kMonthRangeButtonId || command_id == kYearRangeButtonId)
    {
        ApplyQuickRange(command_id);
        RefreshView();
        return true;
    }
    if (command_id == kLanguageComboId && notify_code == CBN_SELCHANGE)
    {
        m_plugin.SetPreferredLanguage(GetSelectedLanguage());
        RefreshView();
        return true;
    }

    return false;
}

bool CTrafficDetailWindow::HandleNotify(NMHDR* header)
{
    if (header == nullptr)
    {
        return false;
    }

    if (IsRangeControlId(header->idFrom) && header->code == DTN_DATETIMECHANGE)
    {
        m_plugin.SetPreferredRange(GetSelectedRange());
        RefreshView();
        return true;
    }

    if (m_list != nullptr &&
        header->hwndFrom == ListView_GetHeader(m_list) &&
        header->code == HDN_ENDTRACKW)
    {
        SaveCurrentColumnWidths();
        return true;
    }

    return false;
}

bool CTrafficDetailWindow::HandleRefreshTimer(WPARAM timer_id)
{
    if (timer_id != kRefreshTimerId || m_refreshPaused)
    {
        return false;
    }

    if (IsInteractiveControlActive())
    {
        return true;
    }

    RefreshView();
    return true;
}

void CTrafficDetailWindow::OnWindowCreated(HWND hwnd)
{
    CreateChildControls(hwnd);
    SetTimer(hwnd, kRefreshTimerId, 1000, nullptr);
    RefreshView();
}

void CTrafficDetailWindow::OnWindowDestroyed(HWND hwnd)
{
    KillTimer(hwnd, kRefreshTimerId);
    if (m_smallImageList != nullptr)
    {
        ImageList_Destroy(m_smallImageList);
        m_smallImageList = nullptr;
    }
    m_iconIndexByKey.clear();
    m_defaultIconIndex = -1;
    ResetControlHandles();
}

void CTrafficDetailWindow::HideWindow()
{
    SaveCurrentColumnWidths();
    ShowWindow(m_hwnd, SW_HIDE);
}

void CTrafficDetailWindow::ResetControlHandles()
{
    m_hwnd = m_list = m_toggleViewButton = m_languageLabel = m_languageCombo = nullptr;
    m_pauseRefreshButton = m_startLabel = m_startDatePicker = m_startTimePicker = nullptr;
    m_endLabel = m_endDatePicker = m_endTimePicker = m_dayRangeButton = nullptr;
    m_monthRangeButton = m_yearRangeButton = m_summary = nullptr;
}

LRESULT CTrafficDetailWindow::HandleMessage(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param)
{
    switch (message)
    {
    case WM_CREATE:
        OnWindowCreated(hwnd);
        return 0;
    case WM_SIZE:
        ResizeChildren(LOWORD(l_param), HIWORD(l_param));
        return 0;
    case WM_TIMER:
        if (HandleRefreshTimer(w_param))
        {
            return 0;
        }
        break;
    case WM_COMMAND:
        if (HandleCommand(LOWORD(w_param), HIWORD(w_param)))
        {
            return 0;
        }
        break;
    case WM_NOTIFY:
        if (HandleNotify(reinterpret_cast<NMHDR*>(l_param)))
        {
            return 0;
        }
        break;
    case WM_CLOSE:
        TMPluginDetail::IgnoreUnused(hwnd);
        HideWindow();
        return 0;
    case WM_DESTROY:
        OnWindowDestroyed(hwnd);
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
