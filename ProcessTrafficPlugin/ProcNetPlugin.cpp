#include "ProcNetPlugin.h"

#include "ProcessFinder.h"
#include "TrafficDetailWindow.h"
#include "Utils.h"

#include <Windows.h>

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace
{
using LocalizedText = CProcNetPlugin::LocalizedText;

std::wstring MakeItemId(int index, bool is_download)
{
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"App%d%ls", index + 1, is_download ? L"Down" : L"Up");
    return buffer;
}

std::wstring MakeItemName(int index, bool is_download)
{
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"Top%d %ls", index + 1, is_download ? L"Down" : L"Up");
    return buffer;
}

void AppendLabeledTraffic(std::wstring& text, const wchar_t* label, std::uint64_t value)
{
    text += label;
    text += Utils::FormatBytes(value);
}

constexpr LocalizedText kPluginName{ L"Process Traffic Stats", L"进程流量统计" };
constexpr LocalizedText kPluginDescription{
    L"Shows real-time process traffic rankings and a detailed traffic window.",
    L"显示实时流量排行，并提供流量详情窗口。"
};
constexpr LocalizedText kCommandName{ L"Traffic Details", L"流量详情" };
constexpr LocalizedText kRangeDownload{ L"Range Download: ", L"区间下载: " };
constexpr LocalizedText kRangeUpload{ L"Range Upload: ", L"区间上传: " };
constexpr LocalizedText kRangeTotal{ L"Range Total: ", L"区间总流量: " };
constexpr LocalizedText kAllTimeDownload{ L"All-Time Download: ", L"历史累计下载: " };
constexpr LocalizedText kAllTimeUpload{ L"All-Time Upload: ", L"历史累计上传: " };
constexpr LocalizedText kAllTimeTotal{ L"All-Time Total: ", L"历史累计总流量: " };
constexpr LocalizedText kCurrentStatus{ L"Status: ", L"当前状态: " };
constexpr LocalizedText kTooltipTitle{ L"Real-time traffic ranking", L"实时流量排行" };
constexpr LocalizedText kTooltipDown{ L" Down=", L" 下载=" };
constexpr LocalizedText kTooltipUp{ L" Up=", L" 上传=" };
constexpr LocalizedText kTooltipMenuHint{ L"\nRight-click plugin menu: Traffic Details", L"\n右键插件菜单: 流量详情" };
constexpr LocalizedText kTooltipStatus{ L"\nStatus: ", L"\n状态: " };
constexpr LocalizedText kDefaultDownLabel{ L"App Dn", L"应用下" };
constexpr LocalizedText kDefaultUpLabel{ L"App Up", L"应用上" };
}

CProcNetPlugin& CProcNetPlugin::Instance()
{
    static CProcNetPlugin instance;
    return instance;
}

CProcNetPlugin::CProcNetPlugin()
    : m_app(nullptr),
      m_tooltip(L"采集器尚未启动。"),
      m_collectorStarted(false),
      m_historyInitialized(false),
      m_processCacheTick(0),
      m_detailWindow(std::make_unique<CTrafficDetailWindow>(*this))
{
    m_items.reserve(kMaxApps * 2);
    for (int i = 0; i < kMaxApps; ++i)
    {
        m_items.emplace_back(MakeItemName(i, true), MakeItemId(i, true), L"应用下");
        m_items.emplace_back(MakeItemName(i, false), MakeItemId(i, false), L"应用上");
    }
}

CProcNetPlugin::~CProcNetPlugin()
{
    m_collector.Stop();
}

int CProcNetPlugin::GetAPIVersion() const
{
    return 7;
}

IPluginItem* CProcNetPlugin::GetItem(int index)
{
    if (index < 0 || index >= static_cast<int>(m_items.size()))
    {
        return nullptr;
    }

    return &m_items[static_cast<size_t>(index)];
}

void CProcNetPlugin::DataRequired()
{
    if (!m_collectorStarted)
    {
        m_collectorStarted = m_collector.Start();
    }

    EnsureHistoryInitialized();
    const auto apps = BuildAllApps();
    UpdateHistory(apps);
    UpdateDisplayText(apps);
}

