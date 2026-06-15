#!/usr/bin/env node
'use strict';
const http = require('http');
const fs   = require('fs');
const path = require('path');
const url  = require('url');
const crypto = require('crypto');

// ── Config ────────────────────────────────────────────────────────────────────
const ROOT_DIR  = path.resolve(process.argv[2] || '.');
const PORT      = parseInt(process.env.PORT || '8080', 10);
const WEBUI_DIR = path.resolve(__dirname, '..');
const USERNAME  = process.env.WUI_USR || 'admin';
const PASSWORD  = process.env.WUI_PWD || 'admin';
const NVS_FILE  = path.join(__dirname, 'nvs_mock.json');

// ── State ─────────────────────────────────────────────────────────────────────
const sessions = new Map();  // token → timestamp

let otaContext = null;

let nvs = {};
try { nvs = JSON.parse(fs.readFileSync(NVS_FILE, 'utf8')); } catch {}
if (!Object.keys(nvs).length) {
  nvs = {
    launcher: [
      { k: 'brightness', t: 'u8',  v: 128 },
      { k: 'rotation',   t: 'u8',  v: 1   },
      { k: 'wifi_ssid',  t: 'str', v: 'MyNetwork' },
      { k: 'wifi_pwd',   t: 'str', v: 'secret'    },
    ]
  };
  fs.writeFileSync(NVS_FILE, JSON.stringify(nvs, null, 2));
}
function saveNvs() { fs.writeFileSync(NVS_FILE, JSON.stringify(nvs, null, 2)); }

// ── Helpers ───────────────────────────────────────────────────────────────────
function humanReadableSize(bytes) {
  if (bytes < 1024)             return `${bytes} B`;
  if (bytes < 1024 * 1024)      return `${(bytes / 1024).toFixed(2)} kB`;
  if (bytes < 1024 * 1024 * 1024) return `${(bytes / 1024 / 1024).toFixed(2)} MB`;
  return `${(bytes / 1024 / 1024 / 1024).toFixed(2)} GB`;
}

function generateToken() {
  return crypto.randomBytes(18).toString('base64url').slice(0, 24);
}

function parseCookies(req) {
  const out = {};
  (req.headers.cookie || '').split(';').forEach(p => {
    const [k, ...v] = p.trim().split('=');
    if (k) out[k.trim()] = v.join('=').trim();
  });
  return out;
}

function isAuthenticated(req) {
  const token = parseCookies(req)['ESP32SESSION'];
  return !!(token && sessions.has(token));
}

function readBody(req) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    req.on('data', c => chunks.push(c));
    req.on('end',  () => resolve(Buffer.concat(chunks)));
    req.on('error', reject);
  });
}

function clearOtaContext() {
  otaContext = null;
}

// Safe path resolution — never escape ROOT_DIR
function toAbsPath(relPath) {
  const abs = path.normalize(path.join(ROOT_DIR, relPath));
  if (abs !== ROOT_DIR && !abs.startsWith(ROOT_DIR + path.sep)) return null;
  return abs;
}

// ── Binary multipart parser ───────────────────────────────────────────────────
function bufferIndexOf(buf, needle, offset = 0) {
  for (let i = offset; i <= buf.length - needle.length; i++) {
    let match = true;
    for (let j = 0; j < needle.length; j++) {
      if (buf[i + j] !== needle[j]) { match = false; break; }
    }
    if (match) return i;
  }
  return -1;
}

