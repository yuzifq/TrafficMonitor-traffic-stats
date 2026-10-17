const fs = require('node:fs');
const net = require('node:net');
const tls = require('node:tls');
const http = require('node:http');
const { Duplex } = require('node:stream');

const output = process.argv[2];
if (!output) throw new Error('Specify a diagnostic report prefix');
const host = 'postman-echo.com';
let wireIn = 0, wireOut = 0;
const raw = net.connect({ host, port: 443, family: Number(process.argv[3]) || 0 });
const bridge = new Duplex({
    read() { raw.resume(); },
    write(chunk, encoding, callback) { wireOut += chunk.length; raw.write(chunk, callback); },
    final(callback) { raw.end(callback); }
});
raw.on('data', chunk => { wireIn += chunk.length; if (!bridge.push(chunk)) raw.pause(); });
raw.on('end', () => bridge.push(null));
raw.on('error', error => bridge.destroy(error));
const secure = tls.connect({ socket: bridge, servername: host, rejectUnauthorized: true });
const timeout = setTimeout(() => fail(new Error('External TCP probe timed out')), 60000);
function fail(error) {
    fs.writeFileSync(output + '.error', error.message);
    secure.destroy(); raw.destroy();
    clearTimeout(timeout);
    console.error(error.message);
    process.exitCode = 1;
}
secure.on('error', fail);
function waitFor(file) {
    return new Promise((resolve, reject) => {
        let tries = 0;
        const timer = setInterval(() => {
            if (fs.existsSync(file)) { clearInterval(timer); resolve(); }
            else if (file !== output + '.quit' && fs.existsSync(output + '.quit')) {
                clearInterval(timer); reject(new Error('Diagnostic peer stopped before ' + file));
            }
            else if (++tries > 2500) { clearInterval(timer); reject(new Error('Diagnostic peer did not signal ' + file)); }
        }, 20);
    });
}
secure.once('secureConnect', async () => {
    try {
        fs.writeFileSync(output + '.control', `${process.pid} ${raw.localPort} ${raw.remoteFamily === 'IPv6' ? 6 : 4}\n`);
        fs.writeFileSync(output + '.connection.json', JSON.stringify({ pid: process.pid, localPort: raw.localPort,
            remotePort: raw.remotePort, family: raw.remoteFamily, localAddress: raw.localAddress, remoteAddress: raw.remoteAddress }));
        console.log('TLS connection ready for counter baselines');
        await waitFor(output + '.go');
        const baselineIn = wireIn, baselineOut = wireOut;
        const socketBaselineIn = raw.bytesRead, socketBaselineOut = raw.bytesWritten;
        const expected = 'z'.repeat(65536);
        const body = JSON.stringify({ padding: expected });
        const agent = new http.Agent({ keepAlive: true, maxSockets: 1 });
        agent.createConnection = () => secure;
        const response = await new Promise((resolve, reject) => {
            const request = http.request({ host, port: 443, path: '/post', method: 'POST', agent,
                headers: { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(body), Connection: 'keep-alive' }
            }, res => {
                const chunks = [];
                res.on('data', chunk => chunks.push(chunk));
                res.on('end', () => resolve({ status: res.statusCode, body: Buffer.concat(chunks) }));
            });
            request.on('error', reject);
            request.end(body);
        });
        const echoed = JSON.parse(response.body.toString('utf8'));
        if (response.status !== 200 || echoed.json?.padding !== expected) throw new Error('Echo server did not return the exact fixed test string');
        if (raw.bytesRead - socketBaselineIn !== wireIn - baselineIn || raw.bytesWritten - socketBaselineOut !== wireOut - baselineOut)
            throw new Error('TLS bridge counters disagree with native socket byte totals');
        // Count the actual encrypted TCP stream, not plaintext HTTP/TLS estimates.
        fs.writeFileSync(output + '.wire', `${wireIn - baselineIn} ${wireOut - baselineOut} ${response.status}\n`);
        fs.writeFileSync(output + '.wire-total', `${wireIn} ${wireOut}\n`);
        console.log('Verified echo; encrypted TCP byte counters published');
        await waitFor(output + '.quit');
        agent.destroy(); secure.destroy(); raw.destroy();
        clearTimeout(timeout);
    } catch (error) { fail(error); }
});
