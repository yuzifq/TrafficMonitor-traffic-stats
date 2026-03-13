#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "EtwProcNetCollector.h"
#include "HistoryTrafficStore.h"
#include "PluginInterface.h"
#include "ProcNetItem.h"

class CTrafficDetailWindow;

class CProcNetPlugin final : public ITMPlugin
{
public:
    struct AppTrafficEntry
    {
        std::wstring exeName;
        std::wstring exePath;
        std::uint64_t rxBytesPerSec{};
        std::uint64_t txBytesPerSec{};
        std::uint64_t rxTotalBytes{};
        std::uint64_t txTotalBytes{};
    };

    static CProcNetPlugin& Instance();

    int GetAPIVersion() const override;
    IPluginItem* GetItem(int index) override;
    void DataRequired() override;
    const wchar_t* GetInfo(PluginInfoIndex index) override;
    const wchar_t* GetTooltipInfo() override;
    int GetCommandCount() override;
    const wchar_t* GetCommandName(int command_index) override;
    void OnPluginCommand(int command_index, void* hWnd, void* para) override;
    void OnInitialize(ITrafficMonitor* pApp) override;
    void OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data) override;

    std::vector<AppTrafficEntry> BuildAllApps() const;
    std::vector<AppTrafficEntry> BuildHistoryApps(const CHistoryTrafficStore::DateTimeRange& range) const;
    std::wstring BuildTotalsText(const CHistoryTrafficStore::DateTimeRange& range, CHistoryTrafficStore::DisplayLanguage language) const;
    CHistoryTrafficStore::DateTimeRange GetPreferredRange() const;
    void SetPreferredRange(const CHistoryTrafficStore::DateTimeRange& range);
    CHistoryTrafficStore::DisplayLanguage GetPreferredLanguage() const;
    void SetPreferredLanguage(CHistoryTrafficStore::DisplayLanguage language);

private:
    CProcNetPlugin();
    ~CProcNetPlugin();

    void UpdateDisplayText();
    void UpdateHistory(const std::vector<AppTrafficEntry>& apps);
    void EnsureHistoryInitialized();
    void ShowDetailWindow(HWND parent);

private:
    static constexpr int kMaxApps = 5;

    std::vector<CProcNetItem> m_items;
    CEtwProcNetCollector m_collector;
    CHistoryTrafficStore m_historyStore;
    ITrafficMonitor* m_app;
    std::wstring m_tooltip;
    std::wstring m_configDir;
    bool m_collectorStarted;
    bool m_historyInitialized;
    std::unique_ptr<CTrafficDetailWindow> m_detailWindow;
};