function parseMultipart(body, boundary) {
  // Returns { fieldName: string|Buffer, ... }
  // Binary files → Buffer; text fields → string
  const result   = {};
  const CRLF     = Buffer.from('\r\n');
  const DBLCRLF  = Buffer.from('\r\n\r\n');
  const delim    = Buffer.from('--' + boundary);
  const endDelim = Buffer.from('--' + boundary + '--');

  let pos = 0;
  while (pos < body.length) {
    const start = bufferIndexOf(body, delim, pos);
    if (start === -1) break;
    pos = start + delim.length;

    // End of multipart
    if (body.slice(pos, pos + 2).toString() === '--') break;
    // Skip CRLF after boundary
    if (body.slice(pos, pos + 2).toString() === '\r\n') pos += 2;

    // Find end of headers
    const headerEnd = bufferIndexOf(body, DBLCRLF, pos);
    if (headerEnd === -1) break;
    const headers = body.slice(pos, headerEnd).toString();
    pos = headerEnd + 4;

    // Find next boundary to know content extent
    const nextBoundary = bufferIndexOf(body, Buffer.from('\r\n--' + boundary), pos);
    const contentEnd   = nextBoundary === -1 ? body.length : nextBoundary;
    const content      = body.slice(pos, contentEnd);
    pos = contentEnd;

    // Parse name and filename from Content-Disposition
    const dispLine = headers.split('\r\n').find(h => /content-disposition/i.test(h)) || '';
    const nameM    = dispLine.match(/\bname="([^"]+)"/i);
    const fileM    = dispLine.match(/\bfilename="([^"]*)"/i);
    if (!nameM) continue;
    const name     = nameM[1];
    const filename = fileM ? fileM[1] : null;

    result[name] = filename !== null
      ? { filename, data: content }
      : content.toString();
  }
  return result;
}

function parseParams(body, contentType) {
  if ((contentType || '').includes('multipart/form-data')) {
    const bm = contentType.match(/boundary=([^\s;]+)/);
    if (!bm) return {};
    const parsed = parseMultipart(body, bm[1]);
    const params = {};
    for (const [key, value] of Object.entries(parsed)) {
      params[key] = typeof value === 'string' ? value : value.toString();
    }
    return params;
  }

  const params = {};
  new URLSearchParams(body.toString()).forEach((v, k) => { params[k] = v; });
  return params;
}

function normalizeOtaPart(part, fileSize) {
  if (!part || typeof part !== 'object') throw new Error('Invalid manifest part');

  const normalized = {
    kind: typeof part.kind === 'string' ? part.kind : '',
    label: typeof part.label === 'string' ? part.label : '',
    subtype: Number(part.subtype),
    sourceOffset: Number(part.sourceOffset),
    copySize: Number(part.copySize),
    declaredSize: Number(part.declaredSize ?? part.copySize),
  };

  if (!['app', 'data'].includes(normalized.kind)) throw new Error('Invalid manifest part kind');
  if (!Number.isInteger(normalized.subtype) || normalized.subtype < 0 || normalized.subtype > 0xFF) {
    throw new Error('Invalid manifest subtype');
  }
  if (!Number.isInteger(normalized.sourceOffset) || normalized.sourceOffset < 0) {
    throw new Error('Invalid manifest sourceOffset');
  }
  if (!Number.isInteger(normalized.copySize) || normalized.copySize <= 0) {
    throw new Error('Invalid manifest copySize');
  }
  if (!Number.isInteger(normalized.declaredSize) || normalized.declaredSize <= 0) {
    throw new Error('Invalid manifest declaredSize');
  }
  if (normalized.sourceOffset > fileSize || normalized.copySize > fileSize - normalized.sourceOffset) {
    throw new Error('Manifest range exceeds file');
  }

  return normalized;
}

function prepareOtaContext(params) {
  const fileSize = Number(params.size);
  if (!Number.isInteger(fileSize) || fileSize <= 0) throw new Error('Invalid OTA size');

  if (!params.manifest) {
    clearOtaContext();
    return { active: false, legacy: true, fileSize, command: Number(params.command) || 0 };
  }

  let manifest;
  try {
    manifest = JSON.parse(params.manifest);
  } catch {
    throw new Error('Bad manifest');
  }

  if (!manifest || !Array.isArray(manifest.parts) || manifest.parts.length === 0) {
    throw new Error('Missing parts');
  }

  const parts = manifest.parts.map((part) => normalizeOtaPart(part, fileSize));
  const appParts = parts.filter((part) => part.kind === 'app');
  if (appParts.length !== 1) throw new Error('Missing app part');

  parts.sort((a, b) => a.sourceOffset - b.sourceOffset);
  otaContext = {
    active: true,
    sourceName: typeof manifest.sourceName === 'string' ? manifest.sourceName : '',
    fileSize,
    parts,
    totalCopySize: parts.reduce((sum, part) => sum + part.copySize, 0),
    totalWritten: 0,
    receivedFileSize: 0,
  };
  return otaContext;
}

