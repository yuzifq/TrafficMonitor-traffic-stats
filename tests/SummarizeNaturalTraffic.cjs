const assert = require('node:assert/strict');
const fs = require('node:fs');
const crypto = require('node:crypto');

const prefix = process.argv[2];
assert(prefix, 'Specify the natural capture prefix');
const report = fs.readFileSync(prefix, 'utf8').trim().split(/\r?\n/).map(line => line.split('\t'));
const start = BigInt(report.find(row => row[0] === 'begin')[1]);
const end = BigInt(report.find(row => row[0] === 'end')[1]);
const localMac = 'bc0ff3d83430';
const input = fs.readFileSync(prefix + '.pcapng');
const interfaces = [];
const group = () => ({ packets: 0, frameBytes: 0, tcpPayload: 0, udpPayload: 0, headers: 0,
    controlPackets: 0, otherPackets: 0, otherBytes: 0, offloadPackets: 0, unsupportedPackets: 0 });
const groups = { rx: group(), tx: group() };
const flows = new Map();
function flow(protocol, family, a, ap, b, bp) {
    const endpoints = [`${a}:${ap}`, `${b}:${bp}`].sort();
    const key = `${protocol}/${family}/${endpoints.join('/')}`;
    if (!flows.has(key)) flows.set(key, { id: crypto.createHash('sha256').update(key).digest('hex').slice(0, 12),
        protocol, family, ports: [ap, bp].sort((a, b) => a - b), pids: [],
        rx: { payload: 0, raw: 0, buffers: 0, offloads: 0 }, tx: { payload: 0, raw: 0, buffers: 0, offloads: 0 } });
    return flows.get(key);
}
let outside = 0;
for (let offset = 0; offset < input.length;) {
    assert(offset + 12 <= input.length, 'Truncated pcapng block');
    const type = input.readUInt32LE(offset), length = input.readUInt32LE(offset + 4);
    assert(length >= 12 && offset + length <= input.length && input.readUInt32LE(offset + length - 4) === length,
        'Invalid pcapng block bounds');
    const block = input.subarray(offset, offset + length);
    offset += length;
    if (type === 0x0a0d0d0a) {
        assert.equal(block.readUInt32LE(8), 0x1a2b3c4d, 'Unsupported pcapng byte order');
        interfaces.length = 0;
    }
    if (type === 1) {
        let scale = 1000000n;
        for (let at = 16; at + 4 <= length - 4;) {
            const code = block.readUInt16LE(at), size = block.readUInt16LE(at + 2);
            if (!code) break;
            assert(at + 4 + size <= length - 4, 'Invalid interface option');
            if (code === 9) scale = block[at + 4] & 128 ? 2n ** BigInt(block[at + 4] & 127) : 10n ** BigInt(block[at + 4]);
            at += 4 + ((size + 3) & ~3);
        }
        interfaces.push({ link: block.readUInt16LE(8), scale });
    }
    if (type !== 6) continue;
    const iface = interfaces[block.readUInt32LE(8)];
    assert(iface && iface.link === 1, 'Unsupported link layer');
    const ticks = (BigInt(block.readUInt32LE(12)) << 32n) | BigInt(block.readUInt32LE(16));
    const time = ticks * 10000000n / iface.scale + 116444736000000000n;
    if (time < start || time >= end) { ++outside; continue; }
    const captured = block.readUInt32LE(20), original = block.readUInt32LE(24);
    assert(captured <= original && 28 + captured <= length - 4, 'Invalid captured size');
    const frame = block.subarray(28, 28 + captured);
    assert(frame.length >= 14, 'Truncated Ethernet header');
    const direction = frame.subarray(6, 12).toString('hex') === localMac ? 'tx' : 'rx';
    const row = groups[direction];
    ++row.packets; row.frameBytes += original;
    let ether = frame.readUInt16BE(12), ip = 14;
    while (ether === 0x8100 || ether === 0x88a8) {
        assert(frame.length >= ip + 4, 'Truncated VLAN header');
        ether = frame.readUInt16BE(ip + 2); ip += 4;
    }
    let transport, protocol, ipSize, family, source, target, offload = false;
    if (ether === 0x800 && frame.length >= ip + 20) {
        transport = ip + (frame[ip] & 15) * 4; protocol = frame[ip + 9]; ipSize = frame.readUInt16BE(ip + 2);
        family = 4; source = frame.subarray(ip + 12, ip + 16).toString('hex'); target = frame.subarray(ip + 16, ip + 20).toString('hex');
        offload = ipSize === 0 && protocol === 6;
        if (frame.readUInt16BE(ip + 6) & 0x3fff) { ++row.unsupportedPackets; continue; }
    } else if (ether === 0x86dd && frame.length >= ip + 40) {
        transport = ip + 40; protocol = frame[ip + 6]; ipSize = 40 + frame.readUInt16BE(ip + 4);
        family = 6; source = frame.subarray(ip + 8, ip + 24).toString('hex'); target = frame.subarray(ip + 24, ip + 40).toString('hex');
        offload = ipSize === 40 && protocol === 6;
    } else {
        ++row.otherPackets; row.otherBytes += original; continue;
    }
    let header;
    if (protocol === 6 && frame.length >= transport + 20) {
        header = (frame[transport + 12] >> 4) * 4;
        if (header < 20) { ++row.unsupportedPackets; continue; }
    } else if (protocol === 17 && frame.length >= transport + 8) header = 8;
    else { ++row.otherPackets; row.otherBytes += original; continue; }
    const payload = offload ? original - transport - header : ipSize - (transport - ip) - header;
    if (payload < 0 || original < transport + header || payload > original - transport - header) {
        ++row.unsupportedPackets; continue;
    }
    if (offload) ++row.offloadPackets;
    if (!payload) ++row.controlPackets;
    row.headers += original - payload;
    if (protocol === 6) row.tcpPayload += payload;
    else row.udpPayload += payload;
    const connection = flow(protocol, family, source, frame.readUInt16BE(transport), target, frame.readUInt16BE(transport + 2));
    connection[direction].payload += payload;
    ++connection[direction].buffers;
    if (offload) ++connection[direction].offloads;
}
for (const row of report.filter(row => row[0] === 'flow')) {
    const family = Number(row[2]), length = family === 4 ? 8 : 32;
    const connection = flow(Number(row[1]), family, row[4].slice(0, length), Number(row[5]), row[6].slice(0, length), Number(row[7]));
    if (!connection.pids.includes(Number(row[3]))) connection.pids.push(Number(row[3]));
    connection.rx.raw += Number(row[8]); connection.tx.raw += Number(row[9]);
}
const guid = '{264301E4-78FC-44D5-819C-CD2B099E2637}';
const adapter = report.filter(row => row[0] === 'adapter' && row[3] === guid);
const raw = report.filter(row => row[0] === 'raw_final').reduce((total, row) => {
    total.rx += Number(row[3]); total.tx += Number(row[4]); return total;
}, { rx: 0, tx: 0 });
const nic = { rx: Number(adapter.at(-1)[4]) - Number(adapter[0][4]), tx: Number(adapter.at(-1)[5]) - Number(adapter[0][5]) };
const summary = { start: String(start), end: String(end), outside, groups, nic, raw,
    residual: Object.fromEntries(['rx', 'tx'].map(direction => [direction, {
        nicMinusFrame: nic[direction] - groups[direction].frameBytes,
        payloadMinusEtw: groups[direction].tcpPayload + groups[direction].udpPayload - raw[direction],
        nicMinusEtw: nic[direction] - raw[direction]
    }])), flowDifferences: [...flows.values()].filter(row => row.pids.length &&
        Math.abs(row.rx.payload - row.rx.raw) + Math.abs(row.tx.payload - row.tx.raw) > 1000)
        .sort((a, b) => (b.tx.raw - b.tx.payload) - (a.tx.raw - a.tx.payload)).slice(0, 20),
    layer: 'Selected physical NIC NDIS edge; offload buffers are not segmented wire packets' };
console.log(JSON.stringify(summary, null, 2));
