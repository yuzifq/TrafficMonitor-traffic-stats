const fs = require('node:fs');

const prefix = process.argv[2];
if (!prefix) throw new Error('Specify the controlled capture prefix');
const port = Number(fs.readFileSync(prefix + '.control', 'utf8').trim().split(' ')[1]);
const report = fs.readFileSync(prefix + '.report', 'utf8');
const times = report.match(/^times\t(\d+)\t(\d+)/m);
if (!times) throw new Error('Missing counter interval');
const start = BigInt(times[1]), end = BigInt(times[2]);
const input = fs.readFileSync(prefix + '.pcapng');
const interfaces = [];
const groups = { rx: group(), tx: group() };
let packets = 0, outside = 0, unrelated = 0, unsupported = 0;
const rejectedHeaders = [];
function group() {
    return { packets: 0, controlPackets: 0, frameBytes: 0, declaredPayload: 0,
        framePayload: 0, offloadPackets: 0, offloadPayload: 0, lengthDisagreements: 0, repeatedSequenceBytes: 0, uniqueSequenceBytes: 0,
        sequenceOrigin: null, ranges: [] };
}
function sequence(row, value, size) {
    if (!size) return;
    if (row.sequenceOrigin === null) row.sequenceOrigin = value;
    const left = (value - row.sequenceOrigin) | 0;
    const right = left + size;
    let repeated = 0;
    for (const [a, b] of row.ranges) repeated += Math.max(0, Math.min(b, right) - Math.max(a, left));
    row.repeatedSequenceBytes += repeated;
    row.uniqueSequenceBytes += size - repeated;
    row.ranges.push([left, right]);
    row.ranges.sort((a, b) => a[0] - b[0]);
    const merged = [];
    for (const span of row.ranges) {
        const last = merged.at(-1);
        if (last && span[0] <= last[1]) last[1] = Math.max(last[1], span[1]);
        else merged.push(span);
    }
    row.ranges = merged;
}
for (let offset = 0; offset < input.length;) {
    if (offset + 12 > input.length) throw new Error('Truncated pcapng block');
    const type = input.readUInt32LE(offset), length = input.readUInt32LE(offset + 4);
    if (length < 12 || offset + length > input.length || input.readUInt32LE(offset + length - 4) !== length)
        throw new Error('Invalid pcapng block bounds');
    const block = input.subarray(offset, offset + length);
    offset += length;
    if (type === 0x0a0d0d0a) {
        if (block.readUInt32LE(8) !== 0x1a2b3c4d) throw new Error('Unsupported pcapng byte order');
        continue;
    }
    if (type === 1) {
        let scale = 1000000n;
        for (let at = 16; at + 4 <= length - 4;) {
            const code = block.readUInt16LE(at), size = block.readUInt16LE(at + 2);
            if (!code) break;
            if (at + 4 + size > length - 4) throw new Error('Invalid pcapng interface option');
            if (code === 9) scale = block[at + 4] & 128 ? 2n ** BigInt(block[at + 4] & 127) : 10n ** BigInt(block[at + 4]);
            at += 4 + ((size + 3) & ~3);
        }
        interfaces.push({ link: block.readUInt16LE(8), scale });
        continue;
    }
    if (type !== 6) continue;
    ++packets;
    const iface = interfaces[block.readUInt32LE(8)];
    if (!iface || iface.link !== 1) { ++unsupported; continue; }
    const ticks = (BigInt(block.readUInt32LE(12)) << 32n) | BigInt(block.readUInt32LE(16));
    const timestamp = ticks * 10000000n / iface.scale + 116444736000000000n;
    if (timestamp < start || timestamp > end) { ++outside; continue; }
    const captured = block.readUInt32LE(20), original = block.readUInt32LE(24);
    if (captured > original || 28 + captured > length - 4) throw new Error('Invalid pcapng packet length');
    const frame = block.subarray(28, 28 + captured);
    if (frame.length < 14) { ++unsupported; continue; }
    let ether = frame.readUInt16BE(12), ip = 14;
    while ((ether === 0x8100 || ether === 0x88a8) && frame.length >= ip + 4) {
        ether = frame.readUInt16BE(ip + 2); ip += 4;
    }
    let tcp, ipSize, offload = false;
    if (ether === 0x86dd && frame.length >= ip + 40 && frame[ip + 6] === 6) {
        tcp = ip + 40; ipSize = 40 + frame.readUInt16BE(ip + 4);
        // LSOv2 deliberately leaves the IPv6 payload length zero. PktMon's
        // OriginalSize describes the NET_BUFFER; do not invent an IP length.
        offload = ipSize === 40;
    } else if (ether === 0x800 && frame.length >= ip + 20 && frame[ip + 9] === 6) {
        tcp = ip + (frame[ip] & 15) * 4; ipSize = frame.readUInt16BE(ip + 2);
    } else { ++unsupported; continue; }
    if (frame.length < tcp + 20) { ++unsupported; continue; }
    const source = frame.readUInt16BE(tcp), target = frame.readUInt16BE(tcp + 2);
    const direction = source === port && target === 443 ? 'tx' : source === 443 && target === port ? 'rx' : null;
    if (!direction) { ++unrelated; continue; }
    const header = (frame[tcp + 12] >> 4) * 4;
    if (header < 20 || original < tcp + header || (!offload && ipSize < tcp - ip + header)) {
        ++unsupported;
        rejectedHeaders.push({ original, captured, ipSize, tcpOffset: tcp, tcpHeader: header,
            sourcePort: source, targetPort: target, sequence: frame.readUInt32BE(tcp + 4), flags: frame[tcp + 13] });
        continue;
    }
    const declared = offload ? null : ipSize - (tcp - ip) - header;
    const payload = original - tcp - header;
    const row = groups[direction];
    ++row.packets; row.frameBytes += original; row.framePayload += payload;
    if (offload) { ++row.offloadPackets; row.offloadPayload += payload; }
    else { row.declaredPayload += declared; if (declared !== payload) ++row.lengthDisagreements; }
    if (!payload) ++row.controlPackets;
    const flags = frame[tcp + 13];
    sequence(row, (frame.readUInt32BE(tcp + 4) + (flags & 2 ? 1 : 0)) >>> 0, payload);
}
for (const row of Object.values(groups)) { delete row.sequenceOrigin; delete row.ranges; }
const known = report.match(/knownTCPRx\t(\d+)\tknownTCPTx\t(\d+)/);
if (!known) throw new Error('Missing independently verified TCP byte counts');
const summary = { start: start.toString(), end: end.toString(), packets, outside, unrelated, unsupported, rejectedHeaders, groups,
    expectedUnique: { rx: Number(known[1]), tx: Number(known[2]) },
    layer: 'NDIS upper edge; offloaded buffer sizes are not post-segmentation wire sizes' };
fs.writeFileSync(prefix + '.packet-summary.json', JSON.stringify(summary, null, 2));
console.log(JSON.stringify(summary, null, 2));
if (!packets || unsupported || unrelated || groups.rx.lengthDisagreements || groups.tx.lengthDisagreements)
    throw new Error('Capture is not sufficient for an unqualified byte-accounting claim');
if (groups.rx.uniqueSequenceBytes !== summary.expectedUnique.rx || groups.tx.uniqueSequenceBytes !== summary.expectedUnique.tx)
    throw new Error('Captured sequence coverage does not account for the independently verified stream');
