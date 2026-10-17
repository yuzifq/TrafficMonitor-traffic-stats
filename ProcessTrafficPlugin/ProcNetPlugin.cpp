#include "ProcNetPlugin.h"

#include "ProcessFinder.h"
#include "TrafficDetailWindow.h"
#include "Utils.h"

#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <unordered_map>
#include <utility>

namespace
{
using LocalizedText = CProcNetPlugin::LocalizedText;
using TrafficAmount = CHistoryTrafficStore::TrafficAmount;

const wchar_t* SelectLocalizedText(CHistoryTrafficStore::DisplayLanguage language, const LocalizedText& text)
{
    return language == CHistoryTrafficStore::DisplayLanguage::English ? text.english : text.chinese;
}

std::wstring MakeItemId(int index, bool is_download)
{
    return L"App" + std::to_wstring(index + 1) + (is_download ? L"Down" : L"Up");
}

std::wstring MakeItemName(int index, bool is_download)
{
    return L"Top" + std::to_wstring(index + 1) + (is_download ? L" Down" : L" Up");
}

void AppendLabeledTraffic(std::wstring& text, const wchar_t* label, std::uint64_t value)
{
    text += label;
    text += Utils::FormatBytes(value);
}

void AppendTrafficLine(std::wstring& text, const wchar_t* label, std::uint64_t value)
{
    AppendLabeledTraffic(text, label, value);
    text += L"\r\n";
}

void AppendTrafficBlock(
    std::wstring& text,
    CHistoryTrafficStore::DisplayLanguage language,
    const TrafficAmount& amount,
    const LocalizedText& download_label,
    const LocalizedText& upload_label,
    const LocalizedText& total_label)
{
    AppendTrafficLine(text, SelectLocalizedText(language, download_label), amount.rxBytes);
    AppendTrafficLine(text, SelectLocalizedText(language, upload_label), amount.txBytes);
    AppendLabeledTraffic(text, SelectLocalizedText(language, total_label), amount.rxBytes + amount.txBytes);
}

constexpr LocalizedText kPluginName{ L"Process Traffic Stats", L"进程流量统计" };
constexpr LocalizedText kPluginDescription{
    L"Shows real-time process traffic rankings and a detailed traffic window.",
    L"显示实时流量排行，并提供流量详情窗口。"
};
constexpr LocalizedText kCommandName{ L"Traffic Details", L"流量详情" };
constexpr LocalizedText kRangeDownload{ L"Process range download: ", L"进程区间下载: " };
constexpr LocalizedText kRangeUpload{ L"Process range upload: ", L"进程区间上传: " };
constexpr LocalizedText kRangeTotal{ L"Process range total: ", L"进程区间总量: " };
constexpr LocalizedText kAllTimeDownload{ L"Process all-time download: ", L"进程累计下载: " };
constexpr LocalizedText kAllTimeUpload{ L"Process all-time upload: ", L"进程累计上传: " };
constexpr LocalizedText kAllTimeTotal{ L"Process all-time total: ", L"进程累计总量: " };
constexpr LocalizedText kStatusRunning{ L"Running", L"运行中" };
constexpr LocalizedText kStatusStarting{ L"Starting", L"启动中" };
constexpr LocalizedText kStatusStopped{ L"Stopped", L"已停止" };
constexpr LocalizedText kStatusError{ L"Error", L"异常" };
constexpr LocalizedText kTooltipTitle{ L"Real-time traffic ranking", L"实时流量排行" };
constexpr LocalizedText kTooltipDown{ L" Down=", L" 下载=" };
constexpr LocalizedText kTooltipUp{ L" Up=", L" 上传=" };
constexpr LocalizedText kTooltipMenuHint{ L"\nRight-click plugin menu: Traffic Details", L"\n右键插件菜单: 流量详情" };
constexpr LocalizedText kTooltipStatus{ L"\nStatus: ", L"\n状态: " };
constexpr LocalizedText kDefaultDownLabel{ L"App Dn", L"应用下" };
constexpr LocalizedText kDefaultUpLabel{ L"App Up", L"应用上" };

const wchar_t* GetSimpleStatus(
    CHistoryTrafficStore::DisplayLanguage language,
    const std::wstring& collector_status)
{
    auto normalized = collector_status;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](wchar_t value) {
        return static_cast<wchar_t>(std::towlower(value));
    });
    if (normalized.find(L"failed") != std::wstring::npos ||
        normalized.find(L"error") != std::wstring::npos ||
        normalized.find(L"ended") != std::wstring::npos)
    {
        return SelectLocalizedText(language, kStatusError);
    }
    if (normalized.find(L"starting") != std::wstring::npos)
    {
        return SelectLocalizedText(language, kStatusStarting);
    }
    if (normalized.find(L"stopped") != std::wstring::npos)
    {
        return SelectLocalizedText(language, kStatusStopped);
    }
    return SelectLocalizedText(language, kStatusRunning);
}

