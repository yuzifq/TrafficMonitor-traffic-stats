#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "HybridTrafficCollector.h"
#include "HistoryTrafficStore.h"
#include "NetworkTrafficStore.h"
#include "PluginInterface.h"
#include "ProcessFinder.h"
#include "ProcNetItem.h"

class CTrafficDetailWindow;

class CProcNetPlugin final : public ITMPlugin
{
public:
    struct LocalizedText
    {
        const wchar_t* english;
        const wchar_t* chinese;
    };

    struct AppTrafficEntry
    {
        std::wstring exeName;
        std::wstring exePath;
        std::uint64_t rxBytesPerSec{};
        std::uint64_t txBytesPerSec{};
        std::uint64_t rxTotalBytes{};
        std::uint64_t txTotalBytes{};
        std::wstring identity;
    };

    struct TotalsSummary
    {
        CHistoryTrafficStore::TrafficAmount networkRange, networkAll, processRange, processAll;
        bool networkAvailable{}, processAvailable{}, processAllAvailable{};
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
    void OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data) override;

    std::vector<AppTrafficEntry> BuildAllApps() const;
    std::vector<AppTrafficEntry> BuildHistoryApps(const CHistoryTrafficStore::DateTimeRange& range) const;
    std::wstring BuildTotalsText(const CHistoryTrafficStore::DateTimeRange& range,
        CHistoryTrafficStore::DisplayLanguage language, TotalsSummary* summary = nullptr) const;
    std::wstring BuildLiveNetworkText(CHistoryTrafficStore::DisplayLanguage language) const;
    CNetworkTrafficCollector::Snapshot GetNetworkSnapshot() const;
    bool SetNetworkAdapter(const std::wstring& id);
    std::wstring BuildDiagnosticsText(CHistoryTrafficStore::DisplayLanguage language) const;
    bool ExportHistory(
        const CHistoryTrafficStore::DateTimeRange& range,
        const std::wstring& output_path,
        std::wstring& error_message, bool network = false) const;
    CHistoryTrafficStore::DateTimeRange GetPreferredRange() const;
    void SetPreferredRange(const CHistoryTrafficStore::DateTimeRange& range);
    CHistoryTrafficStore::DisplayLanguage GetPreferredLanguage() const;
    void SetPreferredLanguage(CHistoryTrafficStore::DisplayLanguage language);

private:
    CProcNetPlugin();
    ~CProcNetPlugin();

    static bool IsEnglish(CHistoryTrafficStore::DisplayLanguage language);
    static const wchar_t* GetLocalizedText(CHistoryTrafficStore::DisplayLanguage language, const LocalizedText& text);
    static const wchar_t* GetInfoText(PluginInfoIndex index, CHistoryTrafficStore::DisplayLanguage language);
    static void SetItemPair(CProcNetItem& down_item, CProcNetItem& up_item, const AppTrafficEntry* app, int index, bool english);
    static std::wstring BuildTooltipText(const std::vector<AppTrafficEntry>& apps, int visible_count, bool english, const std::wstring& status_text);
    void UpdateDisplayText(const std::vector<AppTrafficEntry>& apps);
    void UpdateHistory();
    void EnsureCollectorStarted();
    void EnsureHistoryInitialized();
    void EnsureDetailWindow();
    void ShowDetailWindow(HWND parent);

private:
    static constexpr int kMaxApps = 5;

    std::vector<CProcNetItem> m_items;
    CHybridTrafficCollector m_collector;
    CHistoryTrafficStore m_historyStore;
    CNetworkTrafficCollector m_networkCollector;
    CNetworkTrafficStore m_networkStore;
    mutable std::mutex m_historyMutex;
    std::wstring m_tooltip;
    bool m_collectorStarted;
    bool m_historyInitialized;
    std::uint64_t m_lastHistorySampleSequence{};
    std::uint64_t m_historyWriteFailures{};
    std::unique_ptr<CTrafficDetailWindow> m_detailWindow;
};
