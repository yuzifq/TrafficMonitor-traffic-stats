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
    UpdateDisplayText();
}

const wchar_t* CProcNetPlugin::GetInfo(PluginInfoIndex index)
{
    const bool english = m_historyStore.GetPreferredLanguage() == CHistoryTrafficStore::DisplayLanguage::English;
    switch (index)
    {
    case TMI_NAME:
        return english ? L"Process Traffic Stats" : L"进程流量统计";
    case TMI_DESCRIPTION:
        return english ? L"Shows real-time process traffic rankings and a detailed traffic window."
                       : L"显示实时流量排行，并提供流量详情窗口。";
    case TMI_AUTHOR:
        return L"yuzifq";
    case TMI_COPYRIGHT:
        return english ? L"Copyright (c) 2026 yuzifq" : L"Copyright (c) 2026 yuzifq";
    case TMI_VERSION:
        return L"0.1.0";
    case TMI_URL:
        return L"https://github.com/zhongyang219/TrafficMonitor";
    case TMI_API_VERSION:
        return L"7";
    default:
        return L"";
    }
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

    return m_historyStore.GetPreferredLanguage() == CHistoryTrafficStore::DisplayLanguage::English
        ? L"Traffic Details"
        : L"流量详情";
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
    const auto processes = CProcessFinder::EnumerateProcesses();

    std::unordered_map<DWORD, std::wstring> name_by_pid;
    name_by_pid.reserve(processes.size());
    for (const auto& process : processes)
    {
        name_by_pid[process.pid] = process.exeName;
    }

    std::unordered_map<std::wstring, std::wstring> path_by_name;
    path_by_name.reserve(processes.size());
    for (const auto& process : processes)
    {
        if (!process.exePath.empty() && path_by_name.find(process.exeName) == path_by_name.end())
        {
            path_by_name.emplace(process.exeName, process.exePath);
        }
    }

    std::unordered_map<std::wstring, AppTrafficEntry> apps_by_name;
    for (const auto& entry : snapshot)
    {
        const auto it = name_by_pid.find(entry.first);
        std::wstring exe_name = it != name_by_pid.end() ? it->second : (L"PID" + std::to_wstring(entry.first));
        auto& app = apps_by_name[exe_name];
        app.exeName = exe_name;
        const auto path_it = path_by_name.find(exe_name);
        if (app.exePath.empty() && path_it != path_by_name.end())
        {
            app.exePath = path_it->second;
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

std::vector<CProcNetPlugin::AppTrafficEntry> CProcNetPlugin::BuildHistoryApps(CHistoryTrafficStore::PeriodMode period_mode) const
{
    std::vector<AppTrafficEntry> result;
    const auto history_apps = m_historyStore.GetPeriodAppTotals(period_mode);
    result.reserve(history_apps.size());
    for (const auto& app : history_apps)
    {
        AppTrafficEntry entry{};
        entry.exeName = app.appName;
        entry.exePath = CProcessFinder::FindFirstProcessPathByExeName(app.appName);
        entry.rxTotalBytes = app.rxTotalBytes;
        entry.txTotalBytes = app.txTotalBytes;
        result.push_back(std::move(entry));
    }
    return result;
}

std::wstring CProcNetPlugin::BuildTotalsText(CHistoryTrafficStore::PeriodMode period_mode, CHistoryTrafficStore::DisplayLanguage language) const
{
    const auto selected = m_historyStore.GetPeriodTotal(period_mode);
    const auto all = m_historyStore.GetAllTimeTotal();
    const auto selected_total = selected.rxBytes + selected.txBytes;
    const auto all_total = all.rxBytes + all.txBytes;

    const bool english = language == CHistoryTrafficStore::DisplayLanguage::English;
    std::wstring period_name = english ? L"Today" : L"今日";
    if (period_mode == CHistoryTrafficStore::PeriodMode::Month)
    {
        period_name = english ? L"This Month" : L"本月";
    }
    else if (period_mode == CHistoryTrafficStore::PeriodMode::Year)
    {
        period_name = english ? L"This Year" : L"本年";
    }

    std::wstring text;
    if (english)
    {
        text = period_name + L" Download: ";
        text += Utils::FormatBytes(selected.rxBytes);
        text += L"\r\n" + period_name + L" Upload: ";
        text += Utils::FormatBytes(selected.txBytes);
        text += L"\r\n" + period_name + L" Total: ";
        text += Utils::FormatBytes(selected_total);

        text += L"\r\n\r\nAll-Time Download: ";
        text += Utils::FormatBytes(all.rxBytes);
        text += L"\r\nAll-Time Upload: ";
        text += Utils::FormatBytes(all.txBytes);
        text += L"\r\nAll-Time Total: ";
        text += Utils::FormatBytes(all_total);

        text += L"\r\n\r\nStatus: ";
        text += m_collector.GetStatusText();
    }
    else
    {
        text = period_name + L"下载: ";
        text += Utils::FormatBytes(selected.rxBytes);
        text += L"\r\n" + period_name + L"上传: ";
        text += Utils::FormatBytes(selected.txBytes);
        text += L"\r\n" + period_name + L"总流量: ";
        text += Utils::FormatBytes(selected_total);

        text += L"\r\n\r\n历史累计下载: ";
        text += Utils::FormatBytes(all.rxBytes);
        text += L"\r\n历史累计上传: ";
        text += Utils::FormatBytes(all.txBytes);
        text += L"\r\n历史累计总流量: ";
        text += Utils::FormatBytes(all_total);

        text += L"\r\n\r\n当前状态: ";
        text += m_collector.GetStatusText();
    }
    return text;
}

CHistoryTrafficStore::PeriodMode CProcNetPlugin::GetPreferredPeriodMode() const
{
    return m_historyStore.GetPreferredPeriodMode();
}

void CProcNetPlugin::SetPreferredPeriodMode(CHistoryTrafficStore::PeriodMode period_mode)
{
    m_historyStore.SetPreferredPeriodMode(period_mode);
}

CHistoryTrafficStore::DisplayLanguage CProcNetPlugin::GetPreferredLanguage() const
{
    return m_historyStore.GetPreferredLanguage();
}

void CProcNetPlugin::SetPreferredLanguage(CHistoryTrafficStore::DisplayLanguage language)
{
    m_historyStore.SetPreferredLanguage(language);
}

void CProcNetPlugin::UpdateDisplayText()
{
    const auto apps = BuildAllApps();
    const auto visible_count = static_cast<int>(apps.size()) < kMaxApps ? static_cast<int>(apps.size()) : kMaxApps;
    const bool english = m_historyStore.GetPreferredLanguage() == CHistoryTrafficStore::DisplayLanguage::English;

    for (int i = 0; i < kMaxApps; ++i)
    {
        auto& down_item = m_items[static_cast<size_t>(i * 2)];
        auto& up_item = m_items[static_cast<size_t>(i * 2 + 1)];

        if (i < visible_count)
        {
            down_item.SetName(apps[static_cast<size_t>(i)].exeName + L" Down");
            up_item.SetName(apps[static_cast<size_t>(i)].exeName + L" Up");
            down_item.SetLabel(Utils::ShortLabel(apps[static_cast<size_t>(i)].exeName, true));
            up_item.SetLabel(Utils::ShortLabel(apps[static_cast<size_t>(i)].exeName, false));
            down_item.SetValue(Utils::FormatRate(apps[static_cast<size_t>(i)].rxBytesPerSec));
            up_item.SetValue(Utils::FormatRate(apps[static_cast<size_t>(i)].txBytesPerSec));
        }
        else
        {
            down_item.SetName(MakeItemName(i, true));
            up_item.SetName(MakeItemName(i, false));
            down_item.SetLabel(english ? L"App Dn" : L"应用下");
            up_item.SetLabel(english ? L"App Up" : L"应用上");
            down_item.SetValue(L"N/A");
            up_item.SetValue(L"N/A");
        }
    }

    m_tooltip = english ? L"Real-time traffic ranking" : L"实时流量排行";
    for (int i = 0; i < visible_count; ++i)
    {
        m_tooltip += L"\n";
        m_tooltip += std::to_wstring(i + 1);
        m_tooltip += L". ";
        m_tooltip += apps[static_cast<size_t>(i)].exeName;
        m_tooltip += english ? L" Down=" : L" 下载=";
        m_tooltip += Utils::FormatRate(apps[static_cast<size_t>(i)].rxBytesPerSec);
        m_tooltip += english ? L" Up=" : L" 上传=";
        m_tooltip += Utils::FormatRate(apps[static_cast<size_t>(i)].txBytesPerSec);
    }
    m_tooltip += english ? L"\nRight-click plugin menu: Traffic Details" : L"\n右键插件菜单: 流量详情";
    m_tooltip += english ? L"\nStatus: " : L"\n状态: ";
    m_tooltip += m_collector.GetStatusText();
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