const wchar_t* NetworkStatus(CNetworkTrafficCollector::Status status, CHistoryTrafficStore::DisplayLanguage language)
{
    switch (status)
    {
    case CNetworkTrafficCollector::Status::Running: return SelectLocalizedText(language, kStatusRunning);
    case CNetworkTrafficCollector::Status::Starting: return SelectLocalizedText(language, kStatusStarting);
    case CNetworkTrafficCollector::Status::Disconnected: return SelectLocalizedText(language, { L"Disconnected", L"未连接" });
    default: return SelectLocalizedText(language, kStatusError);
    }
}
}

CProcNetPlugin& CProcNetPlugin::Instance()
{
    static auto* instance = new CProcNetPlugin();
    return *instance;
}

CProcNetPlugin::CProcNetPlugin()
    : m_tooltip(L"采集器尚未启动。"),
      m_collectorStarted(false),
      m_historyInitialized(false)
{
    m_collector.SetCommitSink([](void* context) { static_cast<CProcNetPlugin*>(context)->UpdateHistory(); }, this);
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
    UpdateHistory();
    m_historyStore.Checkpoint(true);
    m_networkStore.Flush(true);
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
    EnsureHistoryInitialized();
    EnsureCollectorStarted();
    {
        std::lock_guard<std::mutex> lock(m_historyMutex);
        const auto interval = m_networkCollector.Sample();
        if (interval) m_networkStore.Add(*interval);
    }
    const auto apps = BuildAllApps();
    UpdateDisplayText(apps);
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
        return L"1.2.0";
    case TMI_URL:
        return L"https://github.com/yuzifq/TrafficMonitor-traffic-stats";
    case TMI_API_VERSION:
        return L"7";
    default:
        return L"";
    }
}

const wchar_t* CProcNetPlugin::GetInfo(PluginInfoIndex index)
{
    return GetInfoText(index, GetPreferredLanguage());
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

    return GetLocalizedText(GetPreferredLanguage(), kCommandName);
}

void CProcNetPlugin::OnPluginCommand(int command_index, void* hWnd, void* para)
{
    TMPluginDetail::IgnoreUnused(para);
    if (command_index == 0)
    {
        ShowDetailWindow(static_cast<HWND>(hWnd));
    }
}

void CProcNetPlugin::OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data)
{
    if (index == EI_CONFIG_DIR && data != nullptr)
    {
        EnsureHistoryInitialized();
    }
}

std::vector<CProcNetPlugin::AppTrafficEntry> CProcNetPlugin::BuildAllApps() const
{
    const auto snapshot = m_collector.GetSnapshot();
    std::vector<AppTrafficEntry> result;
    result.reserve(snapshot.size());
    for (const auto& sample : snapshot)
    {
        AppTrafficEntry app;
        app.exeName = sample.name;
        app.exePath = sample.path;
        app.identity = sample.identity;
        app.rxBytesPerSec = sample.rxPerSec;
        app.txBytesPerSec = sample.txPerSec;
        result.push_back(std::move(app));
    }

    std::sort(result.begin(), result.end(), [](const AppTrafficEntry& left, const AppTrafficEntry& right) {
        return (left.rxBytesPerSec + left.txBytesPerSec) > (right.rxBytesPerSec + right.txBytesPerSec);
    });

    return result;
}

std::vector<CProcNetPlugin::AppTrafficEntry> CProcNetPlugin::BuildHistoryApps(const CHistoryTrafficStore::DateTimeRange& range) const
{
    std::lock_guard<std::mutex> lock(m_historyMutex);
    std::vector<AppTrafficEntry> result;
    const auto history_apps = m_historyStore.GetRangeAppTotals(range);

    result.reserve(history_apps.size());
    for (const auto& app : history_apps)
    {
        AppTrafficEntry entry{};
        entry.exeName = app.appName;
        entry.exePath = app.exePath;
        entry.identity = app.identity;
        entry.rxTotalBytes = app.rxTotalBytes;
        entry.txTotalBytes = app.txTotalBytes;
        result.push_back(std::move(entry));
    }
    return result;
}