std::vector<CProcessFinder::ProcessEntry> CProcNetPlugin::GetCachedProcesses() const
{
    const auto now = GetTickCount64();
    std::lock_guard<std::mutex> lock(m_processCacheMutex);
    if (m_cachedProcesses.empty() || now - m_processCacheTick >= 3000)
    {
        m_cachedProcesses = CProcessFinder::EnumerateProcesses();
        m_processCacheTick = now;
    }
    return m_cachedProcesses;
}

std::unordered_map<std::wstring, std::wstring> CProcNetPlugin::BuildPathMapByName(const std::vector<CProcessFinder::ProcessEntry>& processes)
{
    std::unordered_map<std::wstring, std::wstring> path_by_name;
    path_by_name.reserve(processes.size());
    for (const auto& process : processes)
    {
        if (!process.exePath.empty() && path_by_name.find(process.exeName) == path_by_name.end())
        {
            path_by_name.emplace(process.exeName, process.exePath);
        }
    }
    return path_by_name;
}

CProcNetPlugin::AppTrafficEntry CProcNetPlugin::MakeAppEntry(
    const std::wstring& exe_name,
    const std::unordered_map<std::wstring, std::wstring>& path_by_name)
{
    AppTrafficEntry entry{};
    entry.exeName = exe_name;
    const auto path_it = path_by_name.find(exe_name);
    if (path_it != path_by_name.end())
    {
        entry.exePath = path_it->second;
    }
    return entry;
}

bool CProcNetPlugin::IsEnglish(CHistoryTrafficStore::DisplayLanguage language)
{
    return language == CHistoryTrafficStore::DisplayLanguage::English;
}

const wchar_t* CProcNetPlugin::GetLocalizedText(CHistoryTrafficStore::DisplayLanguage language, const LocalizedText& text)
{
    return IsEnglish(language) ? text.english : text.chinese;
}

const wchar_t* CProcNetPlugin::GetInfoText(PluginInfoIndex index, CHistoryTrafficStore::DisplayLanguage language)
{
    switch (index)
    {
    case TMI_NAME:
        return GetLocalizedText(language, kPluginName);
    case TMI_DESCRIPTION:
        return GetLocalizedText(language, kPluginDescription);
    case TMI_AUTHOR:
        return L"yuzifq";
    case TMI_COPYRIGHT:
        return L"Copyright (c) 2026 yuzifq";
    case TMI_VERSION:
        return L"0.1.2";
    case TMI_URL:
        return L"https://github.com/zhongyang219/TrafficMonitor";
    case TMI_API_VERSION:
        return L"7";
    default:
        return L"";
    }
}

const wchar_t* CProcNetPlugin::GetInfo(PluginInfoIndex index)
{
    return GetInfoText(index, m_historyStore.GetPreferredLanguage());
}

const wchar_t* CProcNetPlugin::GetTooltipInfo()
{
    return m_tooltip.c_str();
}

int CProcNetPlugin::GetCommandCount()
{
    return 1;
}

const wchar_t* CProcNetPlugin::GetCommandName(int command_index)
{
    if (command_index != 0)
    {
        return nullptr;
    }

    return GetLocalizedText(m_historyStore.GetPreferredLanguage(), kCommandName);
}

void CProcNetPlugin::OnPluginCommand(int command_index, void* hWnd, void* para)
{
    TMPluginDetail::IgnoreUnused(para);
    if (command_index == 0)
    {
        ShowDetailWindow(static_cast<HWND>(hWnd));
    }
}

void CProcNetPlugin::OnInitialize(ITrafficMonitor* pApp)
{
    m_app = pApp;
}

void CProcNetPlugin::OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data)
{
    if (index == EI_CONFIG_DIR && data != nullptr)
    {
        m_configDir = data;
        EnsureHistoryInitialized();
    }
}