function logPreparedOtaContext(ctx) {
  if (!ctx.active) return;
  console.log(`[OTA] prepared source=${ctx.sourceName || 'unknown'} fileSize=${ctx.fileSize} totalCopySize=${ctx.totalCopySize}`);
  for (const part of ctx.parts) {
    const label = part.label ? ` label=${part.label}` : '';
    console.log(
      `[OTA] part kind=${part.kind} subtype=0x${part.subtype.toString(16)} offset=0x${part.sourceOffset.toString(16)} copy=0x${part.copySize.toString(16)} declared=0x${part.declaredSize.toString(16)}${label}`
    );
  }
}

// ── Directory listing (matches listFiles() in webInterface.cpp) ───────────────
function listFilesResponse(folder) {
  const normFolder = folder.replace(/\\/g, '/').replace(/\/+$/, '') || '/';
  const abs = toAbsPath(normFolder === '/' ? '' : normFolder);
  let text = `pa:${normFolder}:0\n`;
  if (!abs) return text;
  try {
    const entries = fs.readdirSync(abs, { withFileTypes: true });
    for (const e of entries) {
      if (e.isDirectory()) {
        text += `Fo:${e.name}:0\n`;
      } else {
        const size = fs.statSync(path.join(abs, e.name)).size;
        text += `Fi:${e.name}:${humanReadableSize(size)}\n`;
      }
    }
  } catch (err) {
    console.error(`listfiles error: ${err.message}`);
  }
  return text;
}

