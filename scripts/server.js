// server.js (hardened, with async delete + tmp_<uuid> guard)
// Minimal Fastify file server for Turbo/Spark over HTTP(S)
// - HEAD & GET support with Range
// - /.../index.json directory listing (Parquet-friendly)
// - Bearer auth, path jailing
// - NEW: DELETE /<path> asynchronously removes a folder
// - NEW: GET /tasks/:id to poll delete status
// - NEW: index.json & DELETE require a path segment "tmp_<uuid>"

const path = require('path');
const fs   = require('fs');
const mime = require('mime-types');
const crypto = require('crypto');
const fastifyFactory = require('fastify');

// ---------------- Config ----------------
const ROOT_DIR = process.argv[2]
  ? path.resolve(process.argv[2])
  : process.env.ROOT_DIR || path.join(__dirname, 'public');

const PORT = process.argv[3]
  ? parseInt(process.argv[3], 10)
  : parseInt(process.env.PORT || '10009', 10);

const BEARER_TOKEN = process.env.BEARER_TOKEN || 'my-secret-token';
const USE_HTTP = process.env.USE_HTTP === '1';

if (!fs.existsSync(ROOT_DIR) || !fs.statSync(ROOT_DIR).isDirectory()) {
  console.error(`Root folder "${ROOT_DIR}" does not exist or is not a directory.`);
  process.exit(1);
}

const keyPath  = path.join(__dirname, 'certs/key.pem');
const certPath = path.join(__dirname, 'certs/cert.pem');

const fastify = USE_HTTP
  ? fastifyFactory({ logger: { level: 'info' } })
  : fastifyFactory({
      logger: { level: 'info' },
      https: {
        key:  fs.readFileSync(keyPath),
        cert: fs.readFileSync(certPath)
      }
    });

console.log('Boot info:', {
  ROOT_DIR, PORT, USE_HTTP,
  cwd: process.cwd(), __dirname,
  keyPath: USE_HTTP ? '(HTTP mode)' : keyPath,
  certPath: USE_HTTP ? '(HTTP mode)' : certPath
});

// ---------------- Auth ----------------
fastify.addHook('onRequest', async (req, reply) => {
  if (req.url === '/health') return;
  const auth = req.headers['authorization'];
  if (auth !== `Bearer ${BEARER_TOKEN}`) {
    //return reply.code(401).send({ error: 'Unauthorized' });
  }
});

// ---------------- Health ----------------
fastify.get('/health', async () => ({ ok: true }));

// ---------------- Helpers ----------------
function safeResolve(relUrl) {
  const urlPath = decodeURIComponent((relUrl || '').split('?')[0].split('#')[0]);
  const normalized = path.posix.normalize(urlPath);
  const rel = normalized.startsWith('/') ? normalized : `/${normalized}`;
  const absRoot = path.resolve(ROOT_DIR);
  const absPath = path.resolve(path.join(absRoot, '.' + rel));
  if (!(absPath === absRoot || absPath.startsWith(absRoot + path.sep))) {
    return { error: 'Bad path', code: 400 };
  }
  return { absRoot, absPath, rel };
}

const UUID_RE = /^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[1-5][0-9a-fA-F]{3}-[89abAB][0-9a-fA-F]{3}-[0-9a-fA-F]{12}$/;

// e.g. /a/b/tmp_<uuid>/c  -> true (segment-based)
function hasTmpUuidSegment(relPath) {
  const parts = relPath.split('/').filter(Boolean);
  return parts.some(seg => {
    if (!seg.startsWith('tmp_')) return false;
    const maybe = seg.slice(4);
    return UUID_RE.test(maybe);
  });
}

async function listDirJSON(absPath) {
  const entries = await fs.promises.readdir(absPath, { withFileTypes: true });
  const listing = await Promise.all(entries.map(async d => {
    const p = path.join(absPath, d.name);
    const st = await fs.promises.stat(p);
    return {
      name: d.name,
      size: st.size,
      lastModified: Math.floor(st.mtimeMs),
      directory: d.isDirectory()
    };
  }));
  return listing;
}

function contentTypeFor(filePath) {
  return mime.lookup(filePath) || 'application/octet-stream';
}

// ---------------- Async delete task registry ----------------
const deleteTasks = new Map(); // taskId -> { status: 'pending'|'done'|'error', error?:string, target:string }

function newTaskId() {
  return crypto.randomBytes(12).toString('hex');
}

async function rmRecursive(absPath) {
  await fs.promises.rm(absPath, { recursive: true, force: true, maxRetries: 2, retryDelay: 50 });
}

