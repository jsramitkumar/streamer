
'use strict';

const express    = require('express');
const http       = require('http');
const WebSocket  = require('ws');
const dgram      = require('dgram');
const mqtt       = require('mqtt');
const fs         = require('fs');
const path       = require('path');
const { spawn }  = require('child_process');
const axios      = require('axios');

// ─── Configuration (override via environment variables) ───────────────────────
const PORT            = parseInt(process.env.PORT      || '3000', 10);
const UDP_PORT        = parseInt(process.env.UDP_PORT  || '5000', 10);
const STATIC_DIR      = path.resolve(process.env.STATIC_DIR || './public');
const AI_URL          = process.env.AI_URL             || 'http://ai:5001';
const MQTT_URL        = process.env.MQTT_URL           || 'mqtt://mqtt:1883';
const MQTT_USER       = process.env.MQTT_USER          || '';
const MQTT_PASS       = process.env.MQTT_PASS          || '';
const REC_BASE        = path.resolve(process.env.REC_PATH || './recordings');
const AI_SAMPLE_RATE  = parseFloat(process.env.AI_SAMPLE_RATE || '0.33'); // fraction of frames sent to AI
const MAX_FRAME_BYTES = parseInt(process.env.MAX_FRAME_BYTES  || String(5 * 1024 * 1024), 10); // 5 MB
const SEG_TIME        = parseInt(process.env.SEG_TIME  || '300', 10);  // seconds per recording segment
const CORS_ORIGIN     = process.env.CORS_ORIGIN || '*';
// ─────────────────────────────────────────────────────────────────────────────

// Validate camera ID: alphanumeric + dash/underscore only (prevent path traversal)
const CAMERA_ID_RE = /^[a-zA-Z0-9_-]{1,64}$/;
function isValidCamId(id) {
  return typeof id === 'string' && CAMERA_ID_RE.test(id);
}

// Structured log helper
function log(level, msg, meta) {
  const entry = { ts: new Date().toISOString(), level, msg, ...meta };
  if (level === 'error') console.error(JSON.stringify(entry));
  else console.log(JSON.stringify(entry));
}

// ─── Express app ─────────────────────────────────────────────────────────────
const app = express();
app.use(express.json({ limit: '1mb' }));

// Serve the SPA (index.html + any future static assets)
app.use(express.static(STATIC_DIR));

// Security headers
app.use((req, res, next) => {
  res.setHeader('X-Content-Type-Options', 'nosniff');
  res.setHeader('X-Frame-Options', 'DENY');
  res.setHeader('X-XSS-Protection', '1; mode=block');
  res.setHeader('Access-Control-Allow-Origin', CORS_ORIGIN);
  next();
});

// Health / readiness probe
app.get('/healthz', (req, res) => {
  res.json({ status: 'ok', cameras: Object.keys(cameras), uptime: process.uptime() });
});

// List recordings for a camera
app.get('/api/recordings/:cam', (req, res) => {
  const camId = req.params.cam;
  if (!isValidCamId(camId)) return res.status(400).json({ error: 'Invalid camera ID' });

  const camDir = path.join(REC_BASE, camId);
  // Prevent path traversal: ensure resolved path is under REC_BASE
  if (!camDir.startsWith(REC_BASE + path.sep) && camDir !== REC_BASE) {
    return res.status(400).json({ error: 'Invalid camera ID' });
  }

  if (!fs.existsSync(camDir)) return res.json([]);
  try {
    const files = fs.readdirSync(camDir)
      .filter(f => f.endsWith('.mp4'))
      .sort()
      .reverse();
    res.json(files);
  } catch (err) {
    log('error', 'Failed to list recordings', { camId, err: err.message });
    res.status(500).json({ error: 'Could not list recordings' });
  }
});