std::vector<CProcNetPlugin::AppTrafficEntry> CProcNetPlugin::BuildAllApps() const
{
    const auto snapshot = m_collector.GetProcessTrafficSnapshot();
    const auto processes = GetCachedProcesses();

    std::unordered_map<DWORD, std::wstring> name_by_pid;
    name_by_pid.reserve(processes.size());
    for (const auto& process : processes)
    {
        name_by_pid[process.pid] = process.exeName;
    }
    const auto path_by_name = BuildPathMapByName(processes);

    std::unordered_map<std::wstring, AppTrafficEntry> apps_by_name;
    for (const auto& entry : snapshot)
    {
        const auto it = name_by_pid.find(entry.first);
        std::wstring exe_name = it != name_by_pid.end() ? it->second : (L"PID" + std::to_wstring(entry.first));
        auto& app = apps_by_name[exe_name];
        if (app.exeName.empty())
        {
            app = MakeAppEntry(exe_name, path_by_name);
        }
        app.rxBytesPerSec += entry.second.rxBytesPerSec;
        app.txBytesPerSec += entry.second.txBytesPerSec;
        app.rxTotalBytes += entry.second.rxTotalBytes;
        app.txTotalBytes += entry.second.txTotalBytes;
    }

    std::vector<AppTrafficEntry> result;
    result.reserve(apps_by_name.size());
    for (auto& entry : apps_by_name)
    {
        if (entry.second.rxBytesPerSec == 0 && entry.second.txBytesPerSec == 0 &&
            entry.second.rxTotalBytes == 0 && entry.second.txTotalBytes == 0)
        {
            continue;
        }
        result.push_back(std::move(entry.second));
    }

    std::sort(result.begin(), result.end(), [](const AppTrafficEntry& left, const AppTrafficEntry& right) {
        return (left.rxBytesPerSec + left.txBytesPerSec) > (right.rxBytesPerSec + right.txBytesPerSec);
    });

    return result;
}

std::vector<CProcNetPlugin::AppTrafficEntry> CProcNetPlugin::BuildHistoryApps(const CHistoryTrafficStore::DateTimeRange& range) const
{
    std::vector<AppTrafficEntry> result;
    const auto history_apps = m_historyStore.GetRangeAppTotals(range);
    const auto processes = GetCachedProcesses();
    const auto path_by_name = BuildPathMapByName(processes);

    result.reserve(history_apps.size());
    for (const auto& app : history_apps)
    {
        AppTrafficEntry entry = MakeAppEntry(app.appName, path_by_name);
        entry.rxTotalBytes = app.rxTotalBytes;
        entry.txTotalBytes = app.txTotalBytes;
        result.push_back(std::move(entry));
    }
    return result;
}

std::wstring CProcNetPlugin::BuildTotalsText(const CHistoryTrafficStore::DateTimeRange& range, CHistoryTrafficStore::DisplayLanguage language) const
{
    const auto selected = m_historyStore.GetRangeTotal(range);
    const auto all = m_historyStore.GetAllTimeTotal();
    const auto selected_total = selected.rxBytes + selected.txBytes;
    const auto all_total = all.rxBytes + all.txBytes;
    std::wstring text;
    AppendLabeledTraffic(text, GetLocalizedText(language, kRangeDownload), selected.rxBytes);
    text += L"\r\n";
    AppendLabeledTraffic(text, GetLocalizedText(language, kRangeUpload), selected.txBytes);
    text += L"\r\n";
    AppendLabeledTraffic(text, GetLocalizedText(language, kRangeTotal), selected_total);
    text += L"\r\n\r\n";
    AppendLabeledTraffic(text, GetLocalizedText(language, kAllTimeDownload), all.rxBytes);
    text += L"\r\n";
    AppendLabeledTraffic(text, GetLocalizedText(language, kAllTimeUpload), all.txBytes);
    text += L"\r\n";
    AppendLabeledTraffic(text, GetLocalizedText(language, kAllTimeTotal), all_total);
    text += L"\r\n\r\n";
    text += GetLocalizedText(language, kCurrentStatus);
    text += m_collector.GetStatusText();
    return text;
}

CHistoryTrafficStore::DateTimeRange CProcNetPlugin::GetPreferredRange() const
{
    return m_historyStore.GetPreferredRange();
}

void CProcNetPlugin::SetPreferredRange(const CHistoryTrafficStore::DateTimeRange& range)
{
    m_historyStore.SetPreferredRange(range);
}

CHistoryTrafficStore::DisplayLanguage CProcNetPlugin::GetPreferredLanguage() const
{
    return m_historyStore.GetPreferredLanguage();
}

void CProcNetPlugin::SetPreferredLanguage(CHistoryTrafficStore::DisplayLanguage language)
{
    m_historyStore.SetPreferredLanguage(language);
}

