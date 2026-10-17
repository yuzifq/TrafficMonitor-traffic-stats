const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');

const digits = '0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ';
const max = (1n << 64n) - 1n;
function base62(text) {
    assert(text.length, 'Empty Base62 field');
    let value = 0n;
    for (const char of text) {
        const digit = digits.indexOf(char);
        assert(digit >= 0, 'Invalid Base62 field');
        value = value * 62n + BigInt(digit);
        assert(value <= max, 'Base62 overflow');
    }
    return value;
}
function decimal(text) {
    assert(/^\d+$/.test(text), 'Invalid unsigned decimal field');
    const value = BigInt(text);
    assert(value <= max, 'Decimal overflow');
    return value;
}
function fnv(text) {
    let hash = 14695981039346656037n;
    for (const byte of Buffer.from(text)) hash = BigInt.asUintN(64, (hash ^ BigInt(byte)) * 1099511628211n);
    return hash;
}
function coverage(spans) {
    spans.sort((a, b) => a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0);
    let total = 0n, left = 0n, right = 0n;
    for (const span of spans) {
        if (span[0] > right) { total += right - left; left = span[0]; }
        if (span[1] > right) right = span[1];
    }
    return total + right - left;
}
function minute(text, allowEnd = false) {
    assert(/^(?:[01]\d|2[0-3]):[0-5]\d$/.test(text) || (allowEnd && text === '24:00'), 'Invalid time');
    return Number(text.slice(0, 2)) * 60 + Number(text.slice(3));
}
function lines(text) { return text.replace(/^\uFEFF/, '').split(/\r?\n/).filter(line => line.length); }
function isAnonymous(name) { return /^PID\d+$/i.test(name) || /^v2:unknown:\d+:\d+:\d+$/.test(name); }
function officialDay(text, date) {
    const matches = lines(text).filter(line => line.startsWith(date.replaceAll('-', '/') + ' '));
    assert.equal(matches.length, 1, 'Expected one official daily record');
    const fields = matches[0].match(/^\d{4}\/\d{2}\/\d{2} (\d+)\/(\d+)$/);
    assert(fields, 'Official mixed upload/download record cannot be compared by direction');
    // TrafficMonitor 1.86 persists upload/download in KiB, in this order.
    return { rx: decimal(fields[2]) * 1024n, tx: decimal(fields[1]) * 1024n };
}
function selfTest() {
    assert.equal(base62('10'), 62n);
    assert.equal(base62('Z'), 61n);
    assert.throws(() => base62('-1'));
    assert.throws(() => base62('ZZZZZZZZZZZZZZZ'));
    assert.throws(() => decimal('1e3'));
    assert.equal(fnv('hello'), 0xa430d84680aabd0bn);
    assert.equal(coverage([[10n, 20n], [0n, 15n], [30n, 32n]]), 22n);
    assert.equal(minute('24:00', true), 1440);
    assert.throws(() => minute('24:00'));
    assert.deepEqual(officialDay('lines: "1"\n2026/10/16 2/3\n', '2026-10-16'), { rx: 3072n, tx: 2048n });
    assert.throws(() => officialDay('2026/10/16 2\n', '2026-10-16'));
    assert(isAnonymous('PID42') && isAnonymous('v2:unknown:42:0:7') && !isAnonymous('v2:path:C:\\test.exe'));
    console.log('PASS: read-only history audit parsing, checksums and interval coverage');
}
function audit(root, date, from, to) {
    assert(/^\d{4}-\d{2}-\d{2}$/.test(date), 'Expected YYYY-MM-DD');
    const startMinute = minute(from), endMinute = minute(to, true);
    assert(startMinute < endMinute, 'Expected a nonempty half-open interval');
    const day = new Date(`${date}T00:00:00`);
    assert(!Number.isNaN(day.valueOf()) && `${day.getFullYear()}-${String(day.getMonth() + 1).padStart(2, '0')}-${String(day.getDate()).padStart(2, '0')}` === date, 'Invalid date');
    const start = BigInt(day.valueOf() + startMinute * 60000) + 11644473600000n;
    const end = BigInt(day.valueOf() + endMinute * 60000) + 11644473600000n;
    const tail = path.join(date.slice(0, 4), date.slice(5, 7), `${date}.tsv`);
    const processPath = path.join(root, 'app_traffic_history', tail);
    const adapterPath = path.join(root, 'network_traffic_history', tail);
    const dictionary = new Map();
    for (const line of lines(fs.readFileSync(path.join(root, 'app_traffic_history', 'apps.tsv'), 'utf8'))) {
        const fields = line.split('\t');
        if (fields[0] === 'version') { assert.equal(fields[1], '4', 'Unsupported dictionary version'); continue; }
        assert(fields[0] === 'app' && fields.length >= 3, 'Invalid dictionary row');
        assert(!dictionary.has(fields[1]), 'Duplicate dictionary ID');
        dictionary.set(fields[1], fields[2]);
    }
    const processRaw = fs.readFileSync(processPath, 'utf8');
    const adapterRaw = fs.readFileSync(adapterPath, 'utf8');
    assert(processRaw.endsWith('\n') && adapterRaw.endsWith('\n'), 'Incomplete daily file');
    const hash = text => crypto.createHash('sha256').update(text).digest('hex');
    const process = { rx: 0n, tx: 0n, anonymousRx: 0n, anonymousTx: 0n, rows: 0 };
    let time;
    for (const line of lines(processRaw)) {
        const fields = line.split('\t');
        assert.equal(fields.length, 4, 'Unsupported process daily format');
        if (fields[0]) time = minute(fields[0]);
        assert(time !== undefined, 'Missing compact-row minute');
        const name = dictionary.get(fields[1]);
        assert(name !== undefined, 'Unknown application ID');
        const rx = base62(fields[2]), tx = base62(fields[3]);
        if (time < startMinute || time >= endMinute) continue;
        process.rx += rx; process.tx += tx; ++process.rows;
        if (isAnonymous(name)) { process.anonymousRx += rx; process.anonymousTx += tx; }
    }
    const adapter = { rx: 0n, tx: 0n, boundaryRx: 0n, boundaryTx: 0n, coarseRecords: 0, records: 0 };
    const ids = new Set(), spans = [];
    let previousId = 0n;
    const rows = lines(adapterRaw);
    assert.equal(rows.shift(), 'network\t1', 'Unsupported adapter history version');
    for (const line of rows) {
        const fields = line.split('\t');
        assert.equal(fields.length, 8, 'Invalid adapter record');
        const [id, left, right, rx, tx] = fields.slice(0, 5).map(decimal);
        assert(id > previousId && right > left, 'Invalid record ordering');
        assert.equal(decimal(fields[7]), fnv(line.slice(0, line.lastIndexOf('\t'))), 'Adapter checksum mismatch');
        assert(fields[6] === '0' || fields[6] === '1', 'Invalid coarse flag');
        previousId = id;
        if (right <= start || left >= end) continue;
        ids.add(fields[5]); ++adapter.records;
        if (fields[6] === '1') ++adapter.coarseRecords;
        if (left >= start && right <= end) { adapter.rx += rx; adapter.tx += tx; }
        else { adapter.boundaryRx += rx; adapter.boundaryTx += tx; }
        spans.push([left > start ? left : start, right < end ? right : end]);
    }
    assert(ids.size <= 1, 'Multiple adapter identities: do not compare a mixed total');
    adapter.coveredMs = coverage(spans); adapter.requestedMs = end - start;
    assert.equal(hash(fs.readFileSync(processPath, 'utf8')), hash(processRaw), 'Process source changed during audit');
    assert.equal(hash(fs.readFileSync(adapterPath, 'utf8')), hash(adapterRaw), 'Adapter source changed during audit');
    const state = lines(fs.readFileSync(path.join(root, 'app_traffic_state.tsv'), 'utf8')).map(line => line.split('\t'));
    const percent = (part, total) => total ? Number((total - part) * 1000000n / total) / 10000 : null;
    let official;
    if (startMinute === 0 && endMinute === 1440) {
        const officialPath = path.join(root, 'history_traffic.dat');
        const totals = officialDay(fs.readFileSync(officialPath, 'utf8'), date);
        assert.deepEqual(officialDay(fs.readFileSync(officialPath, 'utf8'), date), totals, 'Official day changed during audit');
        official = { ...totals, pluginAdapterMinusOfficial: { rx: adapter.rx - totals.rx, tx: adapter.tx - totals.tx },
            adapterDifferencePercent: { rx: -percent(adapter.rx, totals.rx), tx: -percent(adapter.tx, totals.tx) } };
    }
    console.log(JSON.stringify({ date, interval: `[${from}, ${to})`, timezone: Intl.DateTimeFormat().resolvedOptions().timeZone,
        process, adapter, adapterIds: [...ids], shortfallPercent: { rx: percent(process.rx, adapter.rx), tx: percent(process.tx, adapter.tx) },
        officialDay: official, savedRange: Object.fromEntries(state.filter(fields => fields[0] === 'range_start' || fields[0] === 'range_end')),
        sourceSha256: { process: hash(processRaw), adapter: hash(adapterRaw) }, readOnly: true },
        (_, value) => typeof value === 'bigint' ? value.toString() : value, 2));
}
if (process.argv[2] === '--self-test') selfTest();
else {
    assert.equal(process.argv.length, 6, 'Usage: node AuditTrafficHistory.cjs ROOT YYYY-MM-DD HH:mm HH:mm');
    audit(...process.argv.slice(2));
}
