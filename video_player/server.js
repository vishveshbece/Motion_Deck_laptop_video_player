const http = require('http');
const fs = require('fs');
const os = require('os');
const path = require('path');

const PORT = Number(process.env.PORT || 8080);
const PAGE = path.join(__dirname, 'index.html');
const clients = new Set();
let latestSample = null;

function sendJson(response, status, value) {
  response.writeHead(status, {
    'Content-Type': 'application/json; charset=utf-8',
    'Cache-Control': 'no-store',
  });
  response.end(JSON.stringify(value));
}

function publish(sample) {
  const frame = `data: ${JSON.stringify(sample)}\n\n`;
  for (const client of clients) {
    if (!client.write(frame)) {
      clients.delete(client);
      client.end();
    }
  }
}

const server = http.createServer((request, response) => {
  const requestUrl = new URL(request.url, 'http://localhost');

  if (request.method === 'GET' && requestUrl.pathname === '/') {
    fs.readFile(PAGE, (error, page) => {
      if (error) return sendJson(response, 500, { error: 'Player page could not be read.' });
      response.writeHead(200, {
        'Content-Type': 'text/html; charset=utf-8',
        'Cache-Control': 'no-store',
      });
      response.end(page);
    });
    return;
  }

  if (request.method === 'GET' && requestUrl.pathname === '/health') {
    sendJson(response, 200, { ok: true, boardConnected: latestSample !== null, clients: clients.size });
    return;
  }

  if (request.method === 'GET' && requestUrl.pathname === '/events') {
    response.writeHead(200, {
      'Content-Type': 'text/event-stream',
      'Cache-Control': 'no-cache, no-transform',
      Connection: 'keep-alive',
      'X-Accel-Buffering': 'no',
    });
    response.write('retry: 1500\n\n');
    clients.add(response);
    if (latestSample) response.write(`data: ${JSON.stringify(latestSample)}\n\n`);
    request.on('close', () => clients.delete(response));
    return;
  }

  if (request.method === 'POST' && requestUrl.pathname === '/coordinates') {
    let body = '';
    request.on('data', (chunk) => {
      body += chunk;
      if (body.length > 2048) {
        request.destroy();
      }
    });
    request.on('end', () => {
      let sample;
      try {
        sample = JSON.parse(body);
      } catch {
        return sendJson(response, 400, { error: 'Expected a JSON acceleration sample.' });
      }

      const x = Number(sample.x);
      const y = Number(sample.y);
      const z = Number(sample.z);
      if (![x, y, z].every(Number.isFinite)) {
        return sendJson(response, 400, { error: 'Sample must contain finite x, y, and z values.' });
      }

      latestSample = { x, y, z, unit: sample.unit === 'mg' ? 'mg' : 'unknown', receivedAt: Date.now() };
      publish(latestSample);
      sendJson(response, 200, { ok: true });
    });
    return;
  }

  sendJson(response, 404, { error: 'Not found.' });
});

server.listen(PORT, '0.0.0.0', () => {
  console.log(`Video player server listening on port ${PORT}`);
  console.log(`Open on this laptop: http://localhost:${PORT}/`);
  for (const interfaces of Object.values(os.networkInterfaces())) {
    for (const address of interfaces || []) {
      if (address.family === 'IPv4' && !address.internal) {
        console.log(`Board/server LAN address: http://${address.address}:${PORT}/`);
      }
    }
  }
  console.log('Board POST endpoint: /coordinates');
});

server.on('error', (error) => {
  console.error(`Could not start the video player server: ${error.message}`);
  process.exitCode = 1;
});
