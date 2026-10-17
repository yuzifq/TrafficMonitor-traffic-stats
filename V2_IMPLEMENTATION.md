# V2-Hybrid Implementation And Acceptance

Objective: implement the complete user-provided V2-Hybrid specification.
Baseline: de2087a, Release x64 DLL 577536 bytes, TrafficMonitor API 7.

## Invariants

- One DLL, strictly less than 600000 bytes; static /MT runtime, no new driver,
  service, installer, runtime dependency, or proportional adapter reconciliation.
- Adapter counters remain independent GetIfTable2 measurements.
- Never add ETW and EStats bytes for the same connection interval.
- First EStats observation is a baseline, not traffic. Reset, failure, missing
  identity, reused tuple/PID, or unverifiable handoff never produces fabricated
  deltas or assigns traffic to another application.
- EStats stays a shadow source until repeatable real-machine tests establish
  improvement and a nonoverlapping handoff. An available API is not proof.
- Preserve all old history. Record engine cutover, source and completeness in
  lightweight versioned metadata without rewriting legacy history.
- Pausing the UI never pauses collection/checkpoints. Read-only UI/query/export
  operations cannot consume samples or change cumulative traffic.
- Do not replace the installed DLL before all release gates pass.

## Work And Evidence

- [x] Shared connection keys: IPv4/IPv6 endpoints, scopes, protocol, PID,
  process creation time and connection generation; same-name paths separate.
- [x] TcpEStatsCollector.h/.cpp: owner PID table enumeration, Data-only enable,
  valid 64-bit delta baselines, IPv6 parity, per-connection errors and cleanup.
- [x] ETW connection metadata: version/length checked addresses and ports,
  payload PID, timestamp, direction, lifecycle and failure diagnostics.
- [x] HybridTrafficCollector.h/.cpp: independent scheduling (table 500 ms,
  EStats 1 s), shadow comparison, explicit source/coverage state machine,
  bounded lifecycle/cache memory and sequence-based publication.
- [x] Validate safe connection-level source choice, short/closed flows,
  permission and sampling failure fallback, no larger-value heuristic.
- [x] Plugin integration, stable application identity, legacy/new history
  queries, cutover/source/quality metadata and reliable checkpoint retries.
- [x] Preserve ranking, icons, sorting, dates/shortcuts, Chinese/English,
  left-bottom process/adapter totals and streaming export.
- [x] UI pause/resume and compact diagnostics entry: source coverage, ETW
  loss, EStats failures, unknown ownership and history write state.
- [x] Deterministic tests: exact injected TCP/UDP deltas, baselines/reset,
  identity and tuple reuse, delayed/out-of-order events, nonoverlap, failure
  retry, UI reads/pause, restart/cross-day/export and legacy preservation.
- [x] Real Windows tests: known long TCP download/upload, 100 short TCP
  connections, IPv4/IPv6, mixed TCP/UDP, QUIC (or verified UDP preservation),
  proxy/VPN/loopback, exit/reuse, permissions, ETW/EStats failure behavior.
- [x] Measure CPU/memory under many connections and compare known payloads,
  ETW, EStats and same-adapter Windows increments; explain different semantics.
- [x] Release x64: clean build, tests, API 7/load smoke, UI rendering check,
  linker map size audit, /MT dependency audit and DLL <600000 bytes.
- [x] Update Chinese/English docs, record exact results and limitations, provide
  the single DLL without replacing the running installation.

## Current Runtime Result, 2026-10-17

The full runtime now schedules TcpProofCollector and TcpSourceTracker. EStats
actually supplies qualified connection increments; this is no longer the shadow
prototype described in the chronological evidence below. Old per-flow shadow
state/comparison code has been removed instead of retaining two selection engines.

- Birth-captured TCP connections use exact endpoint/scope/PID/owner/connection
  generation identities and common QPC coordinates. Two quiescent native reads
  must match both the accepted legacy prefix and unique modern sequence prefix.
  Nonzero prefixes and different Data/Thru epochs are supported without replay.
- Qualified reads validate Data deltas against anchored unique sequence positions;
  disagreeing/larger counters never enter history. Failure, rollback, read gaps
  over 2.5 seconds or close fall back to uncredited verified ETW ranges. Lost
  proof freezes accounting with visible uncertainty, not estimated bytes.