void CProcNetPlugin::SetItemPair(CProcNetItem& down_item, CProcNetItem& up_item, const AppTrafficEntry* app, int index, bool english)
{
    if (app != nullptr)
    {
        down_item.SetName(app->exeName + L" Down");
        up_item.SetName(app->exeName + L" Up");
        down_item.SetLabel(Utils::ShortLabel(app->exeName, true));
        up_item.SetLabel(Utils::ShortLabel(app->exeName, false));
        down_item.SetValue(Utils::FormatRate(app->rxBytesPerSec));
        up_item.SetValue(Utils::FormatRate(app->txBytesPerSec));
        return;
    }

    down_item.SetName(MakeItemName(index, true));
    up_item.SetName(MakeItemName(index, false));
    down_item.SetLabel(english ? kDefaultDownLabel.english : kDefaultDownLabel.chinese);
    up_item.SetLabel(english ? kDefaultUpLabel.english : kDefaultUpLabel.chinese);
    down_item.SetValue(L"N/A");
    up_item.SetValue(L"N/A");
}

std::wstring CProcNetPlugin::BuildTooltipText(const std::vector<AppTrafficEntry>& apps, int visible_count, bool english, const std::wstring& status_text)
{
    const auto language = english ? CHistoryTrafficStore::DisplayLanguage::English : CHistoryTrafficStore::DisplayLanguage::Chinese;
    std::wstring tooltip = GetLocalizedText(language, kTooltipTitle);
    for (int i = 0; i < visible_count; ++i)
    {
        tooltip += L"\n";
        tooltip += std::to_wstring(i + 1);
        tooltip += L". ";
        tooltip += apps[static_cast<size_t>(i)].exeName;
        tooltip += GetLocalizedText(language, kTooltipDown);
        tooltip += Utils::FormatRate(apps[static_cast<size_t>(i)].rxBytesPerSec);
        tooltip += GetLocalizedText(language, kTooltipUp);
        tooltip += Utils::FormatRate(apps[static_cast<size_t>(i)].txBytesPerSec);
    }
    tooltip += GetLocalizedText(language, kTooltipMenuHint);
    tooltip += GetLocalizedText(language, kTooltipStatus);
    tooltip += status_text;
    return tooltip;
}

void CProcNetPlugin::UpdateDisplayText(const std::vector<AppTrafficEntry>& apps)
{
    const auto visible_count = static_cast<int>(apps.size()) < kMaxApps ? static_cast<int>(apps.size()) : kMaxApps;
    const bool english = m_historyStore.GetPreferredLanguage() == CHistoryTrafficStore::DisplayLanguage::English;

    for (int i = 0; i < kMaxApps; ++i)
    {
        auto& down_item = m_items[static_cast<size_t>(i * 2)];
        auto& up_item = m_items[static_cast<size_t>(i * 2 + 1)];
        const AppTrafficEntry* app = i < visible_count ? &apps[static_cast<size_t>(i)] : nullptr;
        SetItemPair(down_item, up_item, app, i, english);
    }

    m_tooltip = BuildTooltipText(apps, visible_count, english, m_collector.GetStatusText());
}

void CProcNetPlugin::UpdateHistory(const std::vector<AppTrafficEntry>& apps)
{
    std::vector<CHistoryTrafficStore::AppTotalEntry> history_apps;
    history_apps.reserve(apps.size());
    for (const auto& app : apps)
    {
        CHistoryTrafficStore::AppTotalEntry entry{};
        entry.appName = app.exeName;
        entry.rxTotalBytes = app.rxTotalBytes;
        entry.txTotalBytes = app.txTotalBytes;
        history_apps.push_back(std::move(entry));
    }
    m_historyStore.Update(history_apps);
}

void CProcNetPlugin::EnsureHistoryInitialized()
{
    if (m_historyInitialized)
    {
        return;
    }

    m_historyStore.Initialize(m_configDir);
    m_historyInitialized = true;
}

void CProcNetPlugin::ShowDetailWindow(HWND parent)
{
    if (!m_collectorStarted)
    {
        m_collectorStarted = m_collector.Start();
    }
    EnsureHistoryInitialized();

    if (m_detailWindow != nullptr)
    {
        m_detailWindow->Show(parent);
    }
}