// ---------------- File server with Range + HEAD ----------------
function serveFile(req, reply, absPath) {
  try {
    const st = fs.statSync(absPath);
    if (st.isDirectory()) {
      return reply.code(404).send({ error: 'Not Found (directory)' });
    }

    const total = st.size;
    const ctype = contentTypeFor(absPath);

    if (req.method === 'HEAD') {
      reply
        .header('Accept-Ranges', 'bytes')
        .header('Content-Type', ctype)
        .header('Content-Length', String(total))
        .header('Last-Modified', new Date(st.mtimeMs).toUTCString())
        .code(200);
      return reply.send();
    }

    const range = req.headers.range;
    if (range) {
      const m = /^bytes=(\d*)-(\d*)$/.exec(range);
      if (!m) {
        reply.header('Content-Range', `bytes */${total}`);
        return reply.code(416).send();
      }
      let start = m[1] ? parseInt(m[1], 10) : 0;
      let end   = m[2] ? parseInt(m[2], 10) : (total - 1);
      if (isNaN(start) || isNaN(end) || start > end || end >= total) {
        reply.header('Content-Range', `bytes */${total}`);
        return reply.code(416).send();
      }
      const chunkSize = (end - start) + 1;
      const stream = fs.createReadStream(absPath, { start, end, highWaterMark: 1 << 20 });

      reply
        .header('Accept-Ranges', 'bytes')
        .header('Content-Type', ctype)
        .header('Content-Length', String(chunkSize))
        .header('Content-Range', `bytes ${start}-${end}/${total}`)
        .header('Last-Modified', new Date(st.mtimeMs).toUTCString())
        .code(206);

      return reply.send(stream);
    }

    reply
      .header('Accept-Ranges', 'bytes')
      .header('Content-Type', ctype)
      .header('Content-Length', String(total))
      .header('Last-Modified', new Date(st.mtimeMs).toUTCString())
      .code(200);

    return reply.send(fs.createReadStream(absPath, { highWaterMark: 1 << 20 }));
  } catch (e) {
    if (e && e.code === 'ENOENT') return reply.code(404).send({ error: 'Not Found' });
    if (e && e.code === 'EISDIR') return reply.code(404).send({ error: 'Not Found (directory)' });
    if (e && e.code === 'EACCES') return reply.code(403).send({ error: 'Forbidden' });

    reply.request.log.error({ err: e, absPath }, 'serveFile failed');
    return reply.code(500).send({ error: 'Internal Server Error' });
  }
}

// ---------------- Routes ----------------

// Poll a delete task
fastify.get('/tasks/:id', async (req, reply) => {
  const t = deleteTasks.get(req.params.id);
  if (!t) return reply.code(404).send({ error: 'Task not found' });
  return reply.send(t);
});

// Directory listing (index.json) — allowed only if path has a tmp_<uuid> segment
fastify.route({
  method: ['GET', 'HEAD'],
  url: '/*',
  handler: async (req, reply) => {
    const { absPath, rel, error, code } = safeResolve(req.raw.url);
    if (error) return reply.code(code).send({ error });

    if (rel.includes('*')) {
      return reply.code(404).send({ error: 'Wildcard not directly fetchable' });
    }

    if (rel.endsWith('/index.json')) {
      if (!hasTmpUuidSegment(rel)) return reply.code(403).send({ error: 'Forbidden (listing restricted to tmp_<uuid>)' });

      const dirPath = absPath.slice(0, -'/index.json'.length);
      try {
        const st = fs.statSync(dirPath);
        if (!st.isDirectory()) return reply.code(404).send({ error: 'Directory not found' });

        if (req.method === 'HEAD') {
          reply.header('Content-Type', 'application/json').code(200);
          return reply.send();
        }

        const entries = await fs.promises.readdir(dirPath, { withFileTypes: true });
        const namesOnly = 'names' in (req.query || {});

        if (namesOnly) {
          const names = [];
          for (const d of entries) {
            if (d.isDirectory()) continue;
            if (d.name.startsWith('.')) continue;
            if (!d.name.toLowerCase().endsWith('.parquet')) continue;
            names.push(d.name);
          }
          reply.header('Content-Type', 'application/json');
          return reply.send(names);
        } else {
          const listing = await Promise.all(entries.map(async d => {
            const p = path.join(dirPath, d.name);
            const st2 = await fs.promises.stat(p);
            return {
              name: d.name,
              size: st2.size,
              lastModified: Math.floor(st2.mtimeMs),
              directory: d.isDirectory()
            };
          }));
          reply.header('Content-Type', 'application/json');
          return reply.send(listing);
        }
      } catch (e) {
        if (e.code === 'ENOENT') return reply.code(404).send({ error: 'Directory not found' });
        req.log.error({ err: e, dirPath }, 'index.json failed');
        return reply.code(500).send({ error: 'Internal Server Error' });
      }
    }

    // Else: serve a file (HEAD/GET)
    return serveFile(req, reply, absPath);
  }
});

// DELETE a folder — only if URL contains a tmp_<uuid> segment
fastify.delete('/*', async (req, reply) => {
  const { absPath, rel, error, code } = safeResolve(req.raw.url);
  if (error) return reply.code(code).send({ error });

  if (!hasTmpUuidSegment(rel)) {
    return reply.code(403).send({ error: 'Forbidden (delete restricted to tmp_<uuid>)' });
  }

  let st;
  try {
    st = await fs.promises.stat(absPath);
  } catch (e) {
    if (e.code === 'ENOENT') return reply.code(404).send({ error: 'Not Found' });
    throw e;
  }
  if (!st.isDirectory()) {
    return reply.code(400).send({ error: 'Bad Request (target is not a directory)' });
  }

  const taskId = newTaskId();
  deleteTasks.set(taskId, { status: 'pending', target: rel });

  // kick off async deletion
  setImmediate(async () => {
    try {
      await rmRecursive(absPath);
      deleteTasks.set(taskId, { status: 'done', target: rel });
    } catch (err) {
      deleteTasks.set(taskId, { status: 'error', error: String(err && err.message || err), target: rel });
    }
  });

  return reply.code(202).send({ taskId, status: 'pending' });
});

// ---------------- Start ----------------
(async () => {
  try {
    const address = await fastify.listen({ port: PORT, host: '0.0.0.0' });
    fastify.log.info(`🚀 ${USE_HTTP ? 'HTTP' : 'HTTPS'} server running at ${address}`);
    console.log(`BOUND: ${address}`);
  } catch (err) {
    fastify.log.error(err);
    console.error('fastify.listen failed:', err);
    process.exit(1);
  }
})();
