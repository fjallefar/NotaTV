// NotaTV anonymous usage stats (STATUS 121).
//
// POST /ping        body {"v":"1.0.26"}  -> counts one ping for (today, version)
// GET  /            dashboard (HTTP Basic auth, password = secret DASH_KEY)
// GET  /api/stats   JSON for the dashboard (same auth)
//
// Nothing about the box is stored: no IP, no id. The IP is only held in
// memory for a few pings per isolate as a light flood guard.

import DASHBOARD from "./dashboard.html";

const VERSION_RE = /^\d{1,3}\.\d{1,3}\.\d{1,4}(-[a-z0-9]{1,12})?$/;
const PER_IP_PER_DAY = 5;
const seen = new Map(); // "day|ip" -> count, lives only as long as the isolate

const today = () => new Date().toISOString().slice(0, 10);

function floodGuard(ip) {
  const day = today();
  if (seen.size > 5000) seen.clear();
  const key = day + "|" + ip;
  const n = (seen.get(key) || 0) + 1;
  seen.set(key, n);
  return n <= PER_IP_PER_DAY;
}

async function ping(req, env) {
  const len = Number(req.headers.get("content-length") || 0);
  if (len > 200) return new Response("too large", { status: 413 });
  let v;
  try {
    v = (await req.json()).v;
  } catch {
    return new Response("bad json", { status: 400 });
  }
  if (typeof v !== "string" || !VERSION_RE.test(v))
    return new Response("bad version", { status: 400 });
  if (!floodGuard(req.headers.get("cf-connecting-ip") || "?"))
    return new Response(null, { status: 204 });
  await env.DB.prepare(
    "INSERT INTO pings (day, version, n) VALUES (?1, ?2, 1) " +
      "ON CONFLICT (day, version) DO UPDATE SET n = n + 1"
  )
    .bind(today(), v)
    .run();
  return new Response(null, { status: 204 });
}

async function sha(s) {
  return new Uint8Array(
    await crypto.subtle.digest("SHA-256", new TextEncoder().encode(s))
  );
}

async function authorized(req, env) {
  if (!env.DASH_KEY) return false;
  const h = req.headers.get("authorization") || "";
  if (!h.startsWith("Basic ")) return false;
  let pass = "";
  try {
    pass = atob(h.slice(6)).split(":").slice(1).join(":");
  } catch {
    return false;
  }
  const a = await sha(pass), b = await sha(env.DASH_KEY);
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a[i] ^ b[i];
  return diff === 0;
}

async function stats(env, days) {
  const since = new Date(Date.now() - (days - 1) * 86400000)
    .toISOString()
    .slice(0, 10);
  const { results } = await env.DB.prepare(
    "SELECT day, version, n FROM pings WHERE day >= ?1 ORDER BY day"
  )
    .bind(since)
    .all();
  return { since, today: today(), rows: results };
}

const security = {
  "x-content-type-options": "nosniff",
  "referrer-policy": "no-referrer",
  "cache-control": "no-store",
};

export default {
  async fetch(req, env) {
    const url = new URL(req.url);

    if (url.pathname === "/ping") {
      if (req.method !== "POST") return new Response(null, { status: 405 });
      return ping(req, env);
    }

    if (url.pathname === "/" || url.pathname === "/api/stats") {
      if (!(await authorized(req, env)))
        return new Response("Login kræves", {
          status: 401,
          headers: { "www-authenticate": 'Basic realm="NotaTV stats"' },
        });
      if (url.pathname === "/")
        return new Response(DASHBOARD, {
          headers: {
            ...security,
            "content-type": "text/html; charset=utf-8",
            "content-security-policy":
              "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'",
          },
        });
      const days = Math.min(365, Math.max(7, Number(url.searchParams.get("days")) || 90));
      return Response.json(await stats(env, days), { headers: security });
    }

    return new Response("not found", { status: 404 });
  },
};
