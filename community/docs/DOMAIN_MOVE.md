# Moving the community service and stats from 2dfm.org to povertycaster.org

Written 2026-10-21, NOT run. The owner registers povertycaster.org later; these are the steps, in order.

What already supports the move:

- Signed calls (ticket refresh, profile visibility, reports) verify against a LIST of hosts:
  `COMMUNITY_PUBLIC_HOST=2dfm.org,povertycaster.org` (`config.py` `public_hosts`). A launcher signs the host of its
  own `community.url`, so old and new launchers both work while both hosts are listed.
- `GET /v1/config` tells launchers where the stats site, replays and the web root live
  (`COMMUNITY_STATS_URL`, `COMMUNITY_REPLAY_PUBLIC_BASE`, `COMMUNITY_WEB_BASE`). The launcher reads it, so changing
  those env values moves every launcher at once, with no release.
- The Discord sign-in uses the launcher's own `http://127.0.0.1/callback`: no domain involved, nothing to change in
  the Discord portal.
- The only domain literal in launcher code is the default community URL (pc-core `Link.hpp` `kDefaultCommunityUrl`)
  plus the stats default constant, both used only when nothing better is known.

## Steps

1. **DNS.** Point `povertycaster.org` and `www.povertycaster.org` (A, and AAAA if the droplet has IPv6) at the
   droplet (`ssh 2dfm`, 67.205.183.131). If replays get a custom R2 domain, add it in the Cloudflare dashboard
   (R2 bucket > Settings > Custom domain, e.g. `replays.povertycaster.org`).
2. **Caddy.** Add the new names to the apex site block so every `handle_path` (community, stats, Updates) answers on
   both: change `2dfm.org, www.2dfm.org {` to `2dfm.org, www.2dfm.org, povertycaster.org, www.povertycaster.org {`.
   Back up first (`cp Caddyfile Caddyfile.bak.domain-$(date +%s)`), `caddy validate`, `systemctl reload caddy`.
   Caddy gets the certificate on the first request; check `curl -I https://povertycaster.org/community/health`.
3. **Community service.** In `community/data/community.env`:
   `COMMUNITY_PUBLIC_HOST=2dfm.org,povertycaster.org`, and point `COMMUNITY_STATS_URL`, `COMMUNITY_WEB_BASE`
   (and `COMMUNITY_REPLAY_PUBLIC_BASE` if replays moved) at povertycaster.org. `systemctl restart pc-community`;
   check `/health` and `/v1/config` on both hosts.
4. **Stats site.** If it is public by then, set any absolute-URL setting to the new host; its pages use relative
   links. A `stats.povertycaster.org` subdomain, if wanted, is one more Caddy site block like `stats.2dfm.org`.
5. **Launcher release.** Change `kDefaultCommunityUrl` (and the stats default) to povertycaster.org, land through
   `tools/land.sh`, ship through the updater. Launchers with an explicit `community.url` in their ini keep it.
6. **Both-host window.** Keep `2dfm.org` in `COMMUNITY_PUBLIC_HOST` and in the Caddy block until old launchers have
   updated. Do not redirect `2dfm.org/community` to the new host: a signed call names its host, so a redirected call
   would fail verification; serve both instead.
7. **Drop the old host** (optional, much later): remove `2dfm.org` from `COMMUNITY_PUBLIC_HOST`. A launcher still on
   the old URL then gets `bad_call_signature` on refresh and must update. Keep serving `2dfm.org/Updates/` for the
   updater regardless.

## Replays and stored URLs

Store replay object keys, not absolute URLs, and compose `replay_url` from `COMMUNITY_REPLAY_PUBLIC_BASE` when
answering or exporting, so a domain change never rewrites stored rows.