- Retained lifetime windows prevent delayed legacy duplicates after handoff.
  TCP tuple/TCB reuse, late duplicate Connect, 32-bit sequence wrap with 64-bit
  increments, out-of-order ranges, FIN control space, wrong endpoints/scopes,
  PID reuse and missing observations pass deterministic tests.
- Zero-byte gap metadata survives checkpoint, restart, failed append and retry,
  without creating byte rows or changing cumulative totals.
- Windows 11 Pro x64 build 26200: both IPv4 and IPv6, a known nonzero 1-byte
  legacy prefix followed by 5330080 bytes deliberately omitted from legacy ETW
  produces exactly 5330081 bytes per direction. Healthy EStats accounts all
  5330080 post-handoff bytes. Injecting a native-read failure after 1184 bytes
  produces the same exact final amounts through overlap-clipped ETW fallback.
  Four full native cases and additional repeat runs passed. Earlier refusal to
  qualify was not converted into a false pass or a relaxed boundary rule.
- Fully scheduled collectors retain exactly 102400 bytes/direction for 100
  IPv4 short connections and another 100 IPv6 connections. Concurrent TCP/UDP
  gives 10786852 bytes/direction for each family. UDP-only verified payloads
  remain exactly 216650 bytes/direction. No new event loss or parsing failures.
- Actual active-observer load: 512 controlled endpoints, 615 total including
  background connections, 256 active source selections; 13.563 seconds wall,
  0.21875 seconds process CPU, +13778944 private bytes and +19603456 working set.
  These are whole-test-process measurements, not a promised steady-state DLL
  overhead. Native poll total 65150 microseconds, zero queue/proof loss.
- Normal proof Stop drains and removes its owned session. Startup cleanup
  checks GUID/PID/process birth; native fixtures prove stale/reused-PID owners
  are removed while live owners are preserved. API 7 has no exit notification:
  an abruptly exited host can leave its session until next startup. No worker
  joins are made under the DLL loader lock, and unrelated traces are not stopped.
- Independent adapter live gate: 40 samples equal raw GetIfTable2 deltas and
  legacy GetIfEntry brackets exactly (30652 download / 62702 upload bytes in
  the recorded run). This does not redefine process payload as adapter bytes.
- Five regression executables pass. Bilingual rendered layouts, icon ownership,
  sorting/ranges/streaming export, nonconsuming reads and UI pause are retained;
  latest durable paused checkpoint gap is 9047 ms. Offline reset rehearsal on
  the full backup preserves every older SHA256, loads today as zero, and
  exactly restarts fresh traffic without resurrecting removed checkpoints.

Final clean Release x64 build: 579584 bytes (566 KiB), strictly below 600000;
API 7/version 1.1.4 retained, /MT, system-only imports. The map includes the actual
proof/source runtime, not dead-code removal of an unused module. Largest retained
functions are completed-summary generation (5608 B), range totals (4864 B),
streaming export (4184 B) and historical recovery, not expendable safety code.
The complete runtime is only 2048 bytes larger than the installed 577536-byte
baseline after removing duplicate shadow state, floating-format overhead and
unnecessary allocation templates. No history recovery was removed for size.

The final UI review found cramped CJK fallback glyphs in the old bitmap default
font. System message fonts and metric-based summary height fix this; bilingual
900/1200 layouts and durable paused checkpoints pass again. Original 174-file
backup hashes were rechecked unchanged before deployment. Conditional Oct-17
reset/install is complete as recorded below; official daily history is preserved
because its separate reset was not confirmed. Detailed historical measurements
below retain their original context and are not the current shadow-mode status.

### Deployment Verification

#### Joint Reset Confirmed By The User

The user subsequently confirmed resetting BOTH official and plugin Oct-17
statistics. On 2026-10-17, a new 176-file SHA256-verified snapshot was created at
`traffic_data_backup_before_joint_reset_20261017_034935`. The test host did not
exit through the duplicate-instance UI, so its exact verified PID and the helper
instance were terminated only after the snapshot. No other process was stopped.