// Serve a recording file (range-request aware via pipe)
app.get('/api/recordings/:cam/:file', (req, res) => {
  const camId   = req.params.cam;
  const fileReq = req.params.file;

  if (!isValidCamId(camId)) return res.status(400).json({ error: 'Invalid camera ID' });
  if (!/^[\w.-]+\.mp4$/.test(fileReq)) return res.status(400).json({ error: 'Invalid filename' });

  const filePath = path.join(REC_BASE, camId, fileReq);
  if (!filePath.startsWith(path.join(REC_BASE, camId) + path.sep) &&
      filePath !== path.join(REC_BASE, camId)) {
    return res.status(400).json({ error: 'Invalid path' });
  }

  if (!fs.existsSync(filePath)) return res.status(404).json({ error: 'Not found' });

  res.setHeader('Content-Type', 'video/mp4');
  res.setHeader('Accept-Ranges', 'bytes');
  const stat = fs.statSync(filePath);
  const range = req.headers.range;

  if (range) {
    const [startStr, endStr] = range.replace(/bytes=/, '').split('-');
    const start = parseInt(startStr, 10);
    const end   = endStr ? parseInt(endStr, 10) : stat.size - 1;
    if (start > end || end >= stat.size) return res.status(416).end();
    res.status(206);
    res.setHeader('Content-Range', `bytes ${start}-${end}/${stat.size}`);
    res.setHeader('Content-Length', end - start + 1);
    fs.createReadStream(filePath, { start, end }).pipe(res);
  } else {
    res.setHeader('Content-Length', stat.size);
    fs.createReadStream(filePath).pipe(res);
  }
});

// SPA fallback — serve index.html for any unmatched non-API route
app.use((req, res, next) => {
  if (req.path.startsWith('/api/') || req.path.startsWith('/ws/') || req.path === '/healthz') {
    return res.status(404).json({ error: 'Not found' });
  }
  res.sendFile(path.join(STATIC_DIR, 'index.html'), (err) => {
    if (err) res.status(500).json({ error: 'Could not serve page' });
  });
});

// ─── HTTP + WebSocket server ──────────────────────────────────────────────────
const server = http.createServer(app);
const wss    = new WebSocket.Server({ server, path: '/' });

/** @type {Object.<string, {frame: Buffer, detections: Array}>} */
const cameras = {};

/** @type {Object.<string, import('child_process').ChildProcess>} */
const recorders = {};

// WebSocket: clients subscribe to a camera stream via path /ws/<camId>
wss.on('connection', (ws, req) => {
  const segments = req.url.split('/');
  const camId    = segments[segments.length - 1];

  if (!isValidCamId(camId)) {
    ws.close(1008, 'Invalid camera ID');
    return;
  }

  log('info', 'WS client connected', { camId, ip: req.socket.remoteAddress });

  const intv = setInterval(() => {
    if (ws.readyState !== WebSocket.OPEN) return;
    const cam = cameras[camId];
    if (!cam) return;
    try {
      ws.send(JSON.stringify({
        frame:      cam.frame.toString('base64'),
        detections: cam.detections,
        ts:         Date.now()
      }));
    } catch (err) {
      log('error', 'WS send error', { camId, err: err.message });
    }
  }, 100);

  ws.on('close', () => {
    clearInterval(intv);
    log('info', 'WS client disconnected', { camId });
  });

  ws.on('error', (err) => {
    clearInterval(intv);
    log('error', 'WS error', { camId, err: err.message });
  });
});

// ─── FFmpeg recorder ─────────────────────────────────────────────────────────
function getOrStartRecorder(camId) {
  if (recorders[camId]) {
    const proc = recorders[camId];
    if (!proc.killed && proc.stdin && !proc.stdin.destroyed) return proc;
    // Process died – remove it so it gets recreated
    delete recorders[camId];
  }

  const camDir = path.join(REC_BASE, camId);
  fs.mkdirSync(camDir, { recursive: true });

  const segPattern = path.join(camDir, '%Y-%m-%d_%H-%M-%S.mp4');
  const proc = spawn('ffmpeg', [
    '-loglevel', 'error',
    '-f',        'mjpeg',
    '-i',        'pipe:0',
    '-c:v',      'libx264',
    '-preset',   'veryfast',
    '-crf',      '28',
    '-f',        'segment',
    '-segment_time', String(SEG_TIME),
    '-reset_timestamps', '1',
    '-strftime',  '1',
    segPattern
  ], { stdio: ['pipe', 'ignore', 'pipe'] });

  proc.on('error', (err) => log('error', 'ffmpeg spawn error', { camId, err: err.message }));
  proc.on('exit',  (code, signal) => {
    log('warn', 'ffmpeg exited', { camId, code, signal });
    delete recorders[camId];
  });
  proc.stderr.on('data', (d) => log('warn', 'ffmpeg stderr', { camId, msg: d.toString().trim() }));

  recorders[camId] = proc;
  log('info', 'Started recorder', { camId });
  return proc;
}

