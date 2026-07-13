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
#include <cwchar>
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
constexpr int kHidePidCheckId = 1016;
constexpr int kDayRangeButtonId = 1013;
constexpr int kMonthRangeButtonId = 1014;
constexpr int kYearRangeButtonId = 1015;
constexpr int kWeekRangeButtonId = 1017;
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
constexpr int kCheckBoxHeight = 22;

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
    columns.reserve(3);
    columns.push_back(NormalizeDisplayName(app.exeName));
    columns.push_back(Utils::FormatRate(app.rxBytesPerSec));
    columns.push_back(Utils::FormatRate(app.txBytesPerSec));
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

bool IsAnonymousPidName(const std::wstring& name)
{
    if (name.size() <= 3 || name.rfind(L"PID", 0) != 0)
    {
        return false;
    }

    for (size_t i = 3; i < name.size(); ++i)
    {
        if (!std::iswdigit(name[i]))
        {
            return false;
        }
    }
    return true;
}

void MoveSystemTimeBackDays(SYSTEMTIME& time, unsigned int days)
{
    FILETIME file_time{};
    if (days == 0 || SystemTimeToFileTime(&time, &file_time) == FALSE)
    {
        return;
    }

    ULARGE_INTEGER value{};
    value.LowPart = file_time.dwLowDateTime;
    value.HighPart = file_time.dwHighDateTime;
    value.QuadPart -= static_cast<ULONGLONG>(days) * 24ULL * 60ULL * 60ULL * 10000000ULL;
    file_time.dwLowDateTime = value.LowPart;
    file_time.dwHighDateTime = value.HighPart;
    FileTimeToSystemTime(&file_time, &time);
}

const std::array<ColumnDefinition, 4> kRealtimeColumns{ {
    { { L" ", L" " }, LVCFMT_CENTER },
    { { L"Program", L"ç¨‹åº" }, LVCFMT_LEFT },
    { { L"Download", L"å®æ—¶ä¸‹è½½" }, LVCFMT_LEFT },
    { { L"Upload", L"å®æ—¶ä¸Šä¼ " }, LVCFMT_LEFT },
} };

const std::array<ColumnDefinition, 5> kTotalColumns{ {
    { { L" ", L" " }, LVCFMT_CENTER },
    { { L"Program", L"ç¨‹åº" }, LVCFMT_LEFT },
    { { L"Download", L"ä¸‹è½½" }, LVCFMT_LEFT },
    { { L"Upload", L"ä¸Šä¼ " }, LVCFMT_LEFT },
    { { L"Total", L"æ€»æµé‡" }, LVCFMT_LEFT },
} };

LocalizedText GetWindowTitle(bool realtime_view)
{
    return realtime_view
        ? LocalizedText{ L"Real-Time Traffic", L"å®æ—¶æµé‡ç»Ÿè®¡" }
        : LocalizedText{ L"Total Traffic", L"æ€»æµé‡ç»Ÿè®¡" };
}

LocalizedText GetToggleButtonText(bool realtime_view)
{
    return realtime_view
        ? LocalizedText{ L"Show Total Traffic", L"æŸ¥çœ‹æ€»æµé‡" }
        : LocalizedText{ L"Back To Real-Time", L"è¿”å›å®æ—¶æµé‡" };
}

