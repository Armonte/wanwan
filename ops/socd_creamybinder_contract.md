# SOCD when creamybinder drives input

Settled with povertycaster and qoh-session, both of whom read the cb source
at their pinned commit. Written down because every trap here is silent and
surfaces on a crossup, which is the worst possible place to find it.

## The arrangement

FM2K resolves SOCD in the hook, AFTER the facing swap, so the modes operate
on FORWARD/BACK rather than LEFT/RIGHT -- "back beats forward" is what a
player means, and it stays correct when they cross up. The resolved mask is
pre-applied to the spectator and replay streams, so a re-simulating viewer
cannot diverge from the players over a mode difference.

creamybinder resolves in raw stick space, pre-facing, which is right for a
library with no concept of facing.

Both satisfy the rule worth remembering: **resolve before the wire and
placement can only change feel, never desync.**

So: cb reads devices, we keep the semantics.

    Config::socd = cb::Socd::None;   // load-bearing, see below

## Socd::None is genuinely untouched

`socd.cpp` is `case Socd::None: break;`, and `applySocd` has exactly one
call site, so there is no second resolution stage and nothing downstream
assumes it ran. Two non-obvious properties:

* **cb's pressed/released edges are derived from the POST-SOCD mask.** Under
  None they are raw, which is self-consistent -- but they describe
  PRE-resolution state while the game sees post-resolution input. Fine while
  we consume only the held mask. If anything of ours ever consumes cb's
  `pressed` (menu navigation, the overlay, bind capture), those edges will
  not match our resolved semantics.
* **LastWins memory updates even under None**, so switching modes at runtime
  later starts from a current winner rather than a stale one.

## Do NOT add an enum value

Both peers pushed back on `Socd::ResolvedDownstream`, and the argument is
one we supplied ourselves: our own modes 4 and 5 are byte-identical aliases
of 0 and 1, and we called those entries lies. A second enumerator with
identical behaviour is that same shape.

Two concrete traps if anyone adds one anyway:

* `config_io.cpp` range-checks with `s <= (int)Socd::None`, so **None being
  last is load-bearing**; appending after it silently widens the accepted
  range.
* the mode is serialized as its raw integer, so **inserting an enumerator
  renumbers every saved config in the field.**

Put the explanation at our construction site instead, where the reader who
would "helpfully" enable it is standing.

## The two double-resolution risks

**1. The config file puts it back.** `Session::loadConfigFrom` assigns
`im.socd` unconditionally from the parsed file, and `saveConfig` round-trips
the in-memory value. Any binder config written while socd was not None will
silently re-enable cb-side resolution on the next load -- and it reads as a
config problem, not a code one, weeks later.

Rule: pass None at construction, **re-force it after every
`loadConfigFrom`** (`Session::setSocd` exists for this), and make sure what
we persist is None so the round trip is stable. Log when a load changed it.

**2. A second Session with its own Config.** Adopting cb for the in-game
binder overlay means the injected DLL constructs its own Session. If that
site types its own Config, the two can drift, and the default is
`Socd::Neutral` -- so a Session built without an explicit socd RESOLVES.

Rule: one shared factory builds every Session's Config with socd pinned;
log the effective mode per session at startup so drift is visible in the
first line of any run; assert at `Hook_ApplySOCD` init that the session it
consumes reports None.

## Held-mask contract

* **The focus gate synthesizes releases.** Losing foreground releases all
  held keys, so the mask can legitimately snap to zero between two reads
  with no user action. Our SOCD must treat that as clean neutral, and
  anything edge-triggered downstream must tolerate up-edges it never saw
  down-edges for. Alt-tabbing mid-match WILL produce this.
* **Read once per sim frame into a snapshot and resolve from the snapshot.**
  Polling cadence is not the sim's cadence, and two reads straddling a poll
  can disagree. One read, one resolve, one value fed to the sim and the
  wire is also what makes the spectator/replay guarantee airtight: the
  stream must carry the SAME snapshot the sim consumed, not a second read.
* **A locked or headless console reads as all-neutral**, because
  GetForegroundWindow returns null and the gate treats that as unfocused.
  Correct for humans, surprising for an automated soak.

## Worth ten lines

A canary: push a synthetic L+R through the full path across a facing swap
and assert exactly one resolution was applied.