// ── HTTP server ───────────────────────────────────────────────────────────────
const server = http.createServer(async (req, res) => {
  const { pathname, query: queryStr } = url.parse(req.url || '/', false);
  const query  = Object.fromEntries(new URLSearchParams(queryStr || ''));
  const method = req.method || 'GET';

  function send(status, type, body) {
    const buf = Buffer.isBuffer(body) ? body : Buffer.from(body ?? '');
    res.writeHead(status, {
      'Content-Type': type,
      'Content-Length': buf.length,
      'Access-Control-Allow-Origin': '*',
    });
    res.end(buf);
  }
  const ok        = (body)     => send(200, 'text/plain', body);
  const sendJson  = (body)     => send(200, 'application/json', body);
  const err       = (code, m)  => send(code, 'text/plain', m);
  const redirect  = (loc, code = 302) => {
    res.writeHead(code, { 'Location': loc, 'Access-Control-Allow-Origin': '*' });
    res.end();
  };
  function serveFile(filePath, contentType) {
    try {
      send(200, contentType, fs.readFileSync(filePath));
    } catch {
      err(404, 'Not found');
    }
  }

  if (method === 'OPTIONS') {
    res.writeHead(204, {
      'Access-Control-Allow-Origin': '*',
      'Access-Control-Allow-Methods': 'GET,POST',
      'Access-Control-Allow-Headers': 'Content-Type,Cookie',
    });
    return res.end();
  }

  // ── Public endpoints ────────────────────────────────────────────────────────

  // GET /ping
  if (pathname === '/ping' && method === 'GET') return ok('launcher-pong');

  // GET /scripts.js
  if (pathname === '/scripts.js') return serveFile(path.join(WEBUI_DIR, 'scripts.js'), 'application/javascript');

  // GET /style.css
  if (pathname === '/style.css') return serveFile(path.join(WEBUI_DIR, 'style.css'), 'text/css');

  // GET /logged-out
  if (pathname === '/logged-out') return serveFile(path.join(WEBUI_DIR, 'logout.html'), 'text/html');

  // GET /systeminfo  (no auth required — scripts.js calls it on page load)
  if (pathname === '/systeminfo' && method === 'GET') {
    const body = JSON.stringify({
      VERSION: '1.0.0-mock',
      SD: { free: '8.00 GB', used: '2.00 GB', total: '10.00 GB' }
    });
    return sendJson(body);
  }

  // GET / — serve login page or main UI
  if (pathname === '/' && method === 'GET') {
    if (isAuthenticated(req)) {
      return serveFile(path.join(WEBUI_DIR, 'index.html'), 'text/html');
    }
    return serveFile(path.join(WEBUI_DIR, 'login.html'), 'text/html');
  }

  // POST /login
  if (pathname === '/login' && method === 'POST') {
    const body = await readBody(req);
    const ct   = req.headers['content-type'] || '';
    let params = {};
    if (ct.includes('multipart/form-data')) {
      const bm = ct.match(/boundary=([^\s;]+)/);
      if (bm) { const p = parseMultipart(body, bm[1]); for (const k in p) params[k] = typeof p[k] === 'string' ? p[k] : p[k].toString(); }
    } else {
      new URLSearchParams(body.toString()).forEach((v, k) => { params[k] = v; });
    }
    if (params['username'] === USERNAME && params['password'] === PASSWORD) {
      const token = generateToken();
      sessions.clear();
      sessions.set(token, Date.now());
      res.writeHead(302, {
        'Location': '/',
        'Set-Cookie': `ESP32SESSION=${token}; Path=/; HttpOnly`,
        'Access-Control-Allow-Origin': '*',
      });
      return res.end();
    }
    return redirect('/?failed');
  }

  // GET /logout
  if (pathname === '/logout') {
    sessions.delete(parseCookies(req)['ESP32SESSION']);
    res.writeHead(302, {
      'Location': '/?loggedout',
      'Set-Cookie': 'ESP32SESSION=0; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT',
      'Access-Control-Allow-Origin': '*',
    });
    return res.end();
  }

  // ── Auth wall ────────────────────────────────────────────────────────────────
  if (!isAuthenticated(req)) return err(401, 'Unauthorized');

  // GET /reboot
  if (pathname === '/reboot') {
    console.log('[REBOOT] simulated');
    return ok('Rebooting (simulated)');
  }

  if (pathname === '/OTA' && method === 'GET' && Object.prototype.hasOwnProperty.call(query, 'update')) {
    clearOtaContext();
    console.log('[OTA] update mode enabled (simulated)');
    return ok('Update');
  }

  // GET /listfiles?folder=X
  if (pathname === '/listfiles' && method === 'GET') {
    return ok(listFilesResponse(query.folder || '/'));
  }

  // GET /file?name=X&action=download|delete|create
  if (pathname === '/file' && method === 'GET') {
    const { name, action } = query;
    if (!name || !action) return err(400, 'ERROR: name and action params required');
    const abs = toAbsPath(name);
    if (!abs) return err(400, 'ERROR: invalid path');

    if (action === 'download') {
      try {
        const content = fs.readFileSync(abs);
        res.writeHead(200, {
          'Content-Type': 'application/octet-stream',
          'Content-Length': content.length,
          'Content-Disposition': `attachment; filename="${path.basename(abs)}"`,
          'Access-Control-Allow-Origin': '*',
        });
        return res.end(content);
      } catch { return err(404, 'File not found'); }
    }
    if (action === 'delete') {
      try { fs.rmSync(abs, { recursive: true, force: true }); return ok(`Deleted : ${name}`); }
      catch { return ok(`FAIL deleting: ${name}`); }
    }
    if (action === 'create') {
      try { fs.mkdirSync(abs, { recursive: true }); return ok(`Created new folder: ${name}`); }
      catch { return ok(`FAIL creating folder: ${name}`); }
    }
    return err(400, 'ERROR: invalid action param supplied');
  }

  // GET /editfile?name=X   POST /editfile?name=X
  if (pathname === '/editfile') {
    const { name } = query;
    if (!name) return err(400, 'Missing name');
    const abs = toAbsPath(name);
    if (!abs) return err(400, 'Invalid path');
    if (method === 'GET') {
      try { return send(200, 'text/plain', fs.readFileSync(abs)); }
      catch { return err(404, 'Not found'); }
    }
    if (method === 'POST') {
      const body = await readBody(req);
      try { fs.writeFileSync(abs, body); return ok('OK'); }
      catch { return ok('FAIL'); }
    }
  }

  // POST / — file upload
  if (pathname === '/' && method === 'POST') {
    const body = await readBody(req);
    const ct   = req.headers['content-type'] || '';
    const bm   = ct.match(/boundary=([^\s;]+)/);
    if (bm) {
      const parts = parseMultipart(body, bm[1]);
      // folder comes from the "folder" field in the XHR FormData
      const folder = typeof parts['folder'] === 'string' ? parts['folder'] : '/';
      for (const [k, v] of Object.entries(parts)) {
        if (typeof v === 'object' && v.data && v.filename) {
          // webkitRelativePath may contain subdirs in the filename
          const relName = v.filename.replace(/\\/g, '/');
          const dest = toAbsPath(path.posix.join(folder === '/' ? '' : folder, relName));
          if (!dest) continue;
          fs.mkdirSync(path.dirname(dest), { recursive: true });
          fs.writeFileSync(dest, v.data);
          console.log(`[UPLOAD] ${dest} (${humanReadableSize(v.data.length)})`);
        }
      }
    }
    return ok('OK');
  }

  // GET/POST /nvs
  if (pathname === '/nvs') {
    if (method === 'GET') return sendJson(JSON.stringify(nvs));
    if (method === 'POST') {
      const body = await readBody(req);
      try {
        const update = JSON.parse(body.toString());
        for (const [ns, fields] of Object.entries(update)) {
          if (!nvs[ns]) nvs[ns] = [];
          for (const field of (fields || [])) {
            if (ns === 'launcher' && field.k === 'token') continue;  // never expose
            const existing = nvs[ns].find(f => f.k === field.k);
            if (existing) Object.assign(existing, field);
            else nvs[ns].push(field);
          }
        }
        saveNvs();
        return ok('OK');
      } catch { return err(400, 'Bad JSON'); }
    }
  }

  // POST /rename
  if (pathname === '/rename' && method === 'POST') {
    const body = await readBody(req);
    const ct   = req.headers['content-type'] || '';
    let params = {};
    if (ct.includes('multipart/form-data')) {
      const bm = ct.match(/boundary=([^\s;]+)/);
      if (bm) { const p = parseMultipart(body, bm[1]); for (const k in p) params[k] = typeof p[k] === 'string' ? p[k] : p[k].toString(); }
    } else {
      new URLSearchParams(body.toString()).forEach((v, k) => { params[k] = v; });
    }
    const { filePath, fileName } = params;
    if (!filePath || !fileName) return err(400, 'Missing fileName or filePath');
    const absOld = toAbsPath(filePath);
    if (!absOld) return err(400, 'Invalid path');
    const absNew = path.join(path.dirname(absOld), fileName);
    try { fs.renameSync(absOld, absNew); return ok(`${filePath} renamed to ${fileName}`); }
    catch { return ok('Fail renaming file.'); }
  }

  // POST /UPDATE (SD update simulation)
  if (pathname === '/UPDATE' && method === 'POST') {
    const body = await readBody(req);
    const ct   = req.headers['content-type'] || '';
    let fileName = '';
    if (ct.includes('multipart/form-data')) {
      const bm = ct.match(/boundary=([^\s;]+)/);
      if (bm) { const p = parseMultipart(body, bm[1]); fileName = typeof p['fileName'] === 'string' ? p['fileName'] : ''; }
    }
    if (fileName) { console.log(`[UPDATE] SD update simulated for: ${fileName}`); return ok('Starting Update'); }
    return err(400, 'Missing fileName');
  }

  // POST /OTA
  if (pathname === '/OTA' && method === 'POST') {
    const body = await readBody(req);
    const params = parseParams(body, req.headers['content-type'] || '');
    if (!Object.prototype.hasOwnProperty.call(params, 'command')) return err(400, 'Invalid OTA request');

    try {
      const ctx = prepareOtaContext(params);
      if (ctx.active) logPreparedOtaContext(ctx);
      else console.log(`[OTA] legacy request received command=${ctx.command} size=${ctx.fileSize} (simulated)`);
      return ok('OK');
    } catch (error) {
      clearOtaContext();
      return err(400, error.message || 'Install prep failed');
    }
  }

  // POST /OTAFILE
  if (pathname === '/OTAFILE' && method === 'POST') {
    const body = await readBody(req);
    const ct = req.headers['content-type'] || '';
    const bm = ct.match(/boundary=([^\s;]+)/);
    if (!bm) return err(400, 'Missing multipart boundary');

    const parts = parseMultipart(body, bm[1]);
    const upload = parts['file1'];
    if (!upload || typeof upload !== 'object' || !upload.data) return ok('No file');

    const fileBuffer = upload.data;
    if (otaContext && otaContext.active) {
      if (fileBuffer.length !== otaContext.fileSize) {
        clearOtaContext();
        return err(400, 'Uploaded file size does not match prepared OTA size');
      }

      for (const part of otaContext.parts) {
        const chunk = fileBuffer.subarray(part.sourceOffset, part.sourceOffset + part.copySize);
        if (chunk.length !== part.copySize) {
          clearOtaContext();
          return err(400, 'Manifest range exceeds uploaded file');
        }
        otaContext.totalWritten += chunk.length;
        console.log(
          `[OTAFILE] wrote kind=${part.kind} subtype=0x${part.subtype.toString(16)} offset=0x${part.sourceOffset.toString(16)} bytes=${chunk.length}` +
          (part.label ? ` label=${part.label}` : '')
        );
      }

      const completed = otaContext.totalWritten === otaContext.totalCopySize;
      console.log(
        `[OTAFILE] completed source=${otaContext.sourceName || upload.filename || 'unknown'} totalWritten=${otaContext.totalWritten}/${otaContext.totalCopySize}`
      );
      clearOtaContext();
      return completed ? ok('OK') : err(400, 'Incomplete OTA write');
    }

    console.log(`[OTAFILE] legacy upload received bytes=${fileBuffer.length} (simulated)`);
    return ok('OK');
  }

  // GET /sdpins
  if (pathname === '/sdpins' && method === 'GET') {
    return ok('Functionality exclusive for Headless environment');
  }

  // GET /wifi
  if (pathname === '/wifi' && method === 'GET') {
    const { usr, pwd, ssid } = query;
    if (usr && pwd) {
      console.log(`[WIFI] credentials updated  user=${usr}`);
      return ok(`User: ${usr} configured with password: ${pwd}`);
    }
    if (ssid && pwd) {
      console.log(`[WIFI] ssid=${ssid}`);
    }
    return ok('OK');
  }

  // Fallback
  redirect('/');
});

server.listen(PORT, () => {
  console.log('\n╔══════════════════════════════════════╗');
  console.log('║   Launcher WebUI — Dev Server        ║');
  console.log('╚══════════════════════════════════════╝');
  console.log(`  URL  : http://localhost:${PORT}`);
  console.log(`  Root : ${ROOT_DIR}`);
  console.log(`  Login: ${USERNAME} / ${PASSWORD}`);
  console.log('  Ctrl+C to stop\n');
});
