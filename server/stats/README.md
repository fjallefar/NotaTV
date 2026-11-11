# notatv-stats

Anonymous daily usage ping for NotaTV boxes (STATUS 121), running as a Cloudflare Worker with a D1 database.

- `POST /ping` with `{"v":"1.0.26"}` adds one to the count for (UTC day, version). Nothing else is stored: no IP, no box id.
- `GET /` is a dashboard (HTTP Basic auth: any username, password = secret `DASH_KEY`).
- `GET /api/stats?days=90` is the JSON behind the dashboard (same auth).

## Deploy (first time)

```
cd server/stats
npx wrangler d1 execute notatv-stats --remote --file schema.sql
npx wrangler deploy
npx wrangler secret put DASH_KEY
```

## Local test

```
echo 'DASH_KEY=test123' > .dev.vars
npx wrangler d1 execute notatv-stats --local --file schema.sql
npx wrangler dev
curl -X POST localhost:8787/ping -d '{"v":"1.0.26"}'
```

## Raw numbers

```
npx wrangler d1 execute notatv-stats --remote --command "SELECT * FROM pings ORDER BY day DESC LIMIT 50"
```