std::wstring CProcNetPlugin::BuildTotalsText(const CHistoryTrafficStore::DateTimeRange& range,
    CHistoryTrafficStore::DisplayLanguage language, TotalsSummary* summary) const
{
    std::lock_guard<std::mutex> lock(m_historyMutex);
    const auto all = m_historyStore.GetAllTimeTotal();
    const bool history_ready = m_historyStore.IsReady();
    const auto selected = history_ready ? m_historyStore.GetRangeTotal(range) : CHistoryTrafficStore::TrafficAmount{};
    const bool range_healthy = history_ready && m_historyStore.IsRangeHealthy(range);
    const bool all_readable = m_historyStore.IsAllTimeHealthy();
    const auto network = m_networkStore.GetRange(range);
    bool all_healthy{};
    const auto network_all = m_networkStore.GetAllTimeTotal(&all_healthy);
    if (summary) *summary = { network.amount, network_all, selected, all, network.available, range_healthy, all_readable };
    std::wstring text;
    if (network.available)
    {
        AppendTrafficBlock(text, language, network.amount,
            { L"Network range download: ", L"网卡区间下载: " },
            { L"Network range upload: ", L"网卡区间上传: " },
            { L"Network range total: ", L"网卡区间总量: " });
        text += GetLocalizedText(language, { L"\r\nSample coverage: ", L"\r\n采样覆盖: " });
        text += Utils::FormatPercent(network.coveredMs, network.requestedMs);
        if (network.coveredMs < network.requestedMs) text += GetLocalizedText(language, { L" (gaps)", L" (有缺失)" });
        if (network.coarse) text += GetLocalizedText(language, { L" (coarse intervals)", L" (含长采样区间)" });
        text += L"\r\n";
        text += GetLocalizedText(language, { L"Boundary D/U: ", L"边界待归属 下/上: " });
        text += Utils::FormatBytes(network.boundary.rxBytes) + L" / " + Utils::FormatBytes(network.boundary.txBytes);
    }
    else text = GetLocalizedText(language, { L"Network range: no samples", L"网卡区间: 尚无采样数据" });
    text += L"\r\n";
    text += GetLocalizedText(language, { L"Network all-time D/U: ", L"网卡累计 下/上: " });
    text += Utils::FormatBytes(network_all.rxBytes) + L" / " + Utils::FormatBytes(network_all.txBytes) + L"\r\n\r\n";
    if (range_healthy)
    {
        AppendTrafficBlock(text, language, selected, kRangeDownload, kRangeUpload, kRangeTotal);
    }
    else text += GetLocalizedText(language, { L"Process history unavailable: recovery pending", L"进程历史暂不可用: 等待安全恢复" });
    text += L"\r\n";
    if (all_readable) AppendTrafficBlock(text, language, all, kAllTimeDownload, kAllTimeUpload, kAllTimeTotal);
    text += L"\r\n";
    text += GetLocalizedText(language, { L"Network / ETW: ", L"网卡 / ETW: " });
    text += NetworkStatus(m_networkCollector.GetSnapshot().status, language);
    text += L" / ";
    text += GetSimpleStatus(language, m_collector.GetStatusText());
    if (!network.healthy || !all_healthy) text += GetLocalizedText(language, { L"; history error", L"；历史读写异常" });
    return text;
}

std::wstring CProcNetPlugin::BuildLiveNetworkText(CHistoryTrafficStore::DisplayLanguage language) const
{
    std::lock_guard<std::mutex> lock(m_historyMutex);
    const auto& live = m_networkCollector.GetSnapshot();
    std::wstring text = GetLocalizedText(language, { L"Network status: ", L"网卡状态: " });
    text += NetworkStatus(live.status, language);
    text += GetLocalizedText(language, { L"\r\nDownload: ", L"\r\n网卡下载: " });
    text += Utils::FormatRate(live.rxPerSec);
    text += GetLocalizedText(language, { L"   Upload: ", L"   网卡上传: " });
    text += Utils::FormatRate(live.txPerSec);
    if (!m_networkStore.Healthy()) text += GetLocalizedText(language, { L"   History write error", L"   历史保存异常" });
    return text;
}

