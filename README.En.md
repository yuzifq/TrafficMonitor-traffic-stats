# TrafficMonitor Traffic Stats Plugin

[简体中文](./README.md) | English

A TrafficMonitor plugin for monitoring **per-process network traffic** in real time and in accumulated history.

This project is a third-party plugin for [TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor).  
It is not part of the official TrafficMonitor project.

## Features

- Real-time upload and download statistics for each process
- Independent adapter speeds and minute history, with automatic or fixed adapter selection
- Supports cumulative historical traffic statistics by time period
- Detailed traffic window with process icons
- English / Chinese interface support
- Preference persistence across restarts

## Requirements

- TrafficMonitor with plugin API version `7`
- Windows x64
- Visual Studio or Build Tools with the MSVC v145 toolset and Windows SDK
- Build target: `Release | x64`

## Installation

1. Build the project successfully.
2. Copy `ProcessTrafficPlugin.dll` into the `plugins` directory.
3. Start TrafficMonitor **as Administrator**.
4. Open the plugin manager and confirm that the plugin is loaded.

## Download

If you do not want to build the plugin yourself, you can download the prebuilt package from the [Releases](../../releases) page.

## Notes

- This plugin relies on **ETW (Event Tracing for Windows)** to collect per-process traffic data.
- In many cases, TrafficMonitor must be started with **administrator privileges**; otherwise traffic collection may fail and you may see errors such as `StartTrace failed`.
- Local loopback traffic (`127.0.0.0/8`, `::1`, and the corresponding IPv4-mapped addresses) is excluded so that local forwarding by proxies, ADB, and similar programs does not count as upload or download.
- Per-process statistics cover TCP/UDP network activity, including virtual adapters and tunnels. Their sum is not guaranteed to match the physical adapter selected in TrafficMonitor. Compare traffic increments over the same time interval.
- Filtering applies to newly collected data only. Existing history does not contain connection addresses, so its loopback traffic cannot be subtracted accurately.

## Adapter Statistics And Minute History

Adapter totals use the same cumulative-counter delta approach as TrafficMonitor. The plugin independently reads Windows 64-bit byte counters through `GetIfTable2`; it does not read the host's live totals or `history_traffic.dat`. Select the same adapter in both applications and compare increments over the same interval. Automatic selection prefers connected hardware adapters and keeps its choice stable. Virtual adapters can be selected manually.

Adapter history is stored alongside process history in `network_traffic_history/YYYY/MM/YYYY-MM-DD.tsv`. Completed intervals are appended; the current interval is checkpointed every 10 seconds. Commit IDs prevent duplicate counting during recovery. Daily `.sum` files are rebuildable summary caches. An abnormal termination can lose data collected since the last checkpoint.

- Consecutive short samples within a minute are merged. Samples crossing a minute boundary and long intervals retain their actual endpoints; bytes are never distributed proportionally.
- Network range totals include only records fully contained in the selected range. Boundary bytes are shown separately and excluded from those totals.
- Sample coverage distinguishes missing observations from measured zero traffic. Long sampling intervals are flagged.
- Range controls use local time. Storage and adapter exports use UTC, preserving adapter identity, precision and boundary flags. Ambiguous daylight-saving minutes follow Windows time-zone conversion rules.
- Adapter selection affects subsequent collection. Historical totals include records from previous selections. The export menu offers process history and adapter history separately.
- Adapter history starts when this feature is enabled. Existing process history is retained and is not backfilled into adapter totals. ETW process totals can still differ from adapter counters.
- Day, week, month and year shortcuts follow the current time. Manual date/time edits stop following; the checkbox can resume it.

Release builds retain the static `/MT` runtime and use size optimization, whole-program optimization and unused-code elimination. No additional Visual C++ runtime installation is required.

## V2 Connection-Level Hybrid Collection

Process accounting uses ETW and switches qualified TCP connections to EStats only at verified boundaries. Sources are never added for the same interval, scaled to adapter totals, or selected merely because one is larger. UDP, short flows and existing connections without sufficient boundary evidence remain on ETW.

