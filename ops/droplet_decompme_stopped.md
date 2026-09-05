# decomp.me stopped on the 2dfm droplet -- 2026-08-28

Owner authorised stopping the decomp.me stack to reclaim memory. Done, and
this is how to put it back.

## Why

The droplet is a 1 vCPU / 1.9 GiB box running the hub, the Rust relay, the
stats site, the log receiver, the Discord bot, weyvelength, dzr2 and Caddy.
Swap was at **1.8 GiB of 2.0 GiB (90%)**, dominated by decomp.me's Next.js
**dev** frontend. Sustained swap pressure on the box that also runs live
matches is worth removing on its own, and it is a candidate explanation for
the recurring `hub DOWN` -> `hub recovered` blips five minutes apart that
`health_check.sh` has been logging every day or two.

## What was done

    docker stop decompme-nginx-1 decompme-frontend-1 \
                decompme-backend-1 decompme-postgres-1

Nothing was removed, rebuilt or reconfigured. Images, volumes and
`/opt/decompme` are untouched, so the database is intact.

`cobalt` and `cobold` were deliberately LEFT RUNNING -- they are a separate
service that happens to share the docker host, not part of decomp.me.

## Result

    swap   1.8 GiB used -> 248 MiB used      (~1.6 GiB reclaimed)
    avail  929 MiB      -> 1.0 GiB
    hub    ok before and after, 473 tokens, uptime unbroken

## To undo

    ssh 2dfm-root 'docker start decompme-postgres-1 decompme-backend-1 \
                                decompme-frontend-1 decompme-nginx-1'

Original restart policies, recorded before stopping, in case they ever need
restoring:

    decompme-frontend-1  no
    decompme-backend-1   unless-stopped
    decompme-postgres-1  unless-stopped
    decompme-nginx-1     unless-stopped

Three of the four are `unless-stopped`, which means docker REMEMBERS the
stopped state -- they will not come back on a daemon restart or a reboot.
That is why a plain `docker stop` was enough and no policy edit was needed;
it also means `docker start` is the whole undo.

## Consequence

`decomp.2dfm.org` now returns 502 through Caddy. The Caddy config was not
touched, so restoring the containers restores the site with no proxy change.