std::wstring CProcNetPlugin::BuildDiagnosticsText(CHistoryTrafficStore::DisplayLanguage language) const
{
    const auto d = m_collector.GetDiagnostics();
    std::wstring text = GetLocalizedText(language,
        { L"Ledger: ETW + verified connection EStats\r\n", L"正式来源: ETW + 已验证连接的 EStats\r\n" });
    const auto line = [&](const LocalizedText& label, std::uint64_t value) {
        text += GetLocalizedText(language, label);
        text += std::to_wstring(value) + L"\r\n";
    };
    line({ L"ETW events: ", L"ETW 有效事件: " }, d.etw.accepted);
    line({ L"ETW parse failures: ", L"ETW 解析失败: " }, d.etw.parseFailures);
    line({ L"Endpoint parse failures: ", L"连接端点解析失败: " }, d.etw.endpointFailures);
    line({ L"ETW events lost since attachment: ", L"接入后 ETW 丢失事件: " }, d.etw.eventsLost);
    line({ L"ETW real-time buffers lost: ", L"ETW 实时缓冲区丢失: " }, d.etw.realtimeBuffersLost);
    line({ L"ETW query error: ", L"ETW 会话查询错误: " }, d.etw.queryError);
    line({ L"ETW pre-start events excluded: ", L"已排除启动前 ETW 事件: " }, d.etw.bufferedBeforeStart);
    line({ L"ETW retransmit events: ", L"ETW 重传事件: " }, d.etw.retransmitEvents);
    line({ L"EStats connections: ", L"EStats 连接: " }, d.estats.connections);
    line({ L"EStats enabled: ", L"EStats 已启用: " }, d.estats.enabled);
    line({ L"Matched probing connections: ", L"已匹配验证连接: " }, d.probing);
    line({ L"EStats official connections: ", L"EStats 正式入账连接: " }, d.active);
    line({ L"Verified source handoffs: ", L"已验证来源切换: " }, d.sources.switches);
    line({ L"Normal connection closes: ", L"正常连接关闭: " }, d.sources.normalCloses);
    line({ L"EStats failure fallbacks: ", L"EStats 异常回退: " }, d.sources.fallbacks);
    line({ L"Source validation refusals: ", L"来源验证拒绝: " }, d.sources.rejected);
    line({ L"Sequence evidence mismatch: ", L"序列证据不一致: " }, d.sources.sequenceRejects);
    line({ L"Identity/generation mismatch: ", L"身份或连接代数不匹配: " }, d.sources.identityRejects);
    line({ L"Confirmed sequence coverage gaps: ", L"已确认序列覆盖缺口: " }, d.sources.gaps);
    line({ L"Unproven connection intervals: ", L"连接完整性未证实区间: " }, d.sources.incomplete);
    line({ L"ETW coverage-unknown intervals: ", L"ETW 覆盖未知区间: " }, d.captureUncertainIntervals);
    line({ L"Open suppression guards: ", L"开放抑制窗口: " }, d.sources.activeWindows);
    line({ L"Closed guards pending retirement: ", L"已关闭待回收窗口: " }, d.sources.closedWindows);
    line({ L"Retired guards: ", L"累计回收窗口: " }, d.sources.reclaimedWindows);
    line({ L"Late/ambiguous generation callbacks: ", L"迟到或连接代数有歧义的事件: " }, d.sources.staleEvents);
    line({ L"Unclocked ambiguous callbacks: ", L"无有效时钟的歧义事件: " }, d.sources.unclockedEvents);
    line({ L"Proof session error: ", L"连接验证会话错误: " }, d.proof.error);
    line({ L"Proof parse failures: ", L"连接验证解析失败: " }, d.proof.parseFailures);
    line({ L"Proof lost events/buffers: ", L"连接验证丢失事件及缓冲区: " }, d.proof.eventsLost + d.proof.buffersLost);
    line({ L"Source tracking capacity reached: ", L"来源跟踪容量超限: " }, d.sources.overflows);
    line({ L"EStats enable failures: ", L"EStats 启用失败: " }, d.estats.enableFailures);
    line({ L"EStats read failures: ", L"EStats 读取失败: " }, d.estats.readFailures);
    line({ L"EStats baseline resets: ", L"EStats 基线重置: " }, d.estats.resets);
    line({ L"EStats coarse intervals: ", L"EStats 长采样区间: " }, d.estats.coarseIntervals);
    line({ L"EStats last error: ", L"EStats 最近错误: " }, d.estats.lastError);
    line({ L"Connection cache overflow: ", L"连接缓存超限: " }, d.queueOverflows);
    line({ L"History scheduler failures: ", L"历史调度异常: " }, d.commitFailures);
    text += GetLocalizedText(language, { L"Unknown D/U: ", L"未归属 下/上: " });
    text += Utils::FormatBytes(d.unknownRx) + L" / " + Utils::FormatBytes(d.unknownTx) + L"\r\n";
    const auto amount = [&](const LocalizedText& label, std::uint64_t rx, std::uint64_t tx) {
        text += GetLocalizedText(language, label);
        text += Utils::FormatBytes(rx) + L" / " + Utils::FormatBytes(tx) + L"\r\n";
    };
    amount({ L"Ambiguous withheld D/U (not proven loss): ", L"歧义暂不入账 下/上（非已证实丢失）: " }, d.sources.quarantinedRx, d.sources.quarantinedTx);
    amount({ L"Session raw ETW D/U: ", L"本次采集原始 ETW 下/上: " }, d.rawRx, d.rawTx);
    amount({ L"Session EStats ledger D/U: ", L"本次采集 EStats 入账 下/上: " }, d.sources.estatsRx, d.sources.estatsTx);
    amount({ L"Session hybrid ledger D/U: ", L"本次采集混合入账 下/上: " }, d.formalRx, d.formalTx);
    std::lock_guard<std::mutex> lock(m_historyMutex);
    line({ L"History write/checkpoint failures: ", L"历史写入或检查点失败: " }, m_historyWriteFailures);
    text += GetLocalizedText(language, { L"History: ", L"历史保存: " });
    text += m_historyStore.IsWriteHealthy() ? GetLocalizedText(language, { L"OK", L"正常" }) :
        GetLocalizedText(language, { L"Write failed; retry pending", L"写入失败，等待重试" });
    text += GetLocalizedText(language, { L"\r\nNetwork history: ", L"\r\n网卡历史保存: " });
    text += m_networkStore.Healthy() ? GetLocalizedText(language, { L"OK", L"正常" }) :
        GetLocalizedText(language, { L"Write failed; retry pending", L"写入失败，等待重试" });
    const auto metadata = m_historyStore.GetEngineMetadata(m_historyStore.GetPreferredRange());
    text += L"\r\n";
    if (metadata.cutover)
    {
        FILETIME fileTime{ static_cast<DWORD>(metadata.cutover), static_cast<DWORD>(metadata.cutover >> 32) };
        SYSTEMTIME utc{};
        FileTimeToSystemTime(&fileTime, &utc);
        text += GetLocalizedText(language, { L"V2 start: ", L"V2 启用: " });
        text += Utils::FormatDateTime(utc, true) + L" UTC";
    }
    else text += GetLocalizedText(language, { L"V2 history: not started", L"V2 历史: 尚未开始" });
    if (metadata.corrupt || (metadata.confidence & 6))
        text += GetLocalizedText(language, { L"\r\nSelected range: incomplete/unknown metadata", L"\r\n所选范围: 存在不完整或未知记录" });
    if (metadata.legacy)
        text += GetLocalizedText(language, { L"\r\nLegacy data has no V2 source/quality metadata", L"\r\n旧历史无 V2 来源及完整性元数据" });
    return text;
}