ResetTrafficDay.ps1 now validates version-6 checkpoint/commit checksums and
transaction identity as well as version-5 data. Rehearsals on both complete
backup generations pass; the V2 rehearsal also passes production-store zero-day
load and fresh-traffic restart checks. Actual removed trial amounts were
27785326 / 112226027 bytes, returning the cumulative baseline exactly to
2963210484269 / 2381640650024. Official Oct-17 rows in BOTH history_traffic.dat
and its .bak were set to 0/0, preserving all other official rows. The seven
current-day raw/quality/cache/checkpoint files were archived, not destroyed.
166 other files retain their snapshot hashes. TrafficMonitor restarted with
PID 32336 at **2026-10-17 03:51:56 +08:00** for the new common-start trial.
The earlier 03:00:41 start and official-not-reset note below are superseded.

#### Earlier Plugin-Only Trial

- Installed `plugins/ProcessTrafficPlugin-V1.1.4.dll`, SHA256
  `A23F57CC10D760A673DE5A2F45BE5E7DF65FDA1AB1BB0B65031D93A5828C54A5`.
  Elevated TrafficMonitor PID 5024 started 2026-10-17 03:00:41 +08:00;
  module enumeration verifies this exact path/hash and one owned proof session.
- Oct-17 plugin history, network history, summaries and old checkpoints were
  archived under `traffic_day_reset_20261017_20261017_030041_079`; not destroyed.
  Exact removed process amounts: 69897720 download / 510000614 upload bytes.
  Remaining cumulative baseline: 2963210484269 / 2381640650024 bytes. Official
  daily statistics were NOT reset because that separate confirmation is absent.
- Before restarting, 166 unaffected original files retained their hashes. After
  startup, all 77 older raw TSV/TMH files, previous adapter raw/cache, and all
  13073 older case-sensitive Base62 dictionary IDs remain intact. One obsolete
  Oct-15 derived day summary was rebuilt: original raw 9902 rows exactly sum to
  the new summary, 38186185768 / 6926010817 bytes. This is cache repair, not an
  old raw-history rewrite or a change to the reset cumulative baseline.
- Fresh v6 checkpoints and quality records show sources=3 (ETW plus actual
  EStats), with uncertainty retained when an interval cannot be proven complete.
  The real installed host is exercising both sources, not only a test executable.
- A read-only two-minute live audit [03:01,03:03) records process 2374068 /
  4883661 bytes and fully-contained adapter 2693768 / 4985254, with boundary
  40493 / 213332 separate and exact 120000/120000 ms adapter coverage. This
  short natural workload does NOT prove the entire remaining payload/adapter
  difference has been attributed or eliminated. No scaling was performed.
- The persisted range now starts Oct-17 00:00 but is fixed at 03:00 until the
  user selects Day or Follow current time. Official comparison must use new
  increments after the common restart baseline, not its retained earlier day sum.

## Historical Evidence Before Runtime Handoff

The following entries are earlier snapshots, not current deployment status.
At the earlier shadow-prototype stage, the installed DLL had not been replaced;
that release build was 543232 bytes (raw-clock/cursor changes before source integration).
Size gate is wired into Release x64 builds. /MT retained. Actual DLL load/API 7,
system-only import audit, and Chinese/English rendering checks passed.

- Deterministic EStats deltas, rollback, denied/recovered sampling, connection
  generations, scope/path identity, delayed PID reuse, unknown ownership and
  immutable pending batch/ACK tests pass.
- Native EStats query: standard-user enable returns error 5; elevated IPv4/IPv6
  reads work. EStats remains shadow-only; `EStatsActive` is not enabled.
- Known loopback stream: ETW exactly matches 4194304 upload and 3145728 download
  bytes, for IPv4 and IPv6. EStats upload reports +40/+60 bytes. Repeating with
  an idle baseline does not remove the difference; no constant is subtracted.
  EStats includes retransmissions, so unexplained extra bytes are not by
  themselves proof of an API defect. The strict source activation gate fails.
- 100 native short TCP connections: ETW Rx/Tx each exactly 102400 bytes after
  correcting a newly introduced startup-filter regression. The long-lived QPC
  session's default ProcessTrace UTC conversion put all 204800 test bytes
  before consumer startup. Raw QPC filtering with local UTC calibration fixes
  this without admitting pre-start buffers. This bug was in the V2 prototype,
  not the installed 1.1.4 DLL. Unsupported session clocks/network flags now fail
  visibly instead of pretending to collect. Deterministic clock tests pass.