- Native IPv4/IPv6 EStats enables only `TcpConnectionEstatsData`. No extra driver, service or runtime is required. ETW continues when EStats cannot be enabled.
- Connection tables are checked every 500 ms, with counter reads targeting one second, independently of UI refresh. Initial observations, counter rollback and recovery after failed sampling establish new baselines rather than backfilling unknown intervals.
- Native TCP endpoint, lifecycle and sequence events verify the handoff. A complete ETW prefix must agree with two stable native snapshots; previously credited bytes are not replayed. Missing identity, sequence origins or boundary evidence denies the switch.
- Failed, stale or inconsistent EStats reads fall back to verified uncredited ETW sequence ranges. Overlaps and SYN/FIN sequence space are excluded; unresolved coverage is marked incomplete, never estimated or filled from adapter totals.
- History commits and checkpoints run on the independent collection schedule, without host/UI callbacks. Failed writes retry the same batch instead of accumulating it again.
- Identities include endpoints, IPv6 scopes, PID, process creation time and a connection generation. `TCP_TABLE_OWNER_MODULE_ALL` supplies both owning PID and connection creation time for stronger tuple-reuse validation than a PID-only table.
- New history distinguishes same-name applications by full executable path. Unknown ownership retains its bytes under anonymous PID entries; hiding those rows does not change totals.
- Legacy history is retained. New records reuse daily TSV files and the dictionary; state records the first V2 observation. Checksummed daily `.quality.tsv` files record source, confidence and observed event endpoints, not minute coverage or another additive traffic ledger.
- Corrupt provenance marks source/completeness unknown without modifying valid bytes. Legacy records are not backfilled or presented as V2 observations.
- New checkpoints and append transactions carry unique commit IDs and checksums. Legitimate identical traffic in one minute is not mistaken for a replay; conflicting recovery preserves existing files and reports an error.
- Diagnostics reports ETW loss/parsing failures, EStats enable/read failures, actual source switches, fallbacks, incomplete intervals, unknown bytes and history write health. Zero-byte uncertainty markers are persisted too. Pausing refresh affects the UI only.
- Release x64 emits a linker map and fails if the DLL reaches **600,000 bytes**. API 7 has no unload callback, so collection pins the module; exit TrafficMonitor to fully unload it.

Counter semantics matter: [EStats `DataBytesIn/Out`](https://learn.microsoft.com/en-us/windows/win32/api/tcpestats/ns-tcpestats-tcp_estats_data_rod_v0) includes retransmitted data and excludes TCP headers; adapter counters also include protocol/control traffic. Process totals are not promised to equal adapter totals. Real IPv4/IPv6 tests verify nonzero-prefix handoff, omitted legacy-event recovery and failure fallback; see `V2_IMPLEMENTATION.md` for evidence and limitations.

API 7 has no exit callback. The plugin never joins workers under the DLL loader lock. Normal Stop removes owned proof sessions; after abrupt process exit, the next start removes abandoned sessions only after validating their GUID, PID and process birth, preserving live sessions.

Controlled TCP, external comparison and load-test tools are under `tests/` and are not DLL runtime dependencies. Their reports do not establish correctness for every proxy, VPN or Windows networking configuration.

## Build

Open `ProcessTrafficPlugin.sln` in Visual Studio or Build Tools with the MSVC v145 toolset and Windows SDK, then build with:

- **Configuration:** `Release`
- **Platform:** `x64`

You can build from the command line with MSBuild:

```powershell
msbuild "ProcessTrafficPlugin.sln" /p:Configuration=Release /p:Platform=x64
```

# Open the main interface

## Right-click the program and open Plugin Management

<img width="587" height="533" alt="image" src="https://github.com/user-attachments/assets/32f020d1-56d8-4d96-8db1-20829a1b6243" />


## Right-click the plugin again to open Traffic Details

<img width="710" height="462" alt="image" src="https://github.com/user-attachments/assets/e9ece008-b293-482a-adcd-f8b49aafc6fc" />


<img width="1029" height="644" alt="image" src="https://github.com/user-attachments/assets/56a89174-2dec-43aa-9b30-567efd8f05aa" />

<img width="1116" height="731" alt="image" src="https://github.com/user-attachments/assets/897f1580-47d6-4dfa-a8ca-a6aef7de9399" />