// ─── UDP ingest ───────────────────────────────────────────────────────────────
const udp = dgram.createSocket('udp4');

udp.on('error', (err) => {
  log('error', 'UDP socket error', { err: err.message });
});

udp.on('message', async (msg) => {
  // Guard: minimum message length and separator present
  if (!msg || msg.length < 3) return;

  const sep = msg.indexOf('|');
  if (sep <= 0 || sep >= 64) return; // camId too long or missing

  const camId = msg.slice(0, sep).toString('utf8');
  if (!isValidCamId(camId)) {
    log('warn', 'Dropped frame: invalid camera ID', { raw: camId.slice(0, 32) });
    return;
  }

  const frame = msg.slice(sep + 1);
  if (frame.length === 0 || frame.length > MAX_FRAME_BYTES) {
    log('warn', 'Dropped frame: size out of range', { camId, size: frame.length });
    return;
  }

  // Validate JPEG magic bytes (0xFF 0xD8)
  if (frame[0] !== 0xFF || frame[1] !== 0xD8) {
    log('warn', 'Dropped frame: not a valid JPEG', { camId });
    return;
  }

  // Run AI detection on a sampled subset of frames
  let detections = cameras[camId]?.detections || [];
  if (Math.random() < AI_SAMPLE_RATE) {
    try {
      const res = await axios.post(`${AI_URL}/detect`, frame, {
        headers:  { 'Content-Type': 'application/octet-stream' },
        timeout:  3000,
        maxContentLength: 256 * 1024
      });
      if (Array.isArray(res.data)) detections = res.data;
    } catch (err) {
      if (!err.code || err.code !== 'ECONNREFUSED') {
        log('warn', 'AI detection failed', { camId, err: err.message });
      }
    }
  }

  cameras[camId] = { frame, detections };

  // Write to FFmpeg recorder (non-blocking)
  try {
    const rec = getOrStartRecorder(camId);
    rec.stdin.write(frame, (writeErr) => {
      if (writeErr) log('warn', 'Recorder write error', { camId, err: writeErr.message });
    });
  } catch (err) {
    log('error', 'Recorder error', { camId, err: err.message });
  }
});

udp.bind(UDP_PORT, () => log('info', `UDP listening on port ${UDP_PORT}`));

// ─── MQTT ─────────────────────────────────────────────────────────────────────
const mqttOpts = {
  clientId:    `backend-${process.pid}`,
  clean:       true,
  reconnectPeriod: 5000,
  ...(MQTT_USER ? { username: MQTT_USER, password: MQTT_PASS } : {})
};
const mqttClient = mqtt.connect(MQTT_URL, mqttOpts);

mqttClient.on('connect',  () => log('info',  'MQTT connected'));
mqttClient.on('error',    (err) => log('error', 'MQTT error', { err: err.message }));
mqttClient.on('offline',  () => log('warn',  'MQTT offline'));

// ─── Graceful shutdown ────────────────────────────────────────────────────────
function shutdown(signal) {
  log('info', `Received ${signal} – shutting down`);

  // Stop all recorders gracefully
  for (const [camId, proc] of Object.entries(recorders)) {
    try {
      if (!proc.killed) {
        proc.stdin.end();
        log('info', 'Stopped recorder', { camId });
      }
    } catch (_) {}
  }

  server.close(() => {
    udp.close();
    mqttClient.end();
    process.exit(0);
  });

  // Force exit after 10 s if graceful close hangs
  setTimeout(() => process.exit(1), 10000).unref();
}

process.on('SIGTERM', () => shutdown('SIGTERM'));
process.on('SIGINT',  () => shutdown('SIGINT'));
process.on('uncaughtException',  (err) => log('error', 'Uncaught exception',  { err: err.message, stack: err.stack }));
process.on('unhandledRejection', (reason) => log('error', 'Unhandled rejection', { reason: String(reason) }));

// ─── Start ────────────────────────────────────────────────────────────────────
server.listen(PORT, () => log('info', `Backend listening on port ${PORT}`));