- Native UDP: IPv4 and IPv6, 60 datagrams per family, sizes 1..16384; both ETW
  directions exactly 216650 payload bytes, no new loss or parse failures.
- External verified HTTPS echo: actual encrypted TCP Rx/Tx 134130/65790 bytes;
  ETW exactly equal, EStats 134130/68534; no observed ETW event loss. Additional
  retransmission diagnostics added; this run alone does not attribute extra send bytes.
  IPv4 external connection timed out, not a passing comparison.
- Repeated external IPv6 test after the clock fix: verified encrypted TCP
  Rx/Tx 134130/65790, ETW exactly equal, EStats 134130/70612. No ETW loss or
  retransmit events for that PID. That run alone did not explain extra send bytes;
  absent retransmit events do not establish that no retransmissions occurred.
  ETW seqnum and connid are zero in these real samples and cannot prove a
  byte-sequence handoff on this machine. Do not promote the source or subtract
  magic constants. EStats acceptance remains failed/unproven, not silently passed.
- Load test: 512 controlled endpoint rows plus background connections, 655 total;
  12.141 seconds wall, 0.109375 seconds process CPU, +6823936 private bytes,
  +14520320 working-set bytes, no queue overflow, 22 table polls (78906 us total).
- Recovery tests pass: byte-preserving legacy append, path-distinct names,
  source/quality CRC and duplicate metadata snapshots, restart/export, failed
  checkpoint/state/minute/metadata writes and retry without repeated deltas,
  corrupt provenance preserving valid bytes, strict/leap date parsing.
  Partial multi-row append recovery now verifies a durable intended block and
  existing prefix before resuming only its suffix. Conflicting/corrupt journals
  preserve existing bytes, report unavailable rather than zero, and cannot be
  bypassed by UI preference saves or export.
- ETW, history recovery, independent adapter, and UI/summary regression suites
  pass after the latest changes. Actual Release DLL smoke passed. Native UDP
  test fixture warning fixed; its final test-only rebuild and ledger tests pass.
  No installed DLL or old user history has been replaced/repaired.

### 2026-10-17 Validation Update

- All five current deterministic/regression executables pass after the clean
  rebuild, including actual DLL loading/API 7, 40 live adapter samples against
  Windows 64-bit and legacy counter brackets, and independent paused/no-host
  history checkpoints (maximum observed gap in this run: 9031 ms).
- Checkpoint version 6 and append journal TMAPND02 use checksummed unique
  transaction IDs. Legitimate identical payloads within the same minute remain
  distinct, including after clock rollback; failed commit-marker recovery does
  not append the raw block twice. Numeric ID corruption blocks unsafe replay.
- The collector invokes its history commit sink independently of host/UI
  refresh. The 9000 ms checkpoint threshold leaves scheduling margin for the
  10-second target. Sink exceptions remain visible and retry the same batch.
- Integer byte/rate/date formatting and string-based export counters remove
  floating-point printf/stream formatter dependencies. Release x64 is 542720
  bytes, 34816 bytes smaller than the 577536-byte baseline. /MT, API 7 and
  system-only imports are retained; size is not a completed hybrid acceptance.
- Native stepped TCP sends/echoes total exactly 5330081 bytes per direction
  for IPv4 and IPv6 in both ETW and EStats. Concurrent TCP/UDP tests preserve
  exact formal ETW app totals (10786852 each direction, including both owned
  endpoints); shadow counters cannot contaminate formal history.
- A separate test-only Microsoft-Windows-TCPIP trace supplies real send
  sequence numbers. A controlled IPv6 TLS connection has 67463 whole-stream
  socket send bytes, 70207 modern send/EStats DataBytesOut bytes, and exactly
  2744 repeated-sequence bytes. Its unique send sequence coverage is exactly
  67463. This explains the counter difference at the TCP-stack event level,
  not necessarily physical wire retransmission: Path BytesRetrans is zero and
  the corresponding complete packet-level retransmission proof is absent.
- The modern trace reuses one TCB pointer for another connection. The analyzer
  now requires endpoint-verified connect/summary events and scopes all pointer
  matches to that lifetime; it excludes 11 events from the reused instance.
  Modern receive NumBytes also includes sequence-space control observations
  and is NOT adopted as a payload accounting source. Test-only broad modern
  provider keywords are NOT enabled in the production DLL.