CNetworkTrafficCollector::Snapshot CProcNetPlugin::GetNetworkSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_historyMutex);
    return m_networkCollector.GetSnapshot();
}

bool CProcNetPlugin::SetNetworkAdapter(const std::wstring& id)
{
    std::lock_guard<std::mutex> lock(m_historyMutex);
    if (!m_networkStore.SetPreferredAdapter(id)) return false;
    m_networkStore.Flush(true);
    m_networkCollector.SelectAdapter(id);
    return true;
}

bool CProcNetPlugin::ExportHistory(
    const CHistoryTrafficStore::DateTimeRange& range,
    const std::wstring& output_path,
    std::wstring& error_message, bool network) const
{
    std::lock_guard<std::mutex> lock(m_historyMutex);
    if (m_networkStore.IsManagedPath(output_path) || m_networkStore.IsManagedPath(output_path + L".tmp"))
    { error_message = L"Protected history path."; return false; }
    if (network) return m_networkStore.ExportRange(range, output_path, error_message);
    return m_historyStore.ExportRange(range, output_path, error_message);
}

CHistoryTrafficStore::DateTimeRange CProcNetPlugin::GetPreferredRange() const
{
    std::lock_guard<std::mutex> lock(m_historyMutex);
    return m_historyStore.GetPreferredRange();
}

