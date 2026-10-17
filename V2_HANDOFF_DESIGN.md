# Connection Handoff Design And Evidence

The runtime tracker and proof collector are now scheduled by HybridTrafficCollector.
The notes below retain the design rationale and historical implementation gates.
The current result is actual connection-level source selection, not a shadow-only
prototype. The complete objective remains unchanged; deployment still requires
the final build/load/render and preservation checks in V2_IMPLEMENTATION.md.

## Evidence Established On The Current Machine

- Legacy TCP send AND receive fields are already local/remote endpoint ordered.
  Native IPv4 and IPv6 checks find zero endpoint mismatches. Do not blindly swap
  receive endpoints. UDP has a different field convention and is not an EStats
  matching candidate.
- Hybrid now requires exact TCP keys, never a reversed same-PID connection.
- The modern Microsoft-Windows-TCPIP provider supplies TCB and real sequence
  values. Legacy TCP seqnum/connid are zero in the measured records and are not
  sequence proof. A sequence of zero is nevertheless valid on a real modern
  record; provider/schema identity, not a nonzero heuristic, establishes trust.
- TcpProofCollector enables only event IDs 1002,1004,1017,1033,1043,1051,1074,
  1176,1332,1333 in its owned session. Summary keyword is deliberately absent:
  broad diagnostic summary collection can change global TCP statistics state.
- Send/receive prefixes are TDH-validated per event ID/version, then decoded
  without TDH calls on every data event. Pointer width, payload bounds and clock
  domain are checked. Raw QPC is mapped from a local precise-UTC calibration.
- Real controlled IPv4/IPv6 client connections anchor send SYN ISN+1 and receive
  SYN observation sequence to initial EStats SndMax/RcvNxt. Both final cursor
  deltas and unique data sequence spans equal 5330081 bytes each direction.
  Raw modern sends can be larger (5330203 in one IPv6 run), so repeated sequence
  ranges must never be treated as new application payload automatically.
- Receive NumBytes includes SYN/FIN sequence-space observations. The initial
  SYN receive is before Established and overlaps the first data sequence.
  The final FIN observation follows the tested local Close event and has
  Fin.RcvNxt = FIN sequence + 1. Do not sum raw modern receive lengths as payload.
- TCB pointers are reused. Bind endpoint/PID/lifetime before matching data;
  never join events merely because the pointer is equal.
- EStats observations now retain ThruBytesAcked/Received and SndUna/SndNxt/
  SndMax/RcvNxt from the SAME Data ROD read, with existing time brackets.
  Through counters can start at the collection-enable epoch (the older TLS
  baseline had DataOut=1673 but ThruBytesAcked=0). Do not assume all counters
  share a connection-birth zero. Through-counter rollback rebases the sample.
- Pending history is keyed by app identity AND source. UI snapshots aggregate
  once per app with a source mask. Same-app mixed-source metadata survives
  checkpoint, minute commit and restart without changing totals.

## Runtime Implementation

1. Add a bounded per-connection proof/state tracker and call TcpProofCollector
   from HybridTrafficCollector. Bind TCB generations to exact endpoints, PID,
   process creation and EStats connection generation. Preserve all old history.
   Metadata/schema/loss failures must deny qualification, not stop legacy ETW.
2. A tractable first proven boundary is a NEW idle connection, captured from
   birth, with SYN/Established sequence origins and two healthy zero-data
   EStats snapshots. The implementation also supports a complete nonzero prefix:
   legacy accepted bytes, unique modern sequence ranges and native positions
   must agree at two quiescent snapshots. Counter epoch offsets are anchored
   without replaying this prefix. Connections not captured from birth or without
   a provable quiescent boundary remain ETW, not guessed handoffs.
3. Both event DTOs now preserve their raw clock coordinate, and EStats retains
   QPC begin/end brackets for its native read. Use these in the tracker before
   distinguishing reused generations. Two independently calibrated UTC mappings
   are not proof of ordering at a microsecond boundary. Kernel sessions using a
   different clock cannot silently qualify for this path.
4. Track absolute sequence positions and bounded disjoint ranges. Accept native
   data only within the verified connection lifetime; separate SYN/FIN space.
   Deny qualification on ambiguous wrap, missing origin, mixed identity,
   overflow, prior loss or unsupported schemas. No larger-value heuristic.
5. While EStatsActive, suppress the same connection's legacy formal increments,
   retain proof ranges for fallback, and use one immutable source-tagged ledger.
   Use anchored through-counter deltas plus SndMax/SndUna to validate cumulative
   sent positions. Receive RcvNxt/through positions require contiguous data proof.
   Counter increment/unique-position disagreement must fail closed before that
   observation affects formal history. The DataOut surplus is not a correction
   factor and must not be used to fill adapter totals.
6. On read failure/reset/close, fall back using verified ETW sequence suffixes
   beyond the last credited EStats prefix. Clip overlaps; never replay an entire
   shadow buffer. Keep boundary/missing evidence explicitly incomplete. If BOTH
   evidence and counters fail, do not pretend a safe exact legacy cutover exists.
   Preserve diagnostic uncertainty and history completeness without fabricating
   bytes. New connections/UDP remain independently usable.
7. Account for loss/error-only intervals in history metadata even if no byte
   delta can safely be committed. Current Update skips engine metadata for
   zero-byte entries in the original prototype; this is now fixed and tested
   through checkpoint/restart/metadata-write-failure without invented byte rows.
8. Complete lifecycle cleanup for the owned proof session before deployment.
   Current stack-owned native tests verify Stop removes their session. API 7
   process-lifetime singleton has no API-7 exit callback. Normal Stop drains and
   removes the owned session; the next Start validates GUID/PID/process birth
   before removing sessions left by exited owners. Live-owner preservation is
   tested. No worker joins occur under the loader lock. A forced exit can retain
   its session until the next start; this limitation is documented, not hidden.

The tracker bounds connections/legacy markers/lifetime windows at 4096 and
disjoint ranges at 64 per direction. Capacity failures retain explicit diagnostic
uncertainty; new unqualified flows remain legacy. Retained windows deliberately
avoid admitting delayed duplicate callbacks after a switch. Missing native reads
for more than 2.5 seconds force fallback instead of leaving stale accounting active.

## Required Verification Before Activation/Installation

- Deterministic source selection, zero boundary, overlap clipping, out-of-order
  events, reused tuple/TCB/PID, wrap, failure/reset/recovery and short close.
- Real IPv4/IPv6 known TCP: idle qualification, normal transfers, deliberate
  legacy event loss, actual EStats recovery, and injected EStats failure with
  exact nonduplicating ETW fallback. A healthy read alone is not improvement.
- Mixed TCP/UDP, short connections, proxy/loopback, exit, UI pause, restart and
  historical provenance/export. Scope each native TCB comparison to its lifetime.
- Live filtered observer CPU/memory at high traffic and many connections, not
  just previous EStats-only load measurements.
- Clean full Release x64 build, map/import/API7/load/render checks, DLL strictly
  below 600000 bytes WITH the runtime source tracker and observer actually used.
- Only after the full gates pass: use the verified pre-v2 backup, clear Oct 17
  consistently across raw records, cumulative state, checkpoints and summaries,
  preserve Oct 16 and earlier, install the new DLL under the user's conditional
  authorization, and test the fresh observation interval without rescaling it.