- A different controlled physical-adapter capture has 132 received frames,
  143956 frame bytes, and exactly 134128 unique TCP payload bytes, matching
  the socket and legacy ETW. Its 9828-byte difference is 132 * 74 bytes of
  Ethernet/IPv6/base-TCP headers plus 60 TCP-option bytes; 27 frames have no
  payload. Send-side LSO metadata and missing capture coverage prevent using
  that trace to claim complete transmit accounting.

Formal EStats activation/source handoff remains unimplemented and unaccepted.
Successful shadow reads or the repeated-sequence result do not establish that
switching improves the installed plugin's process totals. Never silently turn
this evidence into an EStatsActive success or a full V2 delivery claim.

### Subsequent Runtime Foundations, 2026-10-17

- Added source-separated immutable pending batches and per-app snapshot source
  masks. New tests prove same-app ETW/EStats increments retain provenance, produce
  one ranking row, and survive ACK retry, checkpoint/minute commit and restart.
- Removed reversed-flow EStats matching. Native TCP send/receive endpoint checks
  pass on IPv4/IPv6; opposite same-PID endpoints are distinct exact identities.
- EStats retains sequence/through cursors from its existing Data-only read;
  through-counter rollback invalidates the baseline, and read failure clears
  stale cursor evidence. Real stepped IPv4/IPv6 cursor comparisons pass.
- TcpProofCollector is a TDH-validated, event-ID-filtered native sequence
  observer. It does not enable the global Summary keyword. Native lifecycle,
  pointer/sequence/schema bounds, known 5330081-byte send/receive sequence ranges,
  same-connection EStats position anchors and owned-session Stop checks pass.
  It is not yet scheduled in the production collector; it remains linker-dead
  there. Current DLL is 543232 bytes, not a final integrated size result.
- Combined controlled TLS/NDIS/modern trace: verified TCP Rx/Tx 134154/65790,
  legacy ETW exact, EStats Tx 74384, modern repeated send sequence bytes 8594.
  NDIS upper-edge payload is 67090 (65790 unique + 1300 repeated), after correctly
  reading LSOv2 zero IP-length buffers. This does not equate internal send attempts
  to post-segmentation wire traffic. The native external EStats accuracy gate
  remains failed; it is not made green by revising the expected payload size.
- One paused-checkpoint regression run crossed a minute. It recovered all_time
  1234/5678 but queried a deliberately paused range ending in the earlier minute.
  The harness now queries through actual test end and waits for a clean durable
  checkpoint, rather than treating any file timestamp change as target durability.
  This fixes the test boundary, not user history or the underlying counters.
- Remaining implementation and explicit acceptance gates are documented in
  V2_HANDOFF_DESIGN.md. Shadow/proof foundations do not complete the full goal.
- Both event DTOs now retain raw clock coordinates and EStats reads retain QPC
  brackets, so future generation/source boundaries need not compare separately
  calibrated UTC clocks. Updated native endpoint/sequence anchors and stepped
  cursor/QPC tests pass for IPv4/IPv6; all five regression executables and actual
  DLL/API7 loading pass (last paused checkpoint gap: 9031 ms). Original installed
  data/DLL were rehashed against all 174 backup entries: zero changed files.

## Same-Interval User History Audit

At 2026-10-16 17:30, the completed half-open interval 04:00..17:29 contained:

| Source | Download bytes | Upload bytes |
| --- | ---: | ---: |
| All process history, including hidden PID rows | 1809609432 | 2031096271 |
| Selected adapter history, fully contained samples | 1941499012 | 2154097524 |
| Boundary samples, kept separate | 31141 | 10128 |

Adapter coverage was 48540 / 48540 seconds. Relative process shortfall is about
6.79% download / 5.71% upload; this does not reproduce a multiple-fold runaway.
Raw packet-layer overhead and non-TCP/UDP adapter traffic have different semantics
from the tested process payloads, but the whole historical residual has NOT been
attributed byte-for-byte. Do not assert it is all normal overhead or all ETW loss.
The persisted UI range ends at 15:30; if that fixed range is reused, it is not
comparable to an official full-day-through-now number. A saved preference alone
does not prove the user's current visible window is still using that range.

