const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');

const [reportPath, root] = process.argv.slice(2);
assert(reportPath && root, 'Usage: node CompareRawProcess.cjs REPORT HOST_ROOT');
const rows = fs.readFileSync(reportPath, 'utf8').trim().split(/\r?\n/).map(line => line.split('\t'));
assert(rows.some(row => row[0] === 'done'), 'Audit has not finished');
const begin = BigInt(rows.find(row => row[0] === 'begin')[1]) / 10000000n;
const end = BigInt(rows.find(row => row[0] === 'end')[1]) / 10000000n;
// Leave observed traffic on both sides of each chosen minute boundary.
const first = (begin + 3n + 59n) / 60n * 60n, last = (end - 3n) / 60n * 60n;
assert(last > first, 'Not enough complete minutes');
const epoch = 11644473600n;
const localDate = second => new Date(Number(second - epoch) * 1000);
const minuteKey = date => `${String(date.getHours()).padStart(2, '0')}:${String(date.getMinutes()).padStart(2, '0')}`;
const date = localDate(first), dateEnd = localDate(last - 1n);
assert.equal(date.toDateString(), dateEnd.toDateString(), 'Cross-day audit needs separate daily files');
const day = `${date.getFullYear()}-${String(date.getMonth() + 1).padStart(2, '0')}-${String(date.getDate()).padStart(2, '0')}`;
const minutes = new Map();
for (let second = first; second < last; second += 60n)
    minutes.set(minuteKey(localDate(second)), { processRx: 0, processTx: 0, rawRx: 0, rawTx: 0 });
const rawSeconds = rows.filter(row => row[0] === 'second').map(row => [BigInt(row[1]), Number(row[2]), Number(row[3])]);
for (const [second, rx, tx] of rawSeconds) {
    if (second < first || second >= last) continue;
    const row = minutes.get(minuteKey(localDate(second)));
    row.rawRx += rx; row.rawTx += tx;
}
const digits = '0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ';
function decode(text) {
    assert(text.length, 'Empty Base62 field');
    let value = 0n;
    for (const c of text) {
        const digit = digits.indexOf(c);
        assert(digit >= 0, 'Invalid Base62 digit');
        value = value * 62n + BigInt(digit);
    }
    assert(value <= BigInt(Number.MAX_SAFE_INTEGER), 'Unsafe comparison integer');
    return Number(value);
}
let time;
const historyPath = path.join(root, 'app_traffic_history', day.slice(0, 4), day.slice(5, 7), day + '.tsv');
for (const line of fs.readFileSync(historyPath, 'utf8').trim().split(/\r?\n/)) {
    const fields = line.split('\t');
    assert.equal(fields.length, 4, 'Unexpected process history format');
    if (fields[0]) time = fields[0];
    const row = minutes.get(time);
    if (!row) continue;
    row.processRx += decode(fields[2]); row.processTx += decode(fields[3]);
}
const totals = [...minutes.values()].reduce((total, row) => {
    for (const key of Object.keys(total)) total[key] += row[key];
    return total;
}, { processRx: 0, processTx: 0, rawRx: 0, rawTx: 0 });
const boundaryTraffic = { rx: 0, tx: 0 };
for (const [second, rx, tx] of rawSeconds)
    if ((second >= first - 3n && second < first + 3n) || (second >= last - 3n && second < last + 3n)) {
        boundaryTraffic.rx += rx; boundaryTraffic.tx += tx;
    }
console.log(JSON.stringify({ day, from: minuteKey(localDate(first)), toExclusive: minuteKey(localDate(last)),
    minutes: Object.fromEntries(minutes), totals,
    processMinusRaw: { rx: totals.processRx - totals.rawRx, tx: totals.processTx - totals.rawTx },
    trafficWithinThreeSecondsOfBoundaries: boundaryTraffic,
    rawHealth: rows.find(row => row[0] === 'raw_health')?.slice(1),
    caveat: 'Minute placement can differ with delivery, publication and source-switch latency; boundary traffic is not a hard error bound.'
}, null, 2));