LocalizedText GetPauseButtonText(bool refresh_paused)
{
    return refresh_paused
        ? LocalizedText{ L"Resume Refresh", L"ç»§ç»­åˆ·æ–°ç•Œé¢" }
        : LocalizedText{ L"Pause Refresh", L"æš‚åœåˆ·æ–°ç•Œé¢" };
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
      m_hidePidCheck(nullptr),
      m_dayRangeButton(nullptr),
      m_weekRangeButton(nullptr),
      m_monthRangeButton(nullptr),
      m_yearRangeButton(nullptr),
      m_summary(nullptr),
      m_smallImageList(nullptr),
      m_viewMode(ViewMode::Realtime),
      m_lastBuiltView(ViewMode::Realtime),
      m_lastBuiltLanguage(CHistoryTrafficStore::DisplayLanguage::English),
      m_refreshPaused(false),
      m_hideAnonymousPidItems(true),
      m_sortColumn(-1),
      m_sortDirection(SortDirection::None),
      m_realtimeColumnWidths{ 26, 315, 250, 250 },
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
        L"å®æ—¶æµé‡ç»Ÿè®¡",
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
        L"BUTTON", L"æŸ¥çœ‹æ€»æµé‡", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        kMargin, kMargin, kButtonWidth, kButtonHeight, hwnd, kToggleViewButtonId);

    m_languageLabel = CreateChildWindow(
        L"STATIC", L"Language:", WS_CHILD | WS_VISIBLE,
        language_left, kMargin + 4, kLanguageLabelWidth, kLabelHeight, hwnd, kLanguageLabelId);

    m_languageCombo = CreateChildWindow(
        L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        language_left + kLanguageLabelWidth, kMargin, kLanguageComboWidth, kComboDropHeight, hwnd, kLanguageComboId);
    ComboBox_AddString(m_languageCombo, L"English");
    ComboBox_AddString(m_languageCombo, L"ä¸­æ–‡");
    ComboBox_SetCurSel(m_languageCombo, static_cast<int>(m_plugin.GetPreferredLanguage()));

    m_pauseRefreshButton = CreateChildWindow(
        L"BUTTON", L"æš‚åœåˆ·æ–°ç•Œé¢", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
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
    m_hidePidCheck = CreateChildWindow(
        L"BUTTON", L"Hide anonymous PID items", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        end_left, kMargin + kRangeRowSpacing * 2, 220, kCheckBoxHeight, hwnd, kHidePidCheckId);
    Button_SetCheck(m_hidePidCheck, BST_CHECKED);

    m_dayRangeButton = CreateChildWindow(
        L"BUTTON", L"æ—¥", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        quick_button_left, kMargin, kQuickButtonWidth, kQuickButtonHeight, hwnd, kDayRangeButtonId);
    m_weekRangeButton = CreateChildWindow(
        L"BUTTON", L"å‘¨", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        quick_button_left, kMargin + kRangeRowSpacing, kQuickButtonWidth, kQuickButtonHeight, hwnd, kWeekRangeButtonId);
    m_monthRangeButton = CreateChildWindow(
        L"BUTTON", L"æœˆ", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        quick_button_left, kMargin + kRangeRowSpacing * 2, kQuickButtonWidth, kQuickButtonHeight, hwnd, kMonthRangeButtonId);
    m_yearRangeButton = CreateChildWindow(
        L"BUTTON", L"å¹´", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        quick_button_left, kMargin + kRangeRowSpacing * 3, kQuickButtonWidth, kQuickButtonHeight, hwnd, kYearRangeButtonId);

    m_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        kMargin, kTopAreaHeight, 860, kListInitialHeight, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kListId)), GetModuleHandleW(nullptr), nullptr);
    ListView_SetExtendedListViewStyle(m_list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER | LVS_EX_Hã5¶‰Ëkºwµça•%½¸¡•á•}Á…Ñ ¤ì(€€€¥˜€¡¥½¸€ôô¹Õ±±ÁÑÈ€˜˜€…•á•}¹…µ”¹•µÁÑä ¤¤(€€€ì(€€€€€€€¥½¸€ô1½…‘Mµ…±±á•%½¸¡AÉ½•ÍÍ¥¹‘•Èèé¥¹‘¥ÉÍÑAÉ½•ÍÍA…Ñ¡	åá•9…µ”¡•á•}¹…µ”¤¤ì(€€€ô((€€€¥¹Ğ¥µ…•}¥¹‘•à€ôµ}‘•™…Õ±Ñ%½¹%¹‘•àì(€€€¥˜€¡¥½¸€„ô¹Õ±±ÁÑÈ¤(€€€ì(€€€€€€€¥µ…•}¥¹‘•à€ô‘‘%½¹Q½%µ…•1¥ÍĞ¡¥½¸¤ì(€€€ô((€€€µ}¥½¹%¹‘•á	å-•ä¹•µÁ±…”¡…¡•}­•ä°¥µ…•}¥¹‘•à¤ì(€€€É•ÑÕÉ¸¥µ…•}¥¹‘•àì)ô()¥¹ĞQÉ…™™¥•Ñ…¥±]¥¹‘½Üèé‘‘%½¹Q½%µ…•1¥ÍĞ¡!%=8¥½¸¤)ì(€€€¥˜€¡¥½¸€ôô¹Õ±±ÁÑÈ¤(€€€ì(€€€€€€€É•ÑÕÉ¸€´Äì(€€€ô((€€€½¹ÍĞ¥¹Ğ¥µ…•}¥¹‘•à€ôµ}Íµ…±±%µ…•1¥ÍĞ€„ô¹Õ±±ÁÑÈ€ü%µ…•1¥ÍÑ}‘‘%½¸¡µ}Íµ…±±%µ…•1¥ÍĞ°¥½¸¤€è€´Äì(€€€•ÍÑÉ½å%½¸¡¥½¸¤ì(€€€É•ÑÕÉ¸¥µ…•}¥¹‘•àì)ô()!%=8QÉ…™™¥•Ñ…¥±]¥¹‘½Üèé1½…‘Mµ…±±á•%½¸¡½¹ÍĞÍÑèéİÍÑÉ¥¹œ˜•á•}Á…Ñ ¤½¹ÍĞ)ì(€€€¥˜€¡•á•}Á…Ñ ¹•µÁÑä ¤¤(€€€ì(€€€€€€€É•ÑÕÉ¸¹Õ±±ÁÑÈì(€€€ô((€€€M!%1%9=\™¥±•}¥¹™½íôì(€€€¥˜€¡M!•Ñ¥±•%¹™½\¡•á•}Á…Ñ ¹}ÍÑÈ ¤°%1}QQI%	UQ}9=I50°€™™¥±•}¥¹™¼°Í¥é•½˜¡™¥±•}¥¹™¼¤°(€€€€€€€M!%}%=8ğM!%}M511%=8¤€ôô€À¤(€€€ì(€€€€€€€É•ÑÕÉ¸¹Õ±±ÁÑÈì(€€€ô((€€€É•ÑÕÉ¸™¥±•}¥¹™¼¹¡%½¸ì)ô()!%=8QÉ…™™¥•Ñ…¥±]¥¹‘½Üèé1½…‘•™…Õ±Ñá•%½¸ ¤½¹ÍĞ)ì(€€€M!%1%9=\™¥±•}¥¹™½íôì(€€€¥˜€¡M!•Ñ¥±•%¹™½\¡0ˆ¹•á”ˆ°%1}QQI%	UQ}9=I50°€™™¥±•}¥¹™¼°Í¥é•½˜¡™¥±•}¥¹™¼¤°(€€€€€€€M!%}UM%1QQI%	UQLğM!%}%=8ğM!%}M511%=8¤€ôô€À¤(€€€ì(€€€€€€€É•ÑÕÉ¸¹Õ±±ÁÑÈì(€€€ô((€€€É•ÑÕÉ¸™¥±•}¥¹™¼¹¡%½¸ì)ô()Ù½¥QÉ…™™¥•Ñ…¥±]¥¹‘½ÜèéI•Í¥é•¡¥±‘É•¸¡¥¹Ğİ¥‘Ñ °¥¹Ğ¡•¥¡Ğ¤)ì(€€€1…å½ÕÑQ½Á½¹ÑÉ½±Ì¡İ¥‘Ñ ¤ì(€€€1…å½ÕÑ	½ÑÑ½µ½¹ÑÉ½±Ì¡İ¥‘Ñ °¡•¥¡Ğ¤ì(€€€1…å½ÕÑ1¥ÍÑ½¹ÑÉ½°¡İ¥‘Ñ °¡•¥¡Ğ¤ì)ô()Ù½¥QÉ…™™¥•Ñ…¥±]¥¹‘½Üèé±•…É1¥ÍĞ ¤)ì(€€€1¥ÍÑY¥•İ}•±•Ñ•±±%Ñ•µÌ¡µ}±¥ÍĞ¤ì)ô()Ù½¥QÉ…™™¥•Ñ…¥±]¥¹‘½ÜèéM•Ñ1¥ÍÑQ•áĞ¡¥¹ĞÉ½Ü°¥¹Ğ½±Õµ¸°½¹ÍĞİ¡…É}Ğ¨Ñ•áĞ¤)ì(€€€1¥ÍÑY¥•İ}M•Ñ%Ñ•µQ•áĞ¡µ}±¥ÍĞ°É½Ü°½±Õµ¸°½¹ÍÑ}…ÍĞñ1A]MQHø¡Ñ•áĞ¤¤ì)ô()Ù½¥QÉ…™™¥•Ñ…¥±]¥¹‘½ÜèéM•Ñ]¥¹‘½İQ•áÑ%™AÉ•Í•¹Ğ¡!]9½¹ÑÉ½°°½¹ÍĞİ¡…É}Ğ¨Ñ•áĞ¤½¹ÍĞ)ì(€€€¥˜€¡½¹ÑÉ½°€„ô¹Õ±±ÁÑÈ¤(€€€ì(€€€€€€€M•Ñ]¥¹‘½İQ•áÑ\¡½¹ÑÉ½°°Ñ•áĞ¤ì(€€€ô)ô()Ù½¥QÉ…™™¥•Ñ…¥±]¥¹‘½ÜèéUÁ‘…Ñ•M½ÉÑMÑ…Ñ”¡¥¹Ğ±¥­•‘}½±Õµ¸¤)ì(€€€¥˜€¡±¥­•‘}½±Õµ¸€ğô€À¤(€€€ì(€€€€€€€É•ÑÕÉ¸ì(€€€ô((€€€½¹ÍĞ‰½½°Ñ•áÑ}½±Õµ¸€ô±¥­•‘}½±Õµ¸€ôô€Äì(€€€½¹ÍĞ‰½½°Ñ½Ñ…±}½±Õµ¹}¥¹}Ñ½Ñ…±}Ù¥•Ü€ô€¡µ}Ù¥•İ5½‘”€ôôY¥•İ5½‘”èéQ½Ñ…°€˜˜±¥­•‘}½±Õµ¸€ôô€Ğ¤ì((€€€¥˜€¡µ}Í½ÉÑ½±Õµ¸€„ô±¥­•‘}½±Õµ¸¤(€€€ì(€€€€€€€µ}Í½ÉÑ½±Õµ¸€ô±¥­•‘}½±Õµ¸ì(€€€€€€€¥˜€¡Ñ•áÑ}½±Õµ¸¤(€€€€€€€ì(€€€€€€€€€€€µ}Í½ÉÑ¥É•Ñ¥½¸€ôM½ÉÑ¥É•Ñ¥½¸èéÍ•¹‘¥¹œì(€€€€€€€ô(€€€€€€€•±Í”¥˜€¡Ñ½Ñ…±}½±Õµ¹}¥¹}Ñ½Ñ…±}Ù¥•Ü¤(€€€€€€€ì(€€€€€€€€€€€µ}Í½ÉÑ¥É•Ñ¥½¸€ôM½ÉÑ¥É•Ñ¥½¸èéÍ•¹‘¥¹œì(€€€€€€€ô(€€€€€€€•±Í”(€€€€€€€ì(€€€€€€€€€€€µ}Í½ÉÑ¥É•Ñ¥½¸€ôM½ÉÑ¥É•Ñ¥½¸èé•Í•¹‘¥¹œì(€€€€€€€ô(€€€€€€€É•ÑÕÉ¸ì(€€€ô((€€€Íİ¥Ñ €¡µ}Í½ÉÑ¥É•Ñ¥½¸¤(€€€ì(€€€…Í”M½ÉÑ¥É•Ñ¥½¸èé9½¹”è(€€€€€€€µ}Í½ÉÑ¥É•Ñ¥½¸€ôÑ•áÑ}½±Õµ¸€üM½ÉÑ¥É•Ñ¥½¸èéÍ•¹‘¥¹œ€èM½ÉÑ¥É•Ñ¥½¸èé•Í•¹‘¥¹œì(€€€€€€€‰É•…¬ì(€€€…Í”M½ÉÑ¥É•Ñ¥½¸èéÍ•¹‘¥¹œè(€€€€€€€¥˜€¡Ñ•áÑ}½±Õµ¸¤(€€€€€€€ì(€€€€€€€€€€€µ}Í½ÉÑ¥É•Ñ¥½¸€ôM½ÉÑ¥É•Ñ¥½¸èé•Í•¹‘¥¹œì(€€€€€€€ô(€€€€€€€•±Í”¥˜€¡Ñ½Ñ…±}½±Õµ¹}¥¹}Ñ½Ñ…±}Ù¥•Ü¤(€€€€€€€ì(€€€€€€€€€€€µ}Í½ÉÑ¥É•Ñ¥½¸€ôM½ÉÑ¥É•Ñ¥½¸èé9½¹”ì(€€€€€€€€€€€µ}Í½ÉÑ½±Õµ¸€ô€´Äì(€€€€€€€ô(€€€€€€€•±Í”(€€€€€€€ì(€€€€€€€€€€€µ}Í½ÉÑ¥É•Ñ¥½¸€ôM½ÉÑ¥É•Ñ¥½¸èé9½¹”ì(€€€€€€€€€€€µ}Í½ÉÑ½±Õµ¸€ô€´Äì(€€€€€€€ô(€€€€€€€‰É•…¬ì(€€€…Í”M½ÉÑ¥É•Ñ¥½¸èé•Í•¹‘¥¹œè(€€€€€€€¥˜€¡Ñ•áÑ}½±Õµ¸¤(€€€€€€€ì(€€€€€€€€€€€µ}Í½ÉÑ¥É•Ñ¥½¸€ôM½ÉÑ¥É•Ñ¥½¸èé9½¹”ì(€€€€€€€€€€€µ}Í½ÉÑ½±Õµ¸€ô€´Äì(€€€€€€€ô(€€€€€€€•±Í”(€€€€€€€ì(€€€€€€€€€€€µ}Í½ÉÑ¥É•Ñ¥½¸€ôM½ÉÑ¥É•Ñ¥½¸èéÍ•¹‘¥¹œì(€€€€€€€ô(€€€€€€€‰É•…¬ì(€€€ô)ô()Ù½¥QÉ…™™¥•Ñ…¥±]¥¹‘½ÜèéM½ÉÑÁÁÌ¡ÍÑèéÙ•Ñ½ÈñAÉ½9•ÑA±Õ¥¸èéÁÁQÉ…™™¥¹ÑÉäø˜…ÁÁÌ¤½¹ÍĞ)ì(€€€¥˜€¡µ}Í½ÉÑ¥É•Ñ¥½¸€ôôM½ÉÑ¥É•Ñ¥½¸èé9½¹”ñğµ}Í½ÉÑ½±Õµ¸€ğô€À¤(€€€ì(€€€€€€€¥˜€¡µ}Ù¥•İ5½‘”€ôôY¥•İ5½‘”èéQ½Ñ…°¤(€€€€€€€ì(€€€€€€€€€€€ÍÑèéÍ½ÉĞ¡…ÁÁÌ¹‰•¥¸ ¤°…ÁÁÌ¹•¹ ¤°mt¡½¹ÍĞAÉ½9•ÑA±Õ¥¸èéÁÁQÉ…™™¥¹ÑÉä˜±•™Ğ°½¹ÍĞAÉ½9•ÑA±Õ¥¸èéÁÁQÉ…™™¥¹ÑÉä˜É¥¡Ğ¤ì(€€€€€€€€€€€€€€€É•ÑÕÉ¸€¡±•™Ğ¹ÉáQ½Ñ…±	åÑ•Ì€¬±•™Ğ¹ÑáQ½Ñ…±	åÑ•Ì¤€ø€¡É¥¡Ğ¹ÉáQ½Ñ…±	åÑ•Ì€¬É¥¡Ğ¹ÑáQ½Ñ…±	åÑ•Ì¤ì(€€€€€€€€€€€ô¤ì(€€€€€€€ô(€€€€€€€É•ÑÕÉ¸ì(€€€ô((€€€½¹ÍĞ‰½½°…Í•¹‘¥¹œ€ôµ}Í½ÉÑ¥É•Ñ¥½¸€ôôM½ÉÑ¥É•Ñ¥½¸èéÍ•¹‘¥¹œì(€€€ÍÑèéÍ½ÉĞ¡…ÁÁÌ¹‰•¥¸ ¤°…ÁÁÌ¹•¹ ¤°mÑ¡¥Ì°…Í•¹‘¥¹t¡½¹ÍĞAÉ½9•ÑA±Õ¥¸èéÁÁQÉ…™™¥¹ÑÉä˜±•™Ğ°½¹ÍĞAÉ½9•ÑA±Õ¥¸èéÁÁQÉ…™™¥¹ÑÉä˜É¥¡Ğ¤ì(€€€€€€€¥¹Ğ½µÁ…É•}É•ÍÕ±Ğ€ô€Àì(€€€€€€€¥˜€¡µ}Í½ÉÑ½±Õµ¸€ôô€Ä¤(€€€€€€€ì(€€€€€€€€€€€½µÁ…É•}É•ÍÕ±Ğ€ô½µÁ…É•Q•áĞ¡9½Éµ…±¥é•¥ÍÁ±…å9…µ”¡±•™Ğ¹•á•9…µ”¤°9½Éµ…±¥é•¥ÍÁ±…å9…µ”¡É¥¡Ğ¹•á•9…µ”¤¤ì(€€€€€€€ô(€€€€€€€•±Í”¥˜€¡µ}Ù¥•İ5½‘”€ôôY¥•İ5½‘”èéI•…±Ñ¥µ”¤(€€€€€€€ì(€€€€€€€€€€€½¹ÍĞ…ÕÑ¼±•™Ñ}Ù…±Õ”€ô€¡µ}Í½ÉÑ½±Õµ¸€ôô€È¤€ü±•™Ğ¹Éá	åÑ•ÍA•ÉM•Œ€è±•™Ğ¹Ñá	åÑ•ÍA•ÉM•Œì(€€€€€€€€€€€½¹ÍĞ…ÕÑ¼É¥¡Ñ}Ù…±Õ”€ô€¡µ}Í½ÉÑ½±Õµ¸€ôô€È¤€üÉ¥¡Ğ¹Éá	åÑ•ÍA•ÉM•Œ€èÉ¥¡Ğ¹Ñá	åÑ•ÍA•ÉM•Œì(€€€€€€€€€€€½µÁ…É•}É•ÍÕ±Ğ€ô±•™Ñ}Ù…±Õ”€ğÉ¥¡Ñ}Ù…±Õ”€ü€´Ä€è€¡±•™Ñ}Ù…±Õ”€øÉ¥¡Ñ}Ù…±Õ”€ü€Ä€è€À¤ì(€€€€€€€ô(€€€€€€€•±Í”(€€€€€€€ì(€€€€€€€€€€€ÍÑèéÕ¥¹ĞØÑ}Ğ±•™Ñ}Ù…±Õ”€ô€Àì(€€€€€€€€€€€ÍÑèéÕ¥¹ĞØÑ}ĞÉ¥¡Ñ}Ù…±Õ”€ô€Àì(€€€€€€€€€€€¥˜€¡µ}Í½ÉÑ½±Õµ¸€ôô€È¤(€€€€€€€€€€€ì(€€€€€€€€€€€€€€€±•™Ñ}Ù…±Õ”€ô±•™Ğ¹ÉáQ½Ñ…±	åÑ•Ìì(€€€€€€€€€€€€€€€É¥¡Ñ}Ù…±Õ”€ôÉ¥¡Ğ¹ÉáQ½Ñ…±	åÑ•Ìì(€€€€€€€€€€€ô(€€€€€€€€€€€•±Í”¥˜€¡µ}Í½ÉÑ½±Õµ¸€ôô€Ì¤(€€€€€€€€€€€ì(€€€€€€€€€€€€€€€±•™Ñ}Ù…±Õ”€ô±•™Ğ¹ÑáQ½Ñ…±	åÑ•Ìì(€€€€€€€€€€€€€€€É¥¡Ñ}Ù…±Õ”€ôÉ¥¡Ğ¹ÑáQ½Ñ…±	åÑ•Ìì(€€€€€€€€€€€ô(€€€€€€€€€€€•±Í”(€€€€€€€€€€€ì(€€€€€€€€€€€€€€€±•™Ñ}Ù…±Õ”€ô±•™Ğ¹ÉáQ½Ñ…±	åÑ•Ì€¬±•™Ğ¹ÑáQ½Ñ…±	åÑ•Ìì(€€€€€€€€€€€€€€€É¥¡Ñ}Ù…±Õ”€ôÉ¥¡Ğ¹ÉáQ½Ñ…±	åÑ•Ì€¬É¥¡Ğ¹ÑáQ½Ñ…±	åÑ•Ìì(€€€€€€€€€€€ô(€€€€€€€€€€€½µÁ…É•}É•ÍÕ±Ğ€ô±•™Ñ}Ù…±Õ”€ğÉ¥¡Ñ}Ù…±Õ”€ü€´Ä€è€¡±•™Ñ}Ù…±Õ”€øÉ¥¡Ñ}Ù…±Õ”€ü€Ä€è€À¤ì(€€€€€€€ô((€€€€€€€¥˜€¡½µÁ…É•}É•ÍÕ±Ğ€ôô€À¤(€€€€€€€ì(€€€€€€€€€€€½µÁ…É•}É•ÍÕ±Ğ€ô½µÁ…É•Q•áĞ¡9½Éµ…±¥é•¥ÍÁ±…å9…µ”¡±•™Ğ¹•á•9…µ”¤°9½Éµ…±¥é•¥ÍÁ±…å9…µ”¡É¥¡Ğ¹•á•9…µ”¤¤ì(€€€€€€€ô(€€€€€€€É•ÑÕÉ¸…Í•¹‘¥¹œ€ü€¡½µÁ…É•}É•ÍÕ±Ğ€ğ€À¤€è€¡½µÁ…É•}É•ÍÕ±Ğ€ø€À¤ì(€€€ô¤ì)ô()¥¹ĞQÉ…™™¥•Ñ…¥±]¥¹‘½Üèé½µÁ…É•Q•áĞ¡½¹ÍĞÍÑèéİÍÑÉ¥¹œ˜±•™Ğ°½¹ÍĞÍÑèéİÍÑÉ¥¹œ˜É¥¡Ğ¤)ì(€€€É•ÑÕÉ¸}İÍ¥µÀ¡±•™Ğ¹}ÍÑÈ ¤°É¥¡Ğ¹}ÍÑÈ ¤¤ì)ô()Ù½¥QÉ…™™¥•Ñ…¥±]¥¹‘½Üèé1…å½ÕÑQ½Á½¹ÑÉ½±Ì¡¥¹Ğİ¥‘Ñ ¤)ì(€€€½¹ÍĞ¥¹Ğ±…¹Õ…•}±•™Ğ€ô­5…É¥¸€¬­	ÕÑÑ½¹]¥‘Ñ €¬€ÄÈì(€€€½¹ÍĞ¥¹ĞÉ¥¡Ñ}‰ÕÑÑ½¹}à€ôİ¥‘Ñ €´­5…É¥¸€´­	ÕÑÑ½¹]¥‘Ñ ì(€€€5½Ù•½¹ÑÉ½±%™AÉ•Í•¹Ğ¡µ}Ñ½±•Y¥•İ	ÕÑÑ½¸°­5…É¥¸°­5…É¥¸°­	ÕÑÑ½¹]¥‘Ñ °­	ÕÑÑ½¹!•¥¡Ğ¤ì(€€€5½Ù•½¹ÑÉ½±%™AÉ•Í•¹Ğ¡µ}±…¹Õ…•1…‰•°°±…¹Õ…•}±•™Ğ°­5…É¥¸€¬€Ğ°­1…¹Õ…•1…‰•±]¥‘Ñ °€ÈÀ¤ì(€€€5½Ù•½¹ÑÉ½±%™AÉ•Í•¹Ğ¡µ}±…¹Õ…•½µ‰¼°±…¹Õ…•}±•™Ğ€¬­1…¹Õ…•1…‰•±]¥‘Ñ °­5…É¥¸°­1…¹Õ…•½µ‰½]¥‘Ñ °€ÌÀÀ¤ì(€€€5½Ù•½¹ÑÉ½±%™AÉ•Í•¹Ğ¡µ}Á…ÕÍ•I•™É•Í¡	ÕÑÑ½¸°É¥¡Ñ}‰ÕÑÑ½¹}à°­5…É¥¸°­	ÕÑÑ½¹]¥‘Ñ °­	ÕÑÑ½¹!•¥¡Ğ¤ì)ô()Ù½¥QÉ…™™¥•Ñ…¥±]¥¹‘½Üèé1…å½ÕÑ	½ÑÑ½µ½¹ÑÉ½±Ì¡¥¹Ğİ¥‘Ñ °¥¹Ğ¡•¥¡Ğ¤)ì(€€€¥˜€¡µ}Ù¥•İ5½‘”€ôôY¥•İ5½‘”èéI•…±Ñ¥µ”¤(€€€ì(€€€€€€€É•ÑÕÉ¸ì(€€€ô((€€€½¹ÍĞ¥¹ĞÉ…¹•}‰±½­}Ñ½À€ô¡•¥¡Ğ€´­5…É¥¸€´­MÕµµ…Éå!•¥¡Ğì(€€€½¹ÍĞ¥¹ĞÉ…¹•}‰±½­}±•™Ğ€ô­5…É¥¸€¬­MÕµµ…Éå]¥‘Ñ €¬€ÄØì(€€€½¹ÍĞ¥¹ĞÁ¥­•É}±•™Ğ€ôÉ…¹•}‰±½­}±•™Ğ€¬­I…¹•1…‰•±]¥‘Ñ ì(€€€½¹ÍĞ¥¹ĞÅÕ¥­}‰ÕÑÑ½¹}±•™Ğ€ôİ¥‘Ñ €´­5…É¥¸€´­EÕ¥­	ÕÑÑ½¹]¥‘Ñ ì(€€€5½Ù•½¹ÑÉ½±%™AÉ•Í•¹Ğ¡µ}ÍÕµµ…Éä°­5…É¥¸°É…¹•}‰±½­}Ñ½À°­MÕµµ…Éå]¥‘Ñ °­MÕµµ…Éå!•¥¡Ğ¤ì(€€€5½Ù•½¹ÑÉ½±%™AÉ•Í•¹Ğ¡µ}ÍÑ…ÉÑ1…‰•°°É…¹•}‰±½­}±•™Ğ°É…¹•}‰±½­}Ñ½À€¬€Ğ°­I…¹•1…‰•±]¥‘Ñ °€ÈÀ¤ì(€€€5½Ù•½¹ÑÉ½±%™AÉ•Í•¹Ğ¡µ}ÍÑ…ÉÑ…Ñ•A¥­•È°Á¥­•É}±•™Ğ°É…¹•}‰±½­}Ñ½À°­I…¹•…Ñ•]¥‘Ñ °€Èà¤ì(€€€5½Ù•½¹ÑÉ½±%™AÉ•Í•¹Ğ¡µ}ÍÑ…ÉÑQ¥µ•A¥­•È°Á¥­•É}±•™Ğ€¬­I…¹•…Ñ•]¥‘Ñ €¬€à°É…¹•}‰±½­}Ñ½À°­I…¹•Q¥µ•]¥‘Ñ °€Èà¤ì(€€€5½Ù•½¹ÑÉ½±%™AÉ•Í•¹Ğ¡µ}•¹‘1…‰•°°É…¹•}‰±½­}±•™Ğ°É…¹•}‰±½­}Ñ½À€¬­I…¹•I½İMÁ…¥¹œ€¬€Ğ°­I…¹•1…‰•±]¥‘Ñ °€ÈÀ¤ì(€€€5½Ù•½¹ÑÉ½±%™AÉ•Í•¹Ğ¡µ}•¹‘…Ñ•A¥­•È°Á¥­•É}±•™Ğ°É…¹•}‰±½­}Ñ½À€¬­I…¹•I½İMÁ…¥¹œ°­I…¹•…Ñ•]¥‘Ñ °€Èà¤ì(€€€5½Ù•½¹ÑÉ½±%™AÉ•Í•¹Ğ¡µ}•¹‘Q¥µ•A¥­•È°Á¥­•É}±•™Ğ€¬­I…¹•…Ñ•]¥‘Ñ €¬€à°É…¹•}‰±½­}Ñ½À€¬­I…¹•I½İMÁ…¥¹œ°­I…¹•Q¥µ•]¥‘Ñ °€Èà¤ì(€€€5½Ù•½¹ÑÉ½±%™AÉ•Í•¹Ğ¡µ}¡¥‘•A¥‘¡•¬°É…¹•}‰±½­}±•™Ğ°É…¹•}‰±½­}Ñ½À€¬­I…¹•I½İMÁ…¥¹œ€¨€È€¬€È°€ÈÈÀ°­¡•­	½á!•¥¡Ğ¤ì(€€€5½Ù•½¹ÑÉ½±%™AÉ•Í•¹Ğ¡µ}‘…åI…¹•	ÕÑÑ½¸°ÅÕ¥­}‰ÕÑÑ½¹}±•™Ğ°É…¹•}‰±½­}Ñ½À°­EÕ¥­	ÕÑÑ½¹]¥‘Ñ °­EÕ¥­	ÕÑÑ½¹!•¥¡Ğ¤ì(€€€5½Ù•½¹ÑÉ½±%™AÉ•Í•¹Ğ¡µ}İ••­I…¹•	ÕÑÑ½¸°ÅÕ¥­}‰ÕÑÑ½¹}±•™Ğ°É…¹•}‰±½­}Ñ½À€¬­I…¹•I½İMÁ…¥¹œ°­EÕ¥­	ÕÑÑ½¹]¥‘Ñ °­EÕ¥­	ÕÑÑ½¹!•¥¡Ğ¤ì(€€€5½Ù•½¹ÑÉ½±%™AÉ•Í•¹Ğ¡µ}µ½¹Ñ¡I…¹•	ÕÑÑ½¸°ÅÕ¥­}‰ÕÑÑ½¹}±•™Ğ°É…¹•}‰±½­}Ñ½À€¬­I…¹•I½İMÁ…¥¹œ€¨€È°­EÕ¥­	ÕÑÑ½¹]¥‘Ñ °­EÕ¥­	ÕÑÑ½¹!•¥¡Ğ¤ì(€€€5½Ù•½¹ÑÉ½±%™AÉ•Í•¹Ğ¡µ}å•…ÉI…¹•	ÕÑÑ½¸°ÅÕ¥­}‰ÕÑÑ½¹}±•™Ğ°É…¹•}‰±½­}Ñ½À€¬­I…¹•I½İMÁ…¥¹œ€¨€Ì°­EÕ¥­	ÕÑÑ½¹]¥‘Ñ °­EÕ¥­	ÕÑÑ½¹!•¥¡Ğ¤ì)ô()Ù½¥QÉ…™™¥•Ñ…¥±]¥¹‘½Üèé1…å½ÕÑ1¥ÍÑ½¹ÑÉ½°¡¥¹Ğİ¥‘Ñ °¥¹Ğ¡•¥¡Ğ¤)ì(€€€½¹ÍĞ¥¹Ğ±¥ÍÑ}İ¥‘Ñ €ôİ¥‘Ñ €´­5…É¥¸€¨€Èì(€€€¥˜€¡µ}Ù¥•İ5½‘”€ôôY¥•İ5½‘”èéI•…±Ñ¥µ”¤(€€€ì(€€€€€€€5½Ù•½¹ÑÉ½±%™AÉ•Í•¹Ğ¡µ}±¥ÍĞ°­5…É¥¸°­Q½ÁÉ•…!•¥¡Ğ°±¥ÍÑ}İ¥‘Ñ °¡•¥¡Ğ€´­Q½ÁÉ•…!•¥¡Ğ€´­5…É¥¸¤ì(€€€€€€€É•ÑÕÉ¸ì(€€€ô((€€€5½Ù•½¹ÑÉ½±%™AÉ•Í•¹Ğ¡µ}±¥ÍĞ°­5…É¥¸°­Q½ÁÉ•…!•¥¡Ğ°±¥ÍÑ}İ¥‘Ñ °¡•¥¡Ğ€´­Q½ÁÉ•…!•¥¡Ğ€´­MÕµµ…Éå!•¥¡Ğ€´­5…É¥¸€¨€È¤ì)ô()‰½½°QÉ…™™¥•Ñ…¥±]¥¹‘½Üèé%Í%¹Ñ•É…Ñ¥Ù•½¹ÑÉ½±Ñ¥Ù” ¤½¹ÍĞ)ì(€€€É•ÑÕÉ¸€¡µ}±…¹Õ…•½µ‰¼€„ô¹Õ±±ÁÑÈ€˜˜M•¹‘5•ÍÍ…•\¡µ}±…¹Õ…•½µ‰¼°	}QI=AAMQQ°€À°€À¤€„ô€À¤ñğ(€€€€€€€•Ñ½ÕÌ ¤€ôôµ}ÍÑ…ÉÑ…Ñ•A¥­•Èñğ(€€€€€€€•Ñ½ÕÌ ¤€ôôµ}ÍÑ…ÉÑQ¥µ•A¥­•Èñğ(€€€€€€€•Ñ½ÕÌ ¤€ôôµ}•¹‘…Ñ•A¥­•Èñğ(€€€€€€€•Ñ½ÕÌ ¤€ôôµ}•¹‘Q¥µ•A¥­•Èì)ô()!¥ÍÑ½ÉåQÉ…™™¥MÑ½É”èé…Ñ•Q¥µ•I…¹”QÉ…™™¥•Ñ…¥±]¥¹‘½Üèé•ÑM•±•Ñ•‘I…¹” ¤½¹ÍĞ)ì(€€€…ÕÑ¼É…¹”€ôµ}Á±Õ¥¸¹•ÑAÉ•™•ÉÉ•‘I…¹” ¤ì(€€€ÁÁ±å…Ñ•É½µA¥­•È¡µ}ÍÑ…ÉÑ…Ñ•A¥­•È°É…¹”¹ÍÑ…ÉĞ¤ì(€€€ÁÁ±åQ¥µ•É½µA¥­•È¡µ}ÍÑ…ÉÑQ¥µ•A¥­•È°É…¹”¹ÍÑ…ÉĞ¤ì(€€€ÁÁ±å…Ñ•É½µA¥­•È¡µ}•¹‘…Ñ•A¥­•È°É…¹”¹•¹¤ì(€€€ÁÁ±åQ¥µ•É½µA¥­•È¡µ}•¹‘Q¥µ•A¥­•È°É…¹”¹•¹¤ì(€€€É…¹”¹ÍÑ…ÉĞ¹İM•½¹€ô€Àì(€€€É…¹”¹ÍÑ…ÉĞ¹İ5¥±±¥Í•½¹‘Ì€ô€Àì(€€€É…¹”¹•¹¹İM•½¹€ô€Àì(€€€É…¹”¹•¹¹İ5¥±±¥Í•½¹‘Ì€ô€Àì(€€€É•ÑÕÉ¸É…¹”ì)ô()!¥ÍÑ½ÉåQÉ…™™¥MÑ½É”èé¥ÍÁ±…å1…¹Õ…”QÉ…™™¥•Ñ…¥±]¥¹‘½Üèé•ÑM•±•Ñ•‘1…¹Õ…” ¤½¹ÍĞ)ì(€€€¥˜€¡µ}±…¹Õ…•½µ‰¼€ôô¹Õ±±ÁÑÈ¤(€€€ì(€€€€€€€É•ÑÕÉ¸!¥ÍÑ½ÉåQÉ…™™¥MÑ½É”èé¥ÍÁ±…å1…¹Õ…”èé¹±¥Í ì(€€€ô((€€€É•ÑÕÉ¸½µ‰½	½á}•ÑÕÉM•°¡µ}±…¹Õ…•½µ‰¼¤€ôô€Ä(€€€€€€€€ü!¥ÍÑ½ÉåQÉ…™™¥MÑ½É”èé¥ÍÁ±…å1…¹Õ…”èé¡¥¹•Í”(€€€€€€€€è!¥ÍÑ½ÉåQÉ…™™¥MÑ½É”èé¥ÍÁ±…å1…¹Õ…”èé¹±¥Í ì)ô()‰½½°QÉ…™™¥•Ñ…¥±]¥¹‘½Üèé!…¹‘±•½µµ…¹¡]=I½µµ…¹‘}¥°]=I¹½Ñ¥™å}½‘”¤)ì(€€€Q5A±Õ¥¹•Ñ…¥°èé%¹½É•U¹ÕÍ•¡¹½Ñ¥™å}½‘”¤ì((€€€¥˜€¡½µµ…¹‘}¥€ôô­Q½±•Y¥•İ	ÕÑÑ½¹%¤(€€€ì(€€€€€€€Q½±•Y¥•Ü ¤ì(€€€€€€€É•ÑÕÉ¸ÑÉÕ”ì(€€€ô(€€€¥˜€¡½µµ…¹‘}¥€ôô­A…ÕÍ•I•™É•Í¡	ÕÑÑ½¹%¤(€€€ì(€€€€€€€Q½±•I•™É•Í¡A…ÕÍ• ¤ì(€€€€€€€É•ÑÕÉ¸ÑÉÕ”ì(€€€ô(€€€¥˜€¡½µµ…¹‘}¥€ôô­…åI…¹•	ÕÑÑ½¹%ñğ½µµ…¹‘}¥€ôô­]••­I…¹•	ÕÑÑ½¹%ñğ(€€€€€€€½µµ…¹‘}¥€ôô­5½¹Ñ¡I…¹•	ÕÑÑ½¹%ñğ½µµ…¹‘}¥€ôô­e•…ÉI…¹•	ÕÑÑ½¹%¤(€€€ì(€€€€€€€ÁÁ±åEÕ¥­I…¹”¡½µµ…¹‘}¥¤ì(€€€€€€€I•™É•Í¡Y¥•Ü ¤ì(€€€€€€€É•ÑÕÉ¸ÑÉÕ”ì(€€€ô(€€€¥˜€¡½µµ…¹‘}¥€ôô­1…¹Õ…•½µ‰½%€˜˜¹½Ñ¥™å}½‘”€ôô	9}M1!9¤(€€€ì(€€€€€€€µ}Á±Õ¥¸¹M•ÑAÉ•™•ÉÉ•‘1…¹Õ…”¡•ÑM•±•Ñ•‘1…¹Õ…” ¤¤ì(€€€€€€€I•™É•Í¡Y¥•Ü ¤ì(€€€€€€€É•ÑÕÉ¸ÑÉÕ”ì(€€€ô(€€€¥˜€¡½µµ…¹‘}¥€ôô­!¥‘•A¥‘¡•­%¤(€€€ì(€€€€€€€µ}¡¥‘•¹½¹åµ½ÕÍA¥‘%Ñ•µÌ€ô€¡µ}¡¥‘•A¥‘¡•¬€„ô¹Õ±±ÁÑÈ€˜˜	ÕÑÑ½¹}•Ñ¡•¬¡µ}¡¥‘•A¥‘¡•¬¤€ôô	MQ}!-¤ì(€€€€€€€I•™É•Í¡Y¥•Ü ¤ì(€€€€€€€É•ÑÕÉ¸ÑÉÕ”ì(€€€ô((€€€É•ÑÕÉ¸™…±Í”ì)ô()‰½½°QÉ…™™¥•Ñ…¥±]¥¹‘½Üèé!…¹‘±•9½Ñ¥™ä¡95!H¨¡•…‘•È¤)ì(€€€¥˜€¡¡•…‘•È€ôô¹Õ±±ÁÑÈ¤(€€€ì(€€€€€€€É•ÑÕÉ¸™…±Í”ì(€€€ô((€€€¥˜€¡%ÍI…¹•½¹ÑÉ½±%¡¡•…‘•È´ù¥‘É½´¤€˜˜¡•…‘•È´ù½‘”€ôôQ9}QQ%5!9¤(€€€ì(€€€€€€€µ}Á±Õ¥¸¹M•ÑAÉ•™•ÉÉ•‘I…¹”¡•ÑM•±•Ñ•‘I…¹” ¤¤ì(€€€€€€€I•™É•Í¡Y¥•Ü ¤ì(€€€€€€€É•ÑÕÉ¸ÑÉÕ”ì(€€€ô((€€€¥˜€¡µ}±¥ÍĞ€„ô¹Õ±±ÁÑÈ€˜˜(€€€€€€€¡•…‘•È´ù¡İ¹‘É½´€ôô1¥ÍÑY¥•İ}•Ñ!•…‘•È¡µ}±¥ÍĞ¤€˜˜(€€€€€€€¡•…‘•È´ù½‘”€ôô!9}9QI-\¤(€€€ì(€€€€€€€M…Ù•ÕÉÉ•¹Ñ½±Õµ¹]¥‘Ñ¡Ì ¤ì(€€€€€€€É•ÑÕÉ¸ÑÉÕ”ì(€€€ô((€€€¥˜€¡¡•…‘•È´ù¥‘É½´€ôô­1¥ÍÑ%€˜˜¡•…‘•È´ù½‘”€ôô1Y9}=1U591%,¤(€€€ì(€€€€€€€½¹ÍĞ…ÕÑ¼¨¥¹™¼€ôÉ•¥¹Ñ•ÉÁÉ•Ñ}…ÍĞñ951%MQY%\¨ø¡¡•…‘•È¤ì(€€€€€€€UÁ‘…Ñ•M½ÉÑMÑ…Ñ”¡¥¹™¼´ù¥MÕ‰%Ñ•´¤ì(€€€€€€€I•™É•Í¡Y¥•Ü ¤ì(€€€€€€€É•ÑÕÉ¸ÑÉÕ”ì(€€€ô((€€€É•ÑÕÉ¸™…±Í”ì)ô()‰½½°QÉ…™™¥•Ñ…¥±]¥¹‘½Üèé!…¹‘±•I•™É•Í¡Q¥µ•È¡]AI4Ñ¥µ•É}¥¤)ì(€€€¥˜€¡Ñ¥µ•É}¥€„ô­I•™É•Í¡Q¥µ•É%ñğµ}É•™É•Í¡A…ÕÍ•ñğµ}Ù¥•İ5½‘”€ôôY¥•İ5½‘”èéQ½Ñ…°¤(€€€ì(€€€€€€€É•ÑÕÉ¸Ñ¥µ•É}¥€ôô­I•™É•Í¡Q¥µ•É%ì(€€€ô((€€€¥˜€¡%Í%¹Ñ•É…Ñ¥Ù•½¹ÑÉ½±Ñ¥Ù” ¤¤(€€€ì(€€€€€€€É•ÑÕÉ¸ÑÉÕ”ì(€€€ô((€€€I•™É•Í¡Y¥•Ü ¤ì(€€€É•ÑÕÉ¸ÑÉÕ”ì)ô()Ù½¥QÉ…™™¥•Ñ…¥±]¥¹‘½Üèé=¹]¥¹‘½İÉ•…Ñ•¡!]9¡İ¹¤)ì(€€€É•…Ñ•¡¥±‘½¹ÑÉ½±Ì¡¡İ¹¤ì(€€€M•ÑQ¥µ•È¡¡İ¹°­I•™É•Í¡Q¥µ•É%°€ÄÀÀÀ°¹Õ±±ÁÑÈ¤ì(€€€I•™É•Í¡Y¥•Ü ¤ì)ô()Ù½¥QÉ…™™¥•Ñ…¥±]¥¹‘½Üèé=¹]¥¹‘½İ•ÍÑÉ½å•¡!]9¡İ¹¤)ì(€€€-¥±±Q¥µ•È¡¡İ¹°­I•™É•Í¡Q¥µ•É%¤ì(€€€¥˜€¡µ}Íµ…±±%µ…•1¥ÍĞ€„ô¹Õ±±ÁÑÈ¤(€€€ì(€€€€€€€%µ…•1¥ÍÑ}•ÍÑÉ½ä¡µ}Íµ…±±%µ…•1¥ÍĞ¤ì(€€€€€€€µ}Íµ…±±%µ…•1¥ÍĞ€ô¹Õ±±ÁÑÈì(€€€ô(€€€µ}¥½¹%¹‘•á	å-•ä¹±•…È ¤ì(€€€µ}‘•™…Õ±Ñ%½¹%¹‘•à€ô€´Äì(€€€I•Í•Ñ½¹ÑÉ½±!…¹‘±•Ì ¤ì)ô()Ù½¥QÉ…™™¥•Ñ…¥±]¥¹‘½Üèé!¥‘•]¥¹‘½Ü ¤)ì(€€€M…Ù•ÕÉÉ•¹Ñ½±Õµ¹]¥‘Ñ¡Ì ¤ì(€€€M¡½İ]¥¹‘½Ü¡µ}¡İ¹°M]}!%¤ì)ô()Ù½¥QÉ…™™¥•Ñ…¥±]¥¹‘½ÜèéI•Í•Ñ½¹ÑÉ½±!…¹‘±•Ì ¤)ì(€€€µ}¡İ¹€ôµ}±¥ÍĞ€ôµ}Ñ½±•Y¥•İ	ÕÑÑ½¸€ôµ}±…¹Õ…•1…‰•°€ôµ}±…¹Õ…•½µ‰¼€ô¹Õ±±ÁÑÈì(€€€µ}Á…ÕÍ•I•™É•Í¡	ÕÑÑ½¸€ôµ}ÍÑ…ÉÑ1…‰•°€ôµ}ÍÑ…ÉÑ…Ñ•A¥­•È€ôµ}ÍÑ…ÉÑQ¥µ•A¥­•È€ô¹Õ±±ÁÑÈì(€€€µ}•¹‘1…‰•°€ôµ}•¹‘…Ñ•A¥­•È€ôµ}•¹‘Q¥µ•A¥­•È€ôµ}¡¥‘•A¥‘¡•¬€ô¹Õ±±ÁÑÈì(€€€µ}‘…åI…¹•	ÕÑÑ½¸€ôµ}İ••­I…¹•	ÕÑÑ½¸€ô¹Õ±±ÁÑÈì(€€€µ}µ½¹Ñ¡I…¹•	ÕÑÑ½¸€ôµ}å•…ÉI…¹•	ÕÑÑ½¸€ôµ}ÍÕµµ…Éä€ô¹Õ±±ÁÑÈì)ô()1IMU1PQÉ…™™¥•Ñ…¥±]¥¹‘½Üèé!…¹‘±•5•ÍÍ…”¡!]9¡İ¹°U%9Pµ•ÍÍ…”°]AI4İ}Á…É…´°1AI4±}Á…É…´¤)ì(€€€Íİ¥Ñ €¡µ•ÍÍ…”¤(€€€ì(€€€…Í”]5}IQè(€€€€€€€=¹]¥¹‘½İÉ•…Ñ•¡¡İ¹¤ì(€€€€€€€É•ÑÕÉ¸€Àì(€€€…Í”]5}M%iè(€€€€€€€I•Í¥é•¡¥±‘É•¸¡1=]=I¡±}Á…É…´¤°!%]=I¡±}Á…É…´¤¤ì(€€€€€€€É•ÑÕÉ¸€Àì(€€€…Í”]5}Q%5Hè(€€€€€€€¥˜€¡!…¹‘±•I•™É•Í¡Q¥µ•È¡İ}Á…É…´¤¤(€€€€€€€ì(€€€€€€€€€€€É•ÑÕÉ¸€Àì(€€€€€€€ô(€€€€€€€‰É•…¬ì(€€€…Í”]5}=559è(€€€€€€€¥˜€¡!…¹‘±•½µµ…¹¡1=]=I¡İ}Á…É…´¤°!%]=I¡İ}Á…É…´¤¤¤(€€€€€€€ì(€€€€€€€€€€€É•ÑÕÉ¸€Àì(€€€€€€€ô(€€€€€€€‰É•…¬ì(€€€…Í”]5}9=Q%dè(€€€€€€€¥˜€¡!…¹‘±•9½Ñ¥™ä¡É•¥¹Ñ•ÉÁÉ•Ñ}…ÍĞñ95!H¨ø¡±}Á…É…´¤¤¤(€€€€€€€ì(€€€€€€€€€€€É•ÑÕÉ¸€Àì(€€€€€€€ô(€€€€€€€‰É•…¬ì(€€€…Í”]5}Q1=1=IMQQ%è(€€€€€€€M•Ñ	­5½‘”¡É•¥¹Ñ•ÉÁÉ•Ñ}…ÍĞñ!ø¡İ}Á…É…´¤°QI9MAI9P¤ì(€€€€€€€É•ÑÕÉ¸É•¥¹Ñ•ÉÁÉ•Ñ}…ÍĞñ1IMU1Pø¡•ÑMåÍ½±½É	ÉÕÍ ¡=1=I}]%9=\¤¤ì(€€€…Í”]5}1=Mè(€€€€€€€Q5A±Õ¥¹•Ñ…¥°èé%¹½É•U¹ÕÍ•¡¡İ¹¤ì(€€€€€€€!¥‘•]¥¹‘½Ü ¤ì(€€€€€€€É•ÑÕÉ¸€Àì(€€€…Í”]5}MQI=dè(€€€€€€€=¹]¥¹‘½İ•ÍÑÉ½å•¡¡İ¹¤ì(€€€€€€€É•ÑÕÉ¸€Àì(€€€‘•™…Õ±Ğè(€€€€€€€‰É•…¬ì(€€€ô((€€€É•ÑÕÉ¸•™]¥¹‘½İAÉ½\¡¡İ¹°µ•ÍÍ…”°İ}Á…É…´°±}Á…É…´¤ì)ô()1IMU1P11	,QÉ…™™¥•Ñ…¥±]¥¹‘½ÜèéMÑ…Ñ¥]¹‘AÉ½Œ¡!]9¡İ¹°U%9Pµ•ÍÍ…”°]AI4İ}Á…É…´°1AI4±}Á…É…´¤)ì(€€€QÉ…™™¥•Ñ…¥±]¥¹‘½Ü¨Í•±˜€ô¹Õ±±ÁÑÈì(€€€¥˜€¡µ•ÍÍ…”€ôô]5}9IQ¤(€€€ì(€€€€€€€…ÕÑ¼¨É•…Ñ•}ÍÑÉÕĞ€ôÉ•¥¹Ñ•ÉÁÉ•Ñ}…ÍĞñIQMQIUQ\¨ø¡±}Á…É…´¤ì(€€€€€€€Í•±˜€ôÍÑ…Ñ¥}…ÍĞñQÉ…™™¥•Ñ…¥±]¥¹‘½Ü¨ø¡É•…Ñ•}ÍÑÉÕĞ´ù±ÁÉ•…Ñ•A…É…µÌ¤ì(€€€€€€€M•Ñ]¥¹‘½İ1½¹AÑÉ\¡¡İ¹°]1A}UMIQ°É•¥¹Ñ•ÉÁÉ•Ñ}…ÍĞñ1=9}AQHø¡Í•±˜¤¤ì(€€€€€€€Í•±˜´ùµ}¡İ¹€ô¡İ¹ì(€€€ô(€€€•±Í”(€€€ì(€€€€€€€Í•±˜€ôÉ•¥¹Ñ•ÉÁÉ•Ñ}…ÍĞñQÉ…™™¥•Ñ…¥±]¥¹‘½Ü¨ø¡•Ñ]¥¹‘½İ1½¹AÑÉ\¡¡İ¹°]1A}UMIQ¤¤ì(€€€ô((€€€¥˜€¡Í•±˜€„ô¹Õ±±ÁÑÈ¤(€€€ì(€€€€€€€É•ÑÕÉ¸Í•±˜´ù!…¹‘±•5•ÍÍ…”¡¡İ¹°µ•ÍÍ…”°İ}Á…É…´°±}Á…É…´¤ì(€€€ô((€€€É•ÑÕÉ¸•™]¥¹‘½İAÉ½\¡¡İ¹°µ•ÍÍ…”°İ}Á…É…´°±}Á…É…´¤ì)ô(