void CProcNetPlugin::SetPreferredRange(const CHistoryTrafficStore::DateTimeRange& range)
{
    std::lock_guard<std::mutex> lock(m_historyMutex);
    m_historyStore.SetPreferredRange(range);
}

CHistoryTrafficStore::DisplayLanguage CProcNetPlugin::GetPreferredLanguage() const
{
    std::lock_guard<std::mutex> lock(m_historyMutex);
    return m_historyStore.GetPreferredLanguage();
}

void CProcNetPlugin::SetPreferredLanguage(CHistoryTrafficStore::DisplayLanguage language)
{
    std::lock_guard<std::mutex> lock(m_historyMutex);
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
    const bool english = GetPreferredLanguage() == CHistoryTrafficStore::DisplayLanguage::English;

    for (int i = 0; i < kMaxApps; ++i)
    {
        auto& down_item = m_items[static_cast<size_t>(i * 2)];
        auto& up_item = m_items[static_cast<size_t>(i * 2 + 1)];
        const AppTrafficEntry* app = i < visible_count ? &apps[static_cast<size_t>(i)] : nullptr;
        SetItemPair(down_item, up_item, app, i, english);
    }

    m_tooltip = BuildTooltipText(apps, visible_count, english, m_collector.GetStatusText());
}

void CProcNetPlugin::UpdateHistory()
{
    const auto batch = m_collector.PeekPending();
    std::lock_guard<std::mutex> lock(m_historyMutex);
    if (!batch.sequence)
    {
        if (!m_historyStore.Checkpoint()) ++m_historyWriteFailures;
        return;
    }
    std::vector<CHistoryTrafficStore::AppTotalEntry> history_apps;
    history_apps.reserve(batch.deltas.size());
    for (const auto& delta : batch.deltas)
    {
        CHistoryTrafficStore::AppTotalEntry entry;
        entry.appName = delta.name; entry.exePath = delta.path; entry.identity = delta.identity;
        entry.rxTotalBytes = delta.rx; entry.txTotalBytes = delta.tx;
        entry.sampleStart = delta.start; entry.sampleEnd = delta.end; entry.source = delta.source; entry.confidence = delta.confidence;
        history_apps.push_back(std::move(entry));
    }
    if (batch.sequence == m_lastHistorySampleSequence || m_historyStore.Update(history_apps))
    {
        m_lastHistorySampleSequence = batch.sequence;
        m_collector.Acknowledge(batch.sequence);
    }
    else ++m_historyWriteFailures;
}

void CProcNetPlugin::EnsureCollectorStarted()
{
    std::lock_guard<std::mutex> lock(m_historyMutex);
    if (!m_collectorStarted)
    {
        // API 7 has no unload callback. Keep worker code mapped if the host
        // releases its plugin handle; do not join threads under the loader lock.
        HMODULE module{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&CProcNetPlugin::Instance), &module)) return;
        m_collectorStarted = m_collector.Start();
    }
}

void CProcNetPlugin::EnsureHistoryInitialized()
{
    std::lock_guard<std::mutex> lock(m_historyMutex);
    if (m_historyInitialized)
    {
        return;
    }

    m_historyStore.Initialize(Utils::GetHistoryDirectory());
    m_networkStore.Initialize(m_historyStore.GetStorageDirectory());
    m_networkCollector.SelectAdapter(m_networkStore.GetPreferredAdapter());
    m_historyInitialized = true;
}

void CProcNetPlugin::EnsureDetailWindow()
{
    if (m_detailWindow == nullptr)
    {
        m_detailWindow = std::make_unique<CTrafficDetailWindow>(*this);
    }
}

void CProcNetPlugin::ShowDetailWindow(HWND parent)
{
    EnsureHistoryInitialized();
    EnsureCollectorStarted();
    EnsureDetailWindow();

    if (m_detailWindow != nullptr)
    {
        m_detailWindow->Show(parent);
    }
}
