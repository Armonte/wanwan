# Owner to-do: going live with account linking (saved 2026-10-20)

Do these when you have a few minutes. Full commands are in README.md in this folder.

1. **Create the Discord app.** https://discord.com/developers/applications -> New Application -> name it
   **PovertyCaster**. OAuth2 -> Redirects -> add exactly:
   `https://2dfm.org/community/discord/callback`
   Copy the **Client ID** and **Client Secret** (the secret goes only into the server's
   `community/data/community.env`, never into chat or git).
2. **Root keys** (generated in `D:\PC_ROOT_KEYS_MOVE_OFFLINE`): keep them on the PC until step 4 is done, then copy the
   two `*.private.pem` files to two offline places (USB + password manager) and delete them from the PC.
3. **On the server** (ssh alias `2dfm`): generate the day-to-day key and the KEYSET body:
   `python3 admin.py gen-ticket-key --gen 1`, then
   `python3 admin.py keyset-body --serial 1 --root a --ticket-key 1:<pubhex> --out keyset_1.body`.
4. **On the PC with the root keys:** `python3 sign_keyset.py keyset_1.body root_a.private.pem root_b.private.pem keyset_1.pckeys`,
   copy `keyset_1.pckeys` back, `python3 admin.py install-keyset keyset_1.pckeys`, start the service, check
   `https://2dfm.org/community/health` shows `"issuing": {"key_gen": 1}`.
5. **Stats site:** set `FM2K_STATS_ID_SECRET` on the server before deploying the new stats site (otherwise every
   player's public id changes on the next deploy).

Also pending from you: confirm Launch works from the test launcher, and the rematch screen after an offline versus match.
