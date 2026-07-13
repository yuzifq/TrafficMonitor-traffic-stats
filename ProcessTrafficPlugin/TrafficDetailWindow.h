#pragma once

#include <Windows.h>
#include <CommCtrl.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "HistoryTrafficStore.h"
#include "ProcNetPlugin.h"

class CTrafficDetailWindow
{
public:
    explicit CTrafficDetailWindow(CProcNetPlugin& plugin);
    void Show(HWND parent);

private:
    enum class ViewMode
    {
        Realtime,
        Total
    };

    enum class SortDirection
    {
        None,
        Ascending,
        Descending
    };

    void EnsureWindowClassRegistered();
    void CreateOrActivate(HWND parent);
    void EnsureCommonControlsInitialized();
    void CreateChildControls(HWND hwnd);
    void SetAllControlFonts() const;
    void RefreshView();
    void ToggleView();
    void ToggleRefreshPaused();
    void ApplyLanguageTexts();
    void ApplyQuickRange(int button_id);
    void ApplyCurrentRangeToControls();
    void ShowTotalViewControls(bool show);
    void ShowPauseRefreshButton(bool show);
    void UpdateViewSpecificControls();
    void UpdateWindowTitle();
    void UpdateButtonText();
    void EnsureColumnsForCurrentView();
    void RebuildColumnsForView(ViewMode view_mode);
    std::wstring GetColumnTitleText(int column_index, const wchar_t* base_title) const;
    void SaveCurrentColumnWidths();
    void FillRealtimeView();
    void FillTotalView();
    void EnsureListItemCount(int item_count);
    void UpsertListRow(int row, int image_index, const std::vector<std::wstring>& columns);
    void EnsureImageList();
    int GetIconIndex(const std::wstring& exe_name, const std::wstring& exe_path);
    int AddIconToImageList(HICON icon);
    HICON LoadSmallExeIcon(const std::wstring& exe_path) const;
    HICON LoadDefaultExeIcon() const;
    void ApplyRangeControls();
    void ResizeChildren(int width, int height);
    void LayoutTopControls(int width);
    void LayoutBottomControls(int width, int height);
    void LayoutListControl(int width, int height);
    void ClearList();
    void SetListText(int row, int column, const wchar_t* text);
    void SetWindowTextIfPresent(HWND control, const wchar_t* text) const;
    void UpdateSortState(int clicked_column);
    void SortApps(std::vector<CProcNetPlugin::AppTrafficEntry>& apps) const;
    static int CompareText(const std::wstring& left, const std::wstring& right);
    bool IsInteractiveControlActive() const;
    CHistoryTrafficStore::DateTimeRange GetSelectedRange() const;
    CHistoryTrafficStore::DisplayLanguage GetSelectedLanguage() const;
    bool HandleCommand(WORD command_id, WORD notify_code);
    bool HandleNotify(NMHDR* header);
    bool HandleRefreshTimer(WPARAM timer_id);
    void OnWindowCreated(HWND hwnd);
    void OnWindowDestroyed(HWND hwnd);
    void HideWindow();
    void ResetControlHandles();
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param);

    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param);

private:
    CProcNetPlugin& m_plugin;
    HWND m_hwnd;
    HWND m_list;
    HWND m_toggleViewButton;
    HWND m_languageLabel;
    HWND m_languageCombo;
    HWND m_pauseRefreshButton;
    HWND m_startLabel;
    HWND m_startDatePicker;
    HWND m_startTimePicker;
    HWND m_endLabel;
    HWND m_endDatePicker;
    HWND m_endTimePicker;
    HWND m_hidePidCheck;
    HWND m_dayRangeButton;
    HWND m_weekRangeButton;
    HWND m_monthRangeButton;
    HWND m_yearRangeButton;
    HWND m_summary;
    HIMAGELIST m_smallImageList;
    ViewMode m_viewMode;
    ViewMode m_lastBuiltView;
    CHistoryTrafficStore::DisplayLanguage m_lastBuiltLanguage;
    bool m_refreshPaused;
    bool m_hideAnonymousPidItems;
    int m_sortColumn;
    SortDirection m_sortDirection;
    std::vector<int> m_realtimeColumnWidths;
    std::vector<int> m_totalColumnWidths;
    std::unordered_map<std::wstring, int> m_iconIndexByKey;
    int m_defaultIconIndex;
};
