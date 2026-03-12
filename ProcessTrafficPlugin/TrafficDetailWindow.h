#pragma once

#include <Windows.h>
#include <CommCtrl.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "HistoryTrafficStore.h"

class CProcNetPlugin;

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

    void EnsureWindowClassRegistered();
    void CreateOrActivate(HWND parent);
    void EnsureCommonControlsInitialized();
    void RefreshView();
    void ToggleView();
    void ToggleRefreshPaused();
    void ApplyLanguageTexts();
    void UpdateWindowTitle();
    void UpdateButtonText();
    void EnsureColumnsForCurrentView();
    void RebuildColumnsForView(ViewMode view_mode);
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
    void ResizeChildren(int width, int height);
    void ClearList();
    void SetListText(int row, int column, const wchar_t* text);
    CHistoryTrafficStore::PeriodMode GetSelectedPeriodMode() const;
    CHistoryTrafficStore::DisplayLanguage GetSelectedLanguage() const;
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
    HWND m_periodLabel;
    HWND m_periodCombo;
    HWND m_summary;
    HIMAGELIST m_smallImageList;
    ViewMode m_viewMode;
    ViewMode m_lastBuiltView;
    CHistoryTrafficStore::DisplayLanguage m_lastBuiltLanguage;
    bool m_refreshPaused;
    std::vector<int> m_realtimeColumnWidths;
    std::vector<int> m_totalColumnWidths;
    std::unordered_map<std::wstring, int> m_iconIndexByKey;
    int m_defaultIconIndex;
};