### Completed-Day Reconciliation, 2026-10-17

Read-only `tests/AuditTrafficHistory.cjs` validates Base62 fields, dictionary IDs,
network row checksums/order, one adapter identity, interval coverage and unchanged
daily source hashes. Official daily values are KiB, upload first, per host 1.86
HistoryTrafficFile.cpp. Only a full-day query compares the official daily row;
partial-hour/minute queries do not invent an official subdaily allocation.

For 2026-10-16, the persisted daily records contain:

| Source | Download bytes | Upload bytes |
| --- | ---: | ---: |
| Official daily history | 3162402816 | 2642492416 |
| Independent plugin adapter history | 3162412161 | 2643027761 |
| All process history, including anonymous rows | 2943494695 | 2483711932 |

The plugin/official adapter difference is +9345 / +535345 bytes (about 0.00030% /
0.02026%). This does not reproduce a large official-versus-adapter divergence.
The plugin daily adapter file starts at 03:59:04, not midnight: neither this
comparison nor its low difference proves that the earlier unobserved hours had
no traffic. Process totals remain 6.92% / 6.03% below the adapter totals.

Restricting both plugin ledgers to 04:00..23:59 gives exact adapter time coverage
71940000 / 71940000 ms, process 2933313481 / 2443511792, adapter 3161003226 /
2632743489, with boundary bytes 8273 / 211572 kept separate. The remaining
shortfall is 7.20% / 7.19%; anonymous rows contain only 532187 / 193215 bytes.
Neither the hidden-PID display filter nor minute boundary bytes explains it.

The 03:59 startup minute is different: process 9993689 / 32062659 bytes versus
adapter 996474 / 1742957, plus boundary 2015 / 7221. Original 1.1.4 has no
pre-start ETW event filter. Buffered events and different collector/checkpoint
startup boundaries are candidates; minute totals alone do not identify which
caused this historical anomaly. Do not retrospectively edit those bytes.

The read-only controlled packet trace explains one connection's receive gap
exactly as headers/control traffic, not the whole day's residual. A fixed saved
range still ends at 15:30; its current visibility has not been confirmed. No
historical aggregate proves that all residual bytes are normal overhead or loss.

The user exited TrafficMonitor and conditionally authorized clearing October 17
and installing the new DLL **after completion/verification**. Process absence
was confirmed, and 174 files (26040729 bytes) were backed up and SHA256 verified
under `traffic_data_backup_before_v2_20261017_012231` next to the host executable.
The installed DLL and original histories remain unchanged. Do not clear/reset
today while full source-switching acceptance is still unresolved. A future
reset must handle cumulative state/checkpoints/derived summaries consistently,
not merely delete the daily TSV and leave stale cumulative counters.

Remaining acceptance is NOT reduced to shadow mode: investigate/validate the
EStats-vs-ETW accounting difference and safe connection source handoff; complete
the full source-selection/fallback requirements only if the specified evidence
permits activation. Do not claim V2 mixed accounting is finished because the
prototype, legacy regressions or size checks pass.

2026-10-16: baseline source and build settings re-read; worktree clean.
The previous read-only 04:00-04:24 comparison had complete adapter coverage:
process Rx/Tx 83345698/303642429, adapter 91856128/330143576 bytes, with
39843/14282 boundary bytes separate. Three 60-second ETW observations had
no new EventsLost/RealTimeBuffersLost or property parsing errors. Historical
EventsLost=28680845 cannot be attributed to the new observation intervals.
Large proxy loopback traffic was correctly excluded. These measurements do
not establish EStats accuracy or justify enabling it as an accounting source.

Microsoft API semantics verified 2026-10-16:
- https://learn.microsoft.com/en-us/windows/win32/api/tcpestats/ns-tcpestats-tcp_estats_data_rod_v0
- https://learn.microsoft.com/en-us/windows/win32/api/iphlpapi/nf-iphlpapi-getpertcpconnectionestats
- https://learn.microsoft.com/en-us/windows/win32/api/iphlpapi/nf-iphlpapi-setpertcpconnectionestats
- https://learn.microsoft.com/en-us/windows/win32/api/iphlpapi/nf-iphlpapi-getextendedtcptable
