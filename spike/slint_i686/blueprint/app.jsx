// app.jsx — full 640x480 navigable Fightcade-style client (NERV vocabulary).
// Single artboard, internally tab-navigated. Reuses NERV primitives + motion.jsx.

const { useState: aS, useEffect: aE, useRef: aR, useMemo: aM, useCallback: aCB } = React;

// ═════════════════════════════════════════════════════════════
// HELPERS
// ═════════════════════════════════════════════════════════════

// Tiny per-user ping oscillator (32x8 canvas)
function PingWave({ ping, w = 42, h = 12 }) {
  const ref = aR(null);
  const t = useTime();
  aE(() => {
    const c = ref.current; if (!c) return;
    const ctx = c.getContext("2d");
    ctx.clearRect(0, 0, w, h);
    // ping → frequency, jitter
    const amp = Math.min(h * 0.4, 0.6 + ping * 0.04);
    const freq = 0.6 + Math.min(0.8, ping * 0.02);
    const jit = Math.min(0.6, ping * 0.012);
    const col = ping < 30 ? "#5fff95" : ping < 80 ? "#ffb938" : "#ff5a3c";
    ctx.strokeStyle = col;
    ctx.lineWidth = 1;
    ctx.beginPath();
    for (let x = 0; x < w; x++) {
      const phase = t * freq * 4 + x * 0.5;
      const j = (Math.sin(phase * 7 + ping) * jit);
      const y = h / 2 + Math.sin(phase) * amp + j;
      x === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
    }
    ctx.stroke();
  });
  return <canvas ref={ref} width={w} height={h} style={{ display: "block" }} />;
}

// Bars indicator (DEPRECATED — replaced by PingWave + StatusGlyph)
function _PingBars_unused({ ping }) {
  const lvl = ping < 25 ? 4 : ping < 50 ? 3 : ping < 100 ? 2 : 1;
  const col = lvl >= 3 ? "#5fff95" : lvl === 2 ? "#ffb938" : "#ff5a3c";
  return (
    <div style={{ display: "flex", gap: 1, alignItems: "flex-end", height: 8 }}>
      {[2, 4, 6, 8].map((h, i) => (
        <div key={i} style={{ width: 2, height: h, background: i < lvl ? col : "rgba(255,255,255,0.12)" }} />
      ))}
    </div>
  );
}

// Country / region "flag" — colored 2-stripe rectangle so we don't hand-draw real flags
function FlagDot({ region }) {
  const map = {
    JP: ["#ffffff", "#ff3030"], US: ["#0040a0", "#ff3030"], FR: ["#0040a0", "#ff3030"],
    DE: ["#000000", "#ffb938"], KR: ["#ffffff", "#0040a0"], TW: ["#ffffff", "#ff3030"],
    BR: ["#0d8a3a", "#ffd000"], MX: ["#0d8a3a", "#ff3030"], ES: ["#ffd000", "#ff3030"],
    UK: ["#0040a0", "#ff3030"], CA: ["#ffffff", "#ff3030"], AR: ["#7ec0ee", "#ffffff"],
  };
  const [a, b] = map[region] || ["#3d3d3d", "#888"];
  return (
    <div style={{ width: 12, height: 8, border: "1px solid rgba(0,0,0,0.6)", display: "flex", flexDirection: "column", flexShrink: 0 }}>
      <div style={{ flex: 1, background: a }} />
      <div style={{ flex: 1, background: b }} />
    </div>
  );
}

// ═════════════════════════════════════════════════════════════
// LEFT RAIL — recent games + nav icons + avatar
// ═════════════════════════════════════════════════════════════
function LeftRail({ games, currentGame, onPickGame, onSettings, onMap, view, setView }) {
  return (
    <div style={{
      width: 48, borderRight: "1px solid var(--nv-line)",
      display: "flex", flexDirection: "column", alignItems: "center",
      padding: "6px 0", gap: 4, background: "rgba(8,4,14,0.6)"
    }}>
      {/* logo dot */}
      <div style={{ position: "relative", width: 32, height: 32, marginBottom: 4 }}>
        <PulseRing size={32} color="var(--nv-orange)" period={2400} />
        <div style={{ position: "absolute", inset: 0, display: "flex", alignItems: "center", justifyContent: "center", color: "var(--nv-orange)", fontSize: 10, fontFamily: "var(--nv-display)", fontWeight: 700 }}>F</div>
      </div>
      <hr className="nv-hr" style={{ width: "60%" }} />
      {/* recent games */}
      {games.map(g => (
        <div key={g.id} onClick={() => onPickGame(g.id)} title={g.n}
          style={{
            width: 36, height: 27, border: `1px solid ${g.id === currentGame ? "var(--nv-orange)" : "var(--nv-line)"}`,
            background: `${g.tint}`, position: "relative", cursor: "pointer",
            display: "flex", alignItems: "center", justifyContent: "center",
            fontFamily: "var(--nv-mono)", fontSize: 7, color: "#fff",
            letterSpacing: "0.05em", textAlign: "center", lineHeight: 1.0,
            textShadow: "0 1px 2px rgba(0,0,0,0.7)",
          }}>
          {g.code}
          {g.id === currentGame && <div style={{ position: "absolute", left: -2, top: 4, bottom: 4, width: 2, background: "var(--nv-orange)" }} />}
        </div>
      ))}
      <hr className="nv-hr" style={{ width: "60%" }} />
      <RailIcon glyph="◎" active={view === "browse"}   onClick={() => setView("browse")}   label="browse" />
      <RailIcon glyph="✦" active={view === "rankings"} onClick={() => setView("rankings")} label="rank" />
      <RailIcon glyph="▣" active={view === "events"}   onClick={() => setView("events")}   label="evts" />
      <div style={{ flex: 1 }} />
      <RailIcon glyph="◫" active={view === "map"}      onClick={onMap}                     label="ctrl" />
      <RailIcon glyph="⚙" active={view === "cfg"}      onClick={onSettings}                label="cfg" />
      {/* avatar */}
      <div style={{
        width: 32, height: 32, border: "1px solid var(--nv-orange)", marginTop: 4,
        background: "linear-gradient(135deg, #5a2a85, #ff5a3c)", position: "relative",
        display: "flex", alignItems: "center", justifyContent: "center", fontFamily: "var(--nv-display)", color: "#fff", fontSize: 13, fontWeight: 700,
      }}>
        S<div style={{ position: "absolute", right: -2, bottom: -2, width: 8, height: 8, background: "var(--nv-phos)", border: "1px solid #000", borderRadius: "50%" }} />
      </div>
    </div>
  );
}
function RailIcon({ glyph, active, onClick, label }) {
  return (
    <div onClick={onClick} style={{
      width: 32, height: 24, display: "flex", flexDirection: "column", alignItems: "center", justifyContent: "center",
      cursor: "pointer", color: active ? "var(--nv-orange)" : "var(--nv-faint)",
      borderLeft: active ? "2px solid var(--nv-orange)" : "2px solid transparent",
      lineHeight: 1.0,
    }}>
      <div style={{ fontSize: 14 }}>{glyph}</div>
      <div className="nv-data" style={{ fontSize: 7, letterSpacing: "0.1em", color: active ? "var(--nv-orange)" : "var(--nv-faint)" }}>{label}</div>
    </div>
  );
}

// ═════════════════════════════════════════════════════════════
// TOP BAR — game title + tabs + actions
// ═════════════════════════════════════════════════════════════
function TopBar({ game, view, setView, onTest, onTrain, onBoot }) {
  const tabs = [
    { id: "lobby",    k: "LOBBY" },
    { id: "replays",  k: "REPLAYS" },
    { id: "rankings", k: "RANK" },
    { id: "stats",    k: "STATS" },
    { id: "events",   k: "EVTS" },
    { id: "profile",  k: "PROFILE" },
  ];
  return (
    <div style={{ height: 30, borderBottom: "1px solid var(--nv-line)", display: "flex", alignItems: "center", padding: "0 8px", gap: 6, background: "rgba(0,0,0,0.4)" }}>
      <div className="nv-eyebrow nv-acc" style={{ fontSize: 8, whiteSpace: "nowrap" }}>{game.code}</div>
      <div className="nv-data" style={{ fontSize: 10, color: "var(--nv-ink)", whiteSpace: "nowrap", overflow: "hidden", textOverflow: "ellipsis", maxWidth: 110 }}>{game.short}</div>
      <div style={{ width: 1, height: 14, background: "var(--nv-line)" }} />
      <div style={{ display: "flex", gap: 2 }}>
        {tabs.map(t => (
          <div key={t.id} onClick={() => setView(t.id)} style={{
            padding: "3px 6px", fontFamily: "var(--nv-mono)", fontSize: 9, letterSpacing: "0.1em", cursor: "pointer",
            color: view === t.id ? "var(--nv-orange)" : "var(--nv-ink2)",
            borderBottom: view === t.id ? "2px solid var(--nv-orange)" : "2px solid transparent",
            marginBottom: -1,
          }}>{t.k}</div>
        ))}
      </div>
      <div style={{ flex: 1 }} />
      <BarBtn onClick={onTest}>◐ TEST</BarBtn>
      <BarBtn onClick={onTrain}>▤ TRAIN</BarBtn>
      <BarBtn onClick={onBoot} accent>▶ BOOT</BarBtn>
    </div>
  );
}
function BarBtn({ children, onClick, accent }) {
  return (
    <div onClick={onClick} style={{
      padding: "2px 6px", fontFamily: "var(--nv-mono)", fontSize: 9, letterSpacing: "0.08em", cursor: "pointer",
      border: `1px solid ${accent ? "var(--nv-orange)" : "var(--nv-line)"}`,
      color: accent ? "var(--nv-orange)" : "var(--nv-ink2)",
      background: accent ? "rgba(255,90,60,0.08)" : "transparent",
    }}>{children}</div>
  );
}

// ═════════════════════════════════════════════════════════════
// RIGHT RAIL — LOOKING TO PLAY
// ═════════════════════════════════════════════════════════════
function RightRail({ users, activeMatches, onPickUser }) {
  const [menu, setMenu] = aS(null);   // {user, top}
  const [hoverM, setHoverM] = aS(null); // {match, top}
  const railRef = aR(null);

  const openMenu = (u, e) => {
    const rect = e.currentTarget.getBoundingClientRect();
    setMenu({ user: u, top: rect.top, left: rect.left });
  };
  const showMatchHover = (m, e) => {
    const rect = e.currentTarget.getBoundingClientRect();
    setHoverM({ match: m, top: rect.top, left: rect.left });
  };

  return (
    <div ref={railRef} style={{ width: 154, position: "relative", borderLeft: "1px solid var(--nv-line)", display: "flex", flexDirection: "column", background: "rgba(8,4,14,0.4)" }}>
      {/* IN MATCH header */}
      <div style={{ padding: "4px 8px", borderBottom: "1px solid var(--nv-line)", background: "rgba(255,90,60,0.06)", flexShrink: 0 }}>
        <div style={{ display: "flex", alignItems: "baseline", justifyContent: "space-between" }}>
          <div className="nv-eyebrow nv-acc" style={{ fontSize: 8 }}>◈ IN MATCH</div>
          <div className="nv-num" style={{ fontSize: 10, color: "var(--nv-orange)" }}>{activeMatches.length}</div>
        </div>
      </div>
      <div style={{ borderBottom: "1px solid var(--nv-line)", padding: "2px 0", maxHeight: 90, overflowY: "auto", flexShrink: 0 }} className="nv-scroll">
        {activeMatches.map((m, i) => (
          <ActiveMatchRow key={i} match={m}
            onEnter={(e) => showMatchHover(m, e)}
            onLeave={() => setHoverM(null)} />
        ))}
      </div>
      {/* LOOKING TO PLAY */}
      <div style={{ padding: "4px 8px", borderBottom: "1px solid var(--nv-line)", display: "flex", alignItems: "baseline", justifyContent: "space-between", flexShrink: 0 }}>
        <div className="nv-eyebrow" style={{ fontSize: 8 }}>LOOKING TO PLAY</div>
        <div className="nv-num" style={{ fontSize: 11, color: "var(--nv-orange)" }}>{users.length}</div>
      </div>
      <div style={{ flex: 1, overflowY: "auto", overflowX: "hidden", minHeight: 0 }} className="nv-scroll">
        {users.map((u) => (
          <UserRow key={u.name} u={u}
            onClick={(e) => openMenu(u, e)}
            isOpen={menu && menu.user.name === u.name} />
        ))}
      </div>
      <div style={{ borderTop: "1px solid var(--nv-line)", padding: "4px 8px", display: "flex", justifyContent: "space-between", fontFamily: "var(--nv-mono)", fontSize: 8, flexShrink: 0 }}>
        <span style={{ color: "var(--nv-faint)" }}>FILTER ▾</span>
        <span style={{ color: "var(--nv-acc)" }}>+ JOIN QUEUE</span>
      </div>

      {/* RAIL-LEVEL menu (fixed, escapes any clip / stacking issue) */}
      {menu && (
        <UserMiniMenu user={menu.user} top={menu.top} left={menu.left}
          onPick={(k) => { onPickUser(k, menu.user); setMenu(null); }}
          onClose={() => setMenu(null)} />
      )}
      {hoverM && <MatchHoverCard match={hoverM.match} top={hoverM.top} left={hoverM.left} />}
    </div>
  );
}
function StatusGlyph({ status }) {
  const map = {
    idle:   { c: "var(--nv-phos)",   g: "●" },
    queued: { c: "var(--nv-amber)",  g: "◆" },
    match:  { c: "var(--nv-acc)",    g: "◈" },
    afk:    { c: "var(--nv-faint)",  g: "○" },
  };
  const s = map[status] || map.idle;
  return <span style={{ color: s.c, fontSize: 9, fontFamily: "var(--nv-mono)" }}>{s.g}</span>;
}
function UserMiniMenu({ user, top, left, onPick, onClose }) {
  const items = [
    { k: "challenge", glyph: "▶", label: "CHALLENGE",   c: "var(--nv-orange)" },
    { k: "spectate",  glyph: "◍", label: "SPECTATE",    c: "var(--nv-cyan)" },
    { k: "dm",        glyph: "✎", label: "DIRECT MSG",  c: "var(--nv-ink)" },
    { k: "profile",   glyph: "▣", label: "PROFILE",     c: "var(--nv-ink2)" },
    { k: "ignore",    glyph: "⊘", label: "IGNORE USER", c: "var(--nv-magenta)" },
  ];
  const W = 138, H = 110;
  const fixedLeft = Math.max(4, (left ?? 480) - W - 4);
  const fixedTop  = Math.max(4, Math.min(top ?? 100, window.innerHeight - H - 4));
  return ReactDOM.createPortal(
    <>
      <div onClick={onClose} style={{ position: "fixed", inset: 0, zIndex: 99000 }} />
      <div style={{
        position: "fixed", left: fixedLeft, top: fixedTop, zIndex: 99001, width: W,
        background: "rgba(10,6,18,0.98)", border: "1px solid var(--nv-orange)",
        boxShadow: "0 0 16px rgba(255,90,60,0.3)", fontFamily: "var(--nv-mono)",
      }}>
        <div style={{ padding: "3px 6px", borderBottom: "1px solid var(--nv-line)", background: "rgba(255,90,60,0.08)" }}>
          <div className="nv-eyebrow nv-acc" style={{ fontSize: 7 }}>{user.name}</div>
          <div className="nv-data" style={{ fontSize: 7, color: "var(--nv-dim)" }}>{user.region} · {user.ping}ms · {user.status || (user.in === "match" ? "in match" : "idle")}</div>
        </div>
        {items.map(it => (
          <div key={it.k} onClick={() => onPick(it.k)} style={{
            display: "grid", gridTemplateColumns: "14px 1fr", gap: 4, padding: "3px 6px",
            fontSize: 9, cursor: "pointer", color: it.c,
            borderBottom: "1px dotted rgba(255,255,255,0.04)",
          }}
            onMouseEnter={e => e.currentTarget.style.background = "rgba(255,90,60,0.1)"}
            onMouseLeave={e => e.currentTarget.style.background = "transparent"}>
            <span>{it.glyph}</span><span>{it.label}</span>
          </div>
        ))}
      </div>
    </>,
    document.body
  );
}
function UserRow({ u, onClick, isOpen }) {
  const [hov, setHov] = aS(false);
  const status = u.in === "match" ? "match" : (u.status || "idle");
  return (
    <div onMouseEnter={() => setHov(true)} onMouseLeave={() => setHov(false)} onClick={onClick}
      style={{
        display: "grid", gridTemplateColumns: "12px 1fr auto auto", gap: 4, alignItems: "center",
        padding: "3px 8px", cursor: "pointer",
        background: hov || isOpen ? "rgba(255,90,60,0.08)" : "transparent",
        borderLeft: hov || isOpen ? "2px solid var(--nv-orange)" : "2px solid transparent",
        fontFamily: "var(--nv-mono)", fontSize: 10,
      }}>
      <FlagDot region={u.region} />
      <span style={{ color: status === "match" ? "var(--nv-faint)" : "var(--nv-ink)", overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap" }}>{u.name}</span>
      <PingWave ping={u.ping} />
      <StatusGlyph status={status} />
    </div>
  );
}
function ActiveMatchRow({ match, onEnter, onLeave }) {
  const m = match;
  // colored char chip (initials)
  const chipA = m.charA?.slice(0,2).toUpperCase() || "??";
  const chipB = m.charB?.slice(0,2).toUpperCase() || "??";
  return (
    <div onMouseEnter={onEnter} onMouseLeave={onLeave}
      style={{ display: "grid", gridTemplateColumns: "1fr auto 1fr", gap: 3, alignItems: "center", padding: "2px 6px", fontFamily: "var(--nv-mono)", fontSize: 9, cursor: "default" }}>
      <div style={{ display: "flex", alignItems: "center", gap: 3, justifyContent: "flex-end", minWidth: 0 }}>
        <span style={{ color: m.lead === "A" ? "var(--nv-orange)" : "var(--nv-ink2)", overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap", fontSize: 8 }}>{m.a}</span>
        <span style={{ width: 14, height: 8, background: m.colorA || "#5a2a85", border: "1px solid rgba(0,0,0,0.6)", color: "#fff", fontSize: 6, display: "flex", alignItems: "center", justifyContent: "center", flexShrink: 0, letterSpacing: "0.05em" }}>{chipA}</span>
      </div>
      <div style={{ display: "flex", flexDirection: "column", alignItems: "center", lineHeight: 1.0 }}>
        <span style={{ color: "var(--nv-orange)", fontSize: 9, fontFamily: "var(--nv-display)", letterSpacing: "0.05em" }}>{m.scoreA}-{m.scoreB}</span>
        <span style={{ color: "var(--nv-faint)", fontSize: 6, letterSpacing: "0.1em" }}>{m.format}</span>
      </div>
      <div style={{ display: "flex", alignItems: "center", gap: 3, minWidth: 0 }}>
        <span style={{ width: 14, height: 8, background: m.colorB || "#0040a0", border: "1px solid rgba(0,0,0,0.6)", color: "#fff", fontSize: 6, display: "flex", alignItems: "center", justifyContent: "center", flexShrink: 0, letterSpacing: "0.05em" }}>{chipB}</span>
        <span style={{ color: m.lead === "B" ? "var(--nv-orange)" : "var(--nv-ink2)", overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap", fontSize: 8 }}>{m.b}</span>
      </div>
    </div>
  );
}
function MatchHoverCard({ match, top, left }) {
  const m = match;
  const W = 178, H = 178;
  const fixedLeft = Math.max(4, (left ?? 480) - W - 4);
  const fixedTop  = Math.max(4, Math.min(top ?? 100, window.innerHeight - H - 4));
  return ReactDOM.createPortal(
    <div style={{
      position: "fixed", left: fixedLeft, top: fixedTop, zIndex: 99002, width: W,
      background: "rgba(10,6,18,0.98)", border: "1px solid var(--nv-cyan)",
      boxShadow: "0 0 16px rgba(74,218,204,0.25)", padding: "5px 7px",
      pointerEvents: "none", fontFamily: "var(--nv-mono)",
    }}>
      <div className="nv-eyebrow" style={{ color: "var(--nv-cyan)", fontSize: 7 }}>◈ LIVE MATCH · {m.format}</div>
      <hr className="nv-hr" style={{ margin: "3px 0" }} />
      {/* head-to-head */}
      <div style={{ display: "grid", gridTemplateColumns: "1fr auto 1fr", gap: 4, alignItems: "center", marginBottom: 3 }}>
        <div style={{ textAlign: "right" }}>
          <div className="nv-data" style={{ fontSize: 9, color: m.lead === "A" ? "var(--nv-orange)" : "var(--nv-ink)" }}>{m.a}</div>
          <div style={{ display: "inline-block", padding: "0 3px", marginTop: 1, background: m.colorA, color: "#fff", fontFamily: "var(--nv-mono)", fontSize: 7, letterSpacing: "0.08em" }}>{m.charA}</div>
        </div>
        <div className="nv-display" style={{ fontSize: 16, color: "var(--nv-orange)", textAlign: "center", lineHeight: 1.0 }}>
          {m.scoreA}<span style={{ color: "var(--nv-faint)", fontSize: 11 }}>·</span>{m.scoreB}
        </div>
        <div>
          <div className="nv-data" style={{ fontSize: 9, color: m.lead === "B" ? "var(--nv-orange)" : "var(--nv-ink)" }}>{m.b}</div>
          <div style={{ display: "inline-block", padding: "0 3px", marginTop: 1, background: m.colorB, color: "#fff", fontFamily: "var(--nv-mono)", fontSize: 7, letterSpacing: "0.08em" }}>{m.charB}</div>
        </div>
      </div>
      {/* round dots */}
      <div style={{ display: "flex", justifyContent: "center", gap: 3, marginTop: 2 }}>
        {Array.from({ length: m.firstTo * 2 - 1 }).map((_, i) => {
          const won = i < m.scoreA;
          const wonB = i >= m.firstTo * 2 - 1 - m.scoreB;
          const filled = won || wonB;
          const c = won ? "var(--nv-orange)" : wonB ? "var(--nv-cyan)" : "rgba(255,255,255,0.15)";
          return <div key={i} style={{ width: 6, height: 6, background: filled ? c : "transparent", border: `1px solid ${filled ? c : "rgba(255,255,255,0.2)"}` }} />;
        })}
      </div>
      <hr className="nv-hr" style={{ margin: "4px 0" }} />
      {/* stats grid */}
      <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: 4, fontFamily: "var(--nv-mono)", fontSize: 8 }}>
        <Mini k="ROUND"  v={`${m.round}/${m.firstTo*2-1}`} />
        <Mini k="TIME"   v={m.time} />
        <Mini k="H2H"    v={m.h2h} />
        <Mini k="ROOM"   v={m.room} />
        <Mini k="PING"   v={`${m.ping}ms`} />
        <Mini k="DELAY"  v={`${m.delay}F`} />
      </div>
      <hr className="nv-hr" style={{ margin: "4px 0" }} />
      <div style={{ display: "flex", justifyContent: "space-between", fontFamily: "var(--nv-mono)", fontSize: 7 }}>
        <span style={{ color: "var(--nv-acc)" }}>◍ SPECTATE</span>
        <span style={{ color: "var(--nv-faint)" }}>{m.spectators} watching</span>
      </div>
    </div>,
    document.body
  );
}
function Mini({ k, v }) {
  return (
    <div style={{ display: "flex", justifyContent: "space-between", borderBottom: "1px dotted rgba(255,255,255,0.06)", padding: "1px 0" }}>
      <span style={{ color: "var(--nv-faint)" }}>{k}</span>
      <span style={{ color: "var(--nv-ink)" }}>{v}</span>
    </div>
  );
}

// ═════════════════════════════════════════════════════════════
// BROWSE TAB — game grid (popular / categories / hidden / events)
// ═════════════════════════════════════════════════════════════
function BrowseView({ onPickGame }) {
  const [search, setSearch] = aS("");
  const popular = [
    { id: "kof98", code: "KOF98",  n: "King of Fighters '98",      players: 429, tint: "linear-gradient(135deg,#5a2a85 30%, #ff3030)" },
    { id: "3s",    code: "SF3-3S", n: "Street Fighter III · 3rd",  players: 332, tint: "linear-gradient(135deg,#0040a0 30%, #4adacc)" },
    { id: "kof02", code: "KOF02",  n: "King of Fighters 2002",     players: 298, tint: "linear-gradient(135deg,#000 30%, #ff5a3c)" },
    { id: "k02p",  code: "KOF02P", n: "KOF2002 · Plus",            players: 268, tint: "linear-gradient(135deg,#3d1c5e 30%, #ffb938)" },
    { id: "ce",    code: "SF2CE",  n: "Street Fighter II · CE",    players: 157, tint: "linear-gradient(135deg,#501800 30%, #ff8a3c)" },
    { id: "garou", code: "GAROU",  n: "Garou · Mark of the Wolves", players: 142, tint: "linear-gradient(135deg,#0a0a0a 30%, #ff3030)" },
    { id: "lb2",   code: "LB2",    n: "Last Blade 2",              players: 128, tint: "linear-gradient(135deg,#1a1a3a 30%, #4adacc)" },
    { id: "rb2",   code: "RB2",    n: "Real Bout Fatal Fury 2",    players: 119, tint: "linear-gradient(135deg,#400020 30%, #ffb938)" },
    { id: "sst",   code: "SAMSP-T",n: "Samurai Showdown · Tenka",  players: 104, tint: "linear-gradient(135deg,#2a1a0a 30%, #ff8a3c)" },
    { id: "alp3",  code: "SFA3",   n: "Street Fighter Alpha 3",    players: 96,  tint: "linear-gradient(135deg,#0a3a5a 30%, #ff3030)" },
  ];
  const cats = [
    { k: "POPULAR",   c: "var(--nv-orange)" },
    { k: "FAVORITES", c: "var(--nv-amber)" },
    { k: "FIGHTER",   c: "var(--nv-magenta)" },
    { k: "NAOMI",     c: "var(--nv-cyan)" },
    { k: "SHOOTER",   c: "var(--nv-phos)" },
  ];
  const recent = [
    { id: "mvc2",  code: "MVC2",   n: "Marvel vs Capcom 2",        players: 88,  tint: "linear-gradient(135deg,#1a0040 30%, #4adacc)" },
    { id: "vsav",  code: "VSAV",   n: "Vampire Savior",            players: 72,  tint: "linear-gradient(135deg,#2a002a 30%, #ff5a3c)" },
    { id: "jojo",  code: "JOJO",   n: "JoJo's Bizarre Adventure",  players: 64,  tint: "linear-gradient(135deg,#003a3a 30%, #ffb938)" },
    { id: "p4u",   code: "P4U",    n: "Persona 4 Arena",           players: 58,  tint: "linear-gradient(135deg,#000020 30%, #ffe038)" },
    { id: "bbcs",  code: "BBCS",   n: "BlazBlue · Continuum Shift",players: 51,  tint: "linear-gradient(135deg,#1a002a 30%, #5fffe0)" },
  ];
  const hidden = [
    { id: "mc",   code: "MART-CH", n: "Martial Champion",     players: 0,  tint: "linear-gradient(135deg,#1a1a1a 30%, #5a3a1a)" },
    { id: "vot",  code: "V-ON",    n: "Cyber Troopers · V-ON",players: 2,  tint: "linear-gradient(135deg,#0a0a3a 30%, #4adacc)" },
    { id: "doa2", code: "DOA2",    n: "Dead or Alive 2",      players: 1,  tint: "linear-gradient(135deg,#1a0a1a 30%, #ff5a3c)" },
    { id: "ns",   code: "N-SLASH", n: "Night Slashers",       players: 2,  tint: "linear-gradient(135deg,#2a0010 30%, #ffb938)" },
    { id: "cf",   code: "CLIFF",   n: "Cliffhanger · E. Randy",players: 1, tint: "linear-gradient(135deg,#3a2a00 30%, #ff8a3c)" },
    { id: "fhuf", code: "F-HUF",   n: "Fighting Hufflepuff",  players: 0,  tint: "linear-gradient(135deg,#0a0a0a 30%, #5fff95)" },
    { id: "tk2",  code: "TANGO",   n: "Tango Brothers · ARC", players: 1,  tint: "linear-gradient(135deg,#1a0a2a 30%, #ff5a3c)" },
    { id: "wfm",  code: "W-FORM",  n: "Waveform · 95",        players: 0,  tint: "linear-gradient(135deg,#001a2a 30%, #4adacc)" },
    { id: "wmm",  code: "WONMOM",  n: "Wonder Mom · DX",      players: 1,  tint: "linear-gradient(135deg,#2a002a 30%, #ffe038)" },
    { id: "kkb",  code: "KKB",     n: "Kikai Banchou",        players: 0,  tint: "linear-gradient(135deg,#0a1a0a 30%, #ff8a3c)" },
  ];
  return (
    <div style={{ padding: "6px 8px", height: "100%", display: "flex", flexDirection: "column", gap: 6, overflow: "hidden" }}>
      {/* search — sticky */}
      <div style={{ display: "flex", alignItems: "center", gap: 6, padding: "3px 8px", border: "1px solid var(--nv-line)", flexShrink: 0 }}>
        <span className="nv-acc" style={{ fontSize: 10 }}>›</span>
        <input value={search} onChange={e => setSearch(e.target.value)} placeholder="search games · titles · roms..." style={{ flex: 1, background: "transparent", border: "none", outline: "none", color: "var(--nv-ink)", fontFamily: "var(--nv-mono)", fontSize: 10 }} />
        <span className="nv-data" style={{ fontSize: 8, color: "var(--nv-faint)" }}>FILTERS ▾</span>
      </div>
      {/* scroll body */}
      <div className="nv-scroll" style={{ flex: 1, minHeight: 0, overflowY: "auto", display: "flex", flexDirection: "column", gap: 8, paddingRight: 4 }}>
        {/* popular — bigger spotlight tile + grid */}
        <div>
          <SectionHead k="POPULAR · NOW" extra="↻ live · 10 of 47" />
          <div style={{ display: "grid", gridTemplateColumns: "repeat(5, 80px)", gap: 4, justifyContent: "start" }}>
            {popular.map((g, i) => (
              <FadeIn key={g.id} delay={i * 30} dur={300}>
                <GameTile g={g} onClick={() => onPickGame(g.id)} />
              </FadeIn>
            ))}
          </div>
        </div>
        {/* categories */}
        <div>
          <SectionHead k="BROWSE BY CATEGORY" />
          <div style={{ display: "grid", gridTemplateColumns: "repeat(5, 80px)", gap: 4, justifyContent: "start" }}>
            {cats.map((c, i) => (
              <FadeIn key={c.k} delay={i * 40} dur={300}>
                <div style={{ height: 36, border: `1px solid ${c.c}`, background: `linear-gradient(90deg, ${c.c}11, transparent 60%)`, padding: 4, display: "flex", flexDirection: "column", justifyContent: "space-between", cursor: "pointer" }}>
                  <div className="nv-data" style={{ fontSize: 8, color: c.c }}>›</div>
                  <div className="nv-display" style={{ fontSize: 10, color: "var(--nv-ink)", letterSpacing: "0.05em" }}>{c.k}</div>
                </div>
              </FadeIn>
            ))}
          </div>
        </div>
        {/* recent plays */}
        <div>
          <SectionHead k="RECENT · YOU" extra="last 14d" />
          <div style={{ display: "grid", gridTemplateColumns: "repeat(5, 80px)", gap: 4, justifyContent: "start" }}>
            {recent.map((g, i) => (
              <FadeIn key={g.id} delay={i * 30} dur={300}>
                <GameTile g={g} onClick={() => onPickGame(g.id)} />
              </FadeIn>
            ))}
          </div>
        </div>
        {/* hidden gems — full 10 */}
        <div>
          <SectionHead k="HIDDEN GEMS" extra="seed 0xA01F" />
          <div style={{ display: "grid", gridTemplateColumns: "repeat(5, 80px)", gap: 4, justifyContent: "start" }}>
            {hidden.map((g, i) => (
              <FadeIn key={g.id} delay={i * 25} dur={300}>
                <GameTile g={g} dim onClick={() => onPickGame(g.id)} />
              </FadeIn>
            ))}
          </div>
        </div>
        {/* upcoming events strip */}
        <div>
          <SectionHead k="UPCOMING · 14D" />
          <div style={{ display: "grid", gridTemplateColumns: "repeat(4, 1fr)", gap: 4 }}>
            {[
              ["SOFA #504", "5/8 · WEU", "var(--nv-magenta)"],
              ["SOFA #505", "5/9 · NA",  "var(--nv-magenta)"],
              ["DTK #80",   "5/9 · WLD", "var(--nv-amber)"],
              ["MAX MO #2", "5/9 · NA+EU","var(--nv-orange)"],
            ].map(([t, sub, c], i) => (
              <FadeIn key={t} delay={i * 40} dur={300}>
                <div style={{ height: 28, border: `1px solid ${c}`, background: `linear-gradient(135deg, ${c}1a 0%, transparent 70%)`, padding: "2px 4px", display: "flex", flexDirection: "column", justifyContent: "space-between" }}>
                  <div className="nv-display" style={{ fontSize: 9, color: "var(--nv-ink)", letterSpacing: "0.04em" }}>{t}</div>
                  <div className="nv-data" style={{ fontSize: 7, color: c }}>{sub}</div>
                </div>
              </FadeIn>
            ))}
          </div>
        </div>
        {/* footer marker */}
        <div className="nv-data" style={{ fontSize: 7, color: "var(--nv-faint)", textAlign: "center", padding: "4px 0 2px", letterSpacing: "0.15em", flexShrink: 0 }}>
          ━━ END · 30 TITLES SHOWN · 217 MORE IN INDEX ━━
        </div>
      </div>
    </div>
  );
}
function SectionHead({ k, extra }) {
  return (
    <div style={{ display: "flex", alignItems: "baseline", justifyContent: "space-between", margin: "0 0 3px" }}>
      <div className="nv-eyebrow nv-acc" style={{ fontSize: 8 }}>{k}</div>
      {extra && <div className="nv-data" style={{ fontSize: 7, color: "var(--nv-faint)" }}>{extra}</div>}
    </div>
  );
}
function GameTile({ g, dim, onClick }) {
  return (
    <div onClick={onClick} style={{
      width: 80, height: 60, position: "relative", border: "1px solid var(--nv-line)", cursor: "pointer", overflow: "hidden",
      background: g.tint || "linear-gradient(135deg,#1d0d2e,#3d1c5e)",
      opacity: dim ? 0.7 : 1,
    }}>
      <div style={{ position: "absolute", inset: 0, background: "radial-gradient(ellipse at 70% 30%, rgba(255,255,255,0.15), transparent 60%)" }} />
      <div style={{ position: "absolute", top: 2, left: 2, padding: "1px 3px", background: "rgba(0,0,0,0.7)", border: "1px solid rgba(255,255,255,0.2)", fontFamily: "var(--nv-mono)", fontSize: 7, color: "var(--nv-ink2)", letterSpacing: "0.05em" }}>
        ◉ {g.players}
      </div>
      <div style={{ position: "absolute", left: 0, right: 0, bottom: 0, padding: "2px 4px", background: "linear-gradient(0deg, rgba(0,0,0,0.85), transparent)", display: "flex", flexDirection: "column", gap: 0 }}>
        <div className="nv-display" style={{ fontSize: 8, color: "#fff", letterSpacing: "0.02em", whiteSpace: "nowrap", overflow: "hidden", textOverflow: "ellipsis" }}>{g.n}</div>
        <div className="nv-data" style={{ fontSize: 6, color: "var(--nv-orange)", letterSpacing: "0.1em" }}>ARCADE · FC2</div>
      </div>
    </div>
  );
}

// ═════════════════════════════════════════════════════════════
// LOBBY TAB — rules / events / chat
// ═════════════════════════════════════════════════════════════
function LobbyView({ game, chat, onSend }) {
  return (
    <div style={{ display: "grid", gridTemplateRows: "auto 1fr auto", height: "100%", overflow: "hidden" }}>
      {/* upper: rules + events */}
      <div style={{ padding: "6px 8px", borderBottom: "1px solid var(--nv-line)", maxHeight: 170, overflow: "hidden" }}>
        <div className="nv-eyebrow" style={{ color: "var(--nv-cyan)", fontSize: 8 }}>RULES</div>
        <div style={{ fontFamily: "var(--nv-mono)", fontSize: 9, color: "var(--nv-ink2)", lineHeight: 1.45, marginTop: 2 }}>
          By using <span style={{ color: "var(--nv-orange)" }}>FM2K_ROLLBACK</span> you adhere to <span style={{ color: "var(--nv-acc)" }}>fgcoc.com</span>.
          Be civil. No racism, sexism, impersonation, threats. <span style={{ color: "var(--nv-amber)" }}>Ignore User</span> via right-click. <span style={{ color: "var(--nv-amber)" }}>Mute Chat</span> in settings. No turbo / macro / rank-boosting.
        </div>
        <hr className="nv-hr" style={{ margin: "5px 0" }} />
        <div style={{ display: "grid", gridTemplateColumns: "1.2fr 1fr", gap: 8 }}>
          <div>
            <div className="nv-eyebrow" style={{ color: "var(--nv-cyan)", fontSize: 8 }}>RESOURCES</div>
            <div style={{ fontFamily: "var(--nv-mono)", fontSize: 8, lineHeight: 1.6, marginTop: 2 }}>
              <ResLink label="discord · brasileiro"     u="discord.gg/jsJ4tdB" />
              <ResLink label="discord · sehab lmrok"    u="discord.gg/Ewdt8Z9CV8" />
              <ResLink label="wiki · supercombo"        u="wiki.supercombo.gg/w/KOF98" />
              <ResLink label="movelist · youtube"       u="youtube.com/playlist?KOF98" />
            </div>
          </div>
          <div>
            <div className="nv-eyebrow" style={{ color: "var(--nv-cyan)", fontSize: 8 }}>EVENTS · {game.code}</div>
            <div style={{ marginTop: 3, padding: 4, border: "1px solid var(--nv-line)", background: "rgba(255,90,60,0.05)" }}>
              <div className="nv-display" style={{ fontSize: 9, color: "var(--nv-ink)" }}>BRASILEIRO 30 · KOF '98</div>
              <div className="nv-data" style={{ fontSize: 7, color: "var(--nv-faint)", marginTop: 1 }}>5/15 · 17:00 · SA · seed=0xB</div>
              <div style={{ marginTop: 3, height: 3, background: "var(--nv-line)", position: "relative" }}>
                <div style={{ position: "absolute", left: 0, top: 0, bottom: 0, width: "62%", background: "var(--nv-orange)" }} />
              </div>
              <div className="nv-data" style={{ fontSize: 7, color: "var(--nv-orange)", marginTop: 1 }}>62/100 · ENTRY OPEN</div>
            </div>
          </div>
        </div>
      </div>
      {/* chat scroll */}
      <div style={{ overflow: "hidden", padding: "4px 8px", fontFamily: "var(--nv-mono)", fontSize: 9.5, lineHeight: 1.55 }}>
        <Stagger step={50} dur={250}>
          {chat.map((m, i) => (
            <ChatLine key={i} m={m} />
          ))}
        </Stagger>
        <div style={{ marginTop: 2, color: "var(--nv-faint)" }}>
          <span style={{ color: "var(--nv-faint)" }}>02:48</span>{" "}
          <span style={{ color: "var(--nv-orange)" }}>‹system›</span>{" "}
          <Typewriter text="14 peers joined since you opened lobby" cps={26} />
        </div>
      </div>
      {/* compose */}
      <div style={{ borderTop: "1px solid var(--nv-line)", padding: "3px 8px", display: "flex", alignItems: "center", gap: 6 }}>
        <span className="nv-acc" style={{ fontSize: 10 }}>›</span>
        <input placeholder={`message #${game.code}-lobby...`} style={{ flex: 1, background: "transparent", border: "none", outline: "none", color: "var(--nv-ink)", fontFamily: "var(--nv-mono)", fontSize: 10 }} />
        <span className="nv-data" style={{ fontSize: 7, color: "var(--nv-faint)" }}>↵ SEND</span>
      </div>
    </div>
  );
}
function ResLink({ label, u }) {
  return (
    <div style={{ display: "flex", justifyContent: "space-between", gap: 4 }}>
      <span style={{ color: "var(--nv-dim)" }}>{label}</span>
      <span style={{ color: "var(--nv-acc)", whiteSpace: "nowrap", overflow: "hidden", textOverflow: "ellipsis" }}>{u}</span>
    </div>
  );
}
function ChatLine({ m }) {
  const sysCol = "var(--nv-orange)";
  const meCol = "var(--nv-cyan)";
  const col = m.sys ? sysCol : (m.me ? meCol : "var(--nv-ink2)");
  return (
    <div>
      <span style={{ color: "var(--nv-faint)" }}>{m.ts}</span>{" "}
      <span style={{ color: col }}>{m.sys ? `‹${m.who}›` : m.who}</span>{m.sys ? " " : ":"} <span style={{ color: m.sys ? "var(--nv-acc)" : "var(--nv-ink)" }}>{m.text}</span>
    </div>
  );
}

// ═════════════════════════════════════════════════════════════
// REPLAYS · RANKINGS · EVENTS · PROFILE
// ═════════════════════════════════════════════════════════════
function ReplaysView() {
  const rows = [
    ["00","2024-04-14 02:14","ANONYM",  "MURAKUMO","2-1","T-DOM",  "8m12s"],
    ["01","2024-04-14 01:48","SAI",     "WEI",     "2-0","T-EAST", "5m04s"],
    ["02","2024-04-14 01:22","KANAE",   "FRITZ",   "1-2","T-NIGHT","9m38s"],
    ["03","2024-04-13 23:59","ANONYM",  "ADLER",   "2-0","T-DOM",  "4m22s"],
    ["04","2024-04-13 23:14","MARILYN", "KAMI",    "2-1","T-EAST", "7m11s"],
    ["05","2024-04-13 22:48","ANONYM",  "BLITZ",   "0-2","T-NIGHT","6m42s"],
    ["06","2024-04-13 22:01","MYCALE",  "SOLDAT",  "2-0","T-DOM",  "3m58s"],
    ["07","2024-04-13 21:22","ANONYM",  "MURAKUMO","2-2","T-NIGHT","11m04s"],
    ["08","2024-04-13 20:42","WEI",     "FRITZ",   "1-2","T-EAST", "8m18s"],
  ];
  return (
    <div style={{ height: "100%", display: "flex", flexDirection: "column", padding: "4px 0" }}>
      <div style={{ display: "grid", gridTemplateColumns: "22px 80px 1fr 1fr 36px 60px 50px", padding: "3px 8px", borderBottom: "1px solid var(--nv-line)" }}>
        {["IDX","DATE","P1","P2","SCR","STAGE","DUR"].map(h => <span key={h} className="nv-label" style={{ fontSize: 8 }}>{h}</span>)}
      </div>
      <div style={{ flex: 1, overflow: "hidden" }}>
        <Stagger step={26} dur={260}>
          {rows.map((r, i) => (
            <div key={i} style={{
              display: "grid", gridTemplateColumns: "22px 80px 1fr 1fr 36px 60px 50px", padding: "3px 8px",
              fontFamily: "var(--nv-mono)", fontSize: 9, cursor: "pointer",
              borderBottom: "1px dotted rgba(255,255,255,0.04)",
            }}>
              <span style={{ color: "var(--nv-faint)" }}>{r[0]}</span>
              <span style={{ color: "var(--nv-dim)", fontSize: 8 }}>{r[1]}</span>
              <span style={{ color: "var(--nv-ink)" }}>{r[2]}</span>
              <span style={{ color: "var(--nv-ink)" }}>{r[3]}</span>
              <span style={{ color: "var(--nv-orange)" }}>{r[4]}</span>
              <span style={{ color: "var(--nv-dim)" }}>{r[5]}</span>
              <span style={{ color: "var(--nv-dim)" }}>{r[6]}</span>
            </div>
          ))}
        </Stagger>
      </div>
    </div>
  );
}
function RankingsView({ game }) {
  const rows = [
    ["#001", "tokido_2k",   "JP", 2412, "+18", "62-12"],
    ["#002", "luis_cha",    "BR", 2388, "+04", "58-15"],
    ["#003", "xiaohai",     "CN", 2371, "−02", "44-09"],
    ["#004", "hajimari",    "JP", 2354, "+11", "51-14"],
    ["#005", "nemo",        "JP", 2338, "−03", "39-11"],
    ["#006", "luffy",       "FR", 2320, "+06", "33-09"],
    ["#007", "infiltration","KR", 2304, "−05", "28-10"],
    ["#008", "punkdaGod",   "US", 2289, "+02", "26-08"],
    ["#142", "sloth",       "JP", 1842, "+17", "12-04", true],
  ];
  return (
    <div style={{ height: "100%", padding: "4px 8px" }}>
      <div className="nv-eyebrow nv-acc" style={{ fontSize: 8 }}>{game.code} · GLOBAL LADDER · S07</div>
      <hr className="nv-hr" style={{ margin: "3px 0" }} />
      <div style={{ display: "grid", gridTemplateColumns: "30px 1fr 18px 50px 32px 48px", padding: "2px 0", borderBottom: "1px dotted var(--nv-line)" }}>
        {["RNK","HANDLE","R","ELO","∆","W-L"].map(h => <span key={h} className="nv-label" style={{ fontSize: 8 }}>{h}</span>)}
      </div>
      <Stagger step={28} dur={280}>
        {rows.map((r, i) => (
          <div key={i} style={{
            display: "grid", gridTemplateColumns: "30px 1fr 18px 50px 32px 48px", padding: "3px 0", alignItems: "center",
            fontFamily: "var(--nv-mono)", fontSize: 10,
            background: r[6] ? "rgba(255,90,60,0.1)" : "transparent",
            borderLeft: r[6] ? "2px solid var(--nv-orange)" : "2px solid transparent",
            paddingLeft: 4,
          }}>
            <span style={{ color: i < 3 ? "var(--nv-amber)" : "var(--nv-faint)", fontFamily: "var(--nv-display)" }}>{r[0]}</span>
            <span style={{ color: r[6] ? "var(--nv-orange)" : "var(--nv-ink)" }}>{r[1]}</span>
            <FlagDot region={r[2]} />
            <span className="nv-num" style={{ color: "var(--nv-ink)", fontSize: 11 }}>{r[3]}</span>
            <span style={{ color: r[4].startsWith("+") ? "var(--nv-phos)" : "var(--nv-magenta)", fontSize: 9 }}>{r[4]}</span>
            <span style={{ color: "var(--nv-dim)", fontSize: 9 }}>{r[5]}</span>
          </div>
        ))}
      </Stagger>
    </div>
  );
}
function EventsView() {
  const events = [
    { d: "MAY 15", t: "17:00", n: "BRASILEIRO 30",  game: "KOF98", region: "SA",   reg: "62/100" },
    { d: "MAY 09", t: "14:00", n: "DTK · STAGE #80", game: "3S",    region: "WLD",  reg: "44/64" },
    { d: "MAY 09", t: "15:00", n: "MAX MONTHLY #2", game: "ANY",   region: "NA+EU",reg: "OPEN" },
    { d: "MAY 12", t: "20:00", n: "FROSTY · KOF02", game: "KOF02", region: "WORLD",reg: "08/32" },
    { d: "MAY 18", t: "13:00", n: "SOFA #506",      game: "3S",    region: "WEU",  reg: "OPEN" },
  ];
  return (
    <div style={{ padding: "4px 8px", height: "100%" }}>
      <div className="nv-eyebrow nv-acc" style={{ fontSize: 8 }}>UPCOMING · 30D · FILTER ▾</div>
      <hr className="nv-hr" style={{ margin: "3px 0" }} />
      <Stagger step={50} dur={300}>
        {events.map((e, i) => (
          <div key={i} style={{
            display: "grid", gridTemplateColumns: "44px 1fr 50px 80px", gap: 6, alignItems: "center",
            padding: "5px 4px", borderBottom: "1px dashed var(--nv-line)", cursor: "pointer",
          }}>
            <div>
              <div className="nv-display" style={{ fontSize: 11, color: "var(--nv-orange)", lineHeight: 1.0 }}>{e.d}</div>
              <div className="nv-data" style={{ fontSize: 8, color: "var(--nv-faint)" }}>{e.t}</div>
            </div>
            <div>
              <div className="nv-display" style={{ fontSize: 10, color: "var(--nv-ink)", letterSpacing: "0.04em" }}>{e.n}</div>
              <div className="nv-data" style={{ fontSize: 8, color: "var(--nv-dim)" }}>{e.game} · {e.region}</div>
            </div>
            <div style={{ textAlign: "right" }}>
              <div className="nv-num" style={{ fontSize: 11, color: e.reg === "OPEN" ? "var(--nv-phos)" : "var(--nv-orange)" }}>{e.reg}</div>
              <div className="nv-data" style={{ fontSize: 7, color: "var(--nv-faint)" }}>SEAT</div>
            </div>
            <div style={{ padding: "2px 4px", border: "1px solid var(--nv-orange)", color: "var(--nv-orange)", fontFamily: "var(--nv-mono)", fontSize: 8, textAlign: "center" }}>+ ENROLL</div>
          </div>
        ))}
      </Stagger>
    </div>
  );
}
function ProfileView() {
  const ladder = aM(() => Array.from({length:40},(_,i)=>1700 + Math.sin(i*0.4)*60 + Math.random()*30 + i*4),[]);
  const wr = aM(() => Array.from({length:40},(_,i)=>0.5 + Math.sin(i*0.25)*0.18 + (Math.random()-0.5)*0.05),[]);
  return (
    <div style={{ padding: "5px 8px", height: "100%", overflow: "hidden" }}>
      <div style={{ display: "flex", gap: 8, alignItems: "flex-start" }}>
        <div style={{ width: 56, height: 56, border: "1px solid var(--nv-orange)", background: "linear-gradient(135deg,#5a2a85,#ff5a3c)", display: "flex", alignItems: "center", justifyContent: "center", fontFamily: "var(--nv-display)", fontSize: 32, color: "#fff", flexShrink: 0 }}>S</div>
        <div style={{ flex: 1 }}>
          <div className="nv-eyebrow nv-acc" style={{ fontSize: 8 }}>HANDLE · ID/0xF7421</div>
          <div className="nv-display" style={{ fontSize: 18, marginTop: 2 }}>sloth<span className="nv-acc">#7421</span></div>
          <div className="nv-data" style={{ fontSize: 8, color: "var(--nv-dim)", marginTop: 1 }}>JP · joined hub.2dfm.org · 2024-03-11 · main: KOF98</div>
        </div>
      </div>
      <hr className="nv-hr" style={{ margin: "5px 0" }} />
      <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: 10 }}>
        <div>
          <NvRow k="ELO">1842 (#142)</NvRow>
          <NvRow k="W/L" acc>412 / 318 · 56.4%</NvRow>
          <NvRow k="STREAK">+5 ·  🔥</NvRow>
          <NvRow k="MAIN CHAR">ANONYM</NvRow>
          <NvRow k="AVG PING">22ms</NvRow>
          <NvRow k="ROLLBACK">8F · 3F delay</NvRow>
        </div>
        <div>
          <div className="nv-label" style={{ fontSize: 8, marginBottom: 1 }}>ELO · 40 SETS</div>
          <PingGraph buf={ladder} w={186} h={26} color="var(--nv-orange)" max={Math.max(...ladder)} />
          <div className="nv-label" style={{ fontSize: 8, marginTop: 5 }}>WIN RATE · 40 SETS</div>
          <PingGraph buf={wr} w={186} h={26} color="var(--nv-phos)" max={1} fill />
          <div className="nv-data" style={{ fontSize: 7, color: "var(--nv-faint)", display:"flex", justifyContent:"space-between" }}><span>0%</span><span>100%</span></div>
        </div>
      </div>
      <hr className="nv-hr" style={{ margin: "5px 0" }} />
      <div className="nv-eyebrow" style={{ fontSize: 8 }}>RECENT · LAST 5</div>
      <div style={{ marginTop: 3 }}>
        {[
          ["02:14", "vs MURAKUMO/aoki",     "L 1-2", "AKB"],
          ["01:22", "vs FRITZ/kanae",       "W 2-1", "AKB"],
          ["YDAY 23:59", "vs ADLER/missingno","W 2-0", "AKB"],
          ["YDAY 22:48", "vs BLITZ/kuroneko","L 0-2", "AKB"],
          ["YDAY 21:22", "vs MURAKUMO/aoki", "D 2-2", "AKB"],
        ].map((r, i) => (
          <div key={i} style={{ display: "grid", gridTemplateColumns: "82px 1fr 50px 32px", gap: 4, padding: "1px 0", fontFamily: "var(--nv-mono)", fontSize: 9 }}>
            <span style={{ color: "var(--nv-faint)" }}>{r[0]}</span>
            <span style={{ color: "var(--nv-ink2)" }}>{r[1]}</span>
            <span style={{ color: r[2].startsWith("W") ? "var(--nv-phos)" : r[2].startsWith("L") ? "var(--nv-acc)" : "var(--nv-amber)" }}>{r[2]}</span>
            <span style={{ color: "var(--nv-dim)" }}>{r[3]}</span>
          </div>
        ))}
      </div>
    </div>
  );
}

// ═════════════════════════════════════════════════════════════
// CONTROLLER MAPPER — virtual keyboard / gamepad visualizer
// ═════════════════════════════════════════════════════════════
function ControllerMapper({ onClose }) {
  const [mode, setMode] = aS("kb"); // "kb" | "pad"
  const actions = ["LP","MP","HP","LK","MK","HK","START","SELECT"];
  const [step, setStep] = aS(0);
  const [bound, setBound] = aS({});
  const tick = useTick(2);

  const action = actions[step];
  const done = step >= actions.length;

  // simulate input: every 2.5s register
  aE(() => {
    if (done) return;
    const t = setTimeout(() => {
      const sample = mode === "kb"
        ? ["A","S","D","Z","X","C","ENTER","TAB"][step]
        : ["⓪ A","⓪ B","⓪ X","⓪ Y","RB","RT","ST","SE"][step];
      setBound(b => ({...b, [action]: sample}));
      setStep(s => s + 1);
    }, 2200);
    return () => clearTimeout(t);
  }, [step, mode, action, done]);

  return (
    <div style={{ height: "100%", display: "flex", flexDirection: "column" }}>
      <NvHeader id="07.MAP" title="Input · Press To Map" sub={mode === "kb" ? "KEYBOARD MODE" : "GAMEPAD-0 · XInput"} telemetry={
        <div style={{ display: "flex", gap: 4 }}>
          <BarBtn onClick={() => setMode("kb")} accent={mode === "kb"}>KEYBOARD</BarBtn>
          <BarBtn onClick={() => setMode("pad")} accent={mode === "pad"}>GAMEPAD</BarBtn>
        </div>
      } />
      <div style={{ flex: 1, display: "grid", gridTemplateColumns: "1fr 156px", overflow: "hidden" }}>
        {/* left: visualizer */}
        <div style={{ padding: "8px 10px", display: "flex", flexDirection: "column", justifyContent: "space-between", borderRight: "1px solid var(--nv-line)" }}>
          {mode === "kb" ? <KeyboardViz pressed={bound[action]} actionsBound={bound} /> : <GamepadViz pressed={bound[action]} actionsBound={bound} />}
          <div style={{ marginTop: 8 }}>
            <div className="nv-eyebrow" style={{ color: "var(--nv-cyan)", fontSize: 8 }}>{done ? "COMPLETE" : "AWAITING INPUT"}</div>
            <div className="nv-display" style={{ fontSize: 22, marginTop: 2 }}>
              {done ? <span className="nv-acc">ALL BOUND ✓</span> : <>PRESS <span className="nv-acc">{action}</span></>}
            </div>
            <div style={{ marginTop: 4, height: 4, background: "var(--nv-line)", position: "relative" }}>
              <div style={{ position: "absolute", left: 0, top: 0, bottom: 0, width: `${(step / actions.length) * 100}%`, background: "var(--nv-orange)", transition: "width 280ms" }} />
            </div>
            <div className="nv-data" style={{ fontSize: 8, color: "var(--nv-faint)", marginTop: 2, display: "flex", justifyContent: "space-between" }}>
              <span>STEP {Math.min(step + 1, actions.length)}/{actions.length}</span>
              <span>{tick % 2 ? "▌" : " "} listening · 60Hz polling</span>
            </div>
          </div>
        </div>
        {/* right: bindings */}
        <div style={{ padding: "6px 8px" }}>
          <div className="nv-eyebrow" style={{ fontSize: 8 }}>BINDINGS</div>
          <hr className="nv-hr" style={{ margin: "3px 0" }} />
          {actions.map((a, i) => (
            <div key={a} style={{
              display: "grid", gridTemplateColumns: "1fr auto", padding: "3px 0",
              fontFamily: "var(--nv-mono)", fontSize: 10, alignItems: "center",
              borderLeft: i === step ? "2px solid var(--nv-orange)" : "2px solid transparent",
              paddingLeft: 4,
              opacity: i === step ? 1 : (bound[a] ? 0.85 : 0.45),
            }}>
              <span style={{ color: i === step ? "var(--nv-orange)" : (bound[a] ? "var(--nv-ink)" : "var(--nv-faint)") }}>{a}</span>
              <span style={{
                fontSize: 9, padding: "1px 5px", border: "1px solid",
                borderColor: bound[a] ? "var(--nv-phos)" : "var(--nv-line)",
                color: bound[a] ? "var(--nv-phos)" : "var(--nv-faint)",
                minWidth: 30, textAlign: "center",
              }}>
                {bound[a] || (i === step ? "..." : "—")}
              </span>
            </div>
          ))}
          <hr className="nv-hr" style={{ margin: "5px 0" }} />
          <div style={{ display: "flex", flexDirection: "column", gap: 3 }}>
            <BarBtn onClick={() => { setStep(0); setBound({}); }}>↺ RESET</BarBtn>
            <BarBtn onClick={onClose} accent>↵ DONE</BarBtn>
          </div>
        </div>
      </div>
    </div>
  );
}
function KeyboardViz({ pressed, actionsBound }) {
  const rows = [
    ["1","2","3","4","5","6","7","8","9","0"],
    ["Q","W","E","R","T","Y","U","I","O","P"],
    ["A","S","D","F","G","H","J","K","L","↵"],
    ["⇧","Z","X","C","V","B","N","M",",","."],
    ["⌃","⎇","␣","␣","␣","␣","␣","⎇","TAB","⌃"],
  ];
  // reverse-map binding labels per key
  const pressedKey = pressed;
  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 2 }}>
      {rows.map((row, ri) => (
        <div key={ri} style={{ display: "flex", gap: 2, justifyContent: ri === 4 ? "center" : "flex-start" }}>
          {row.map((k, ki) => {
            const boundAction = Object.keys(actionsBound).find(a => actionsBound[a] === k);
            const wasPressed = k === pressedKey;
            return (
              <div key={ki} style={{
                width: ri === 4 ? (k === "␣" ? 16 : 22) : 22, height: 18,
                border: `1px solid ${wasPressed ? "var(--nv-orange)" : boundAction ? "var(--nv-phos)" : "var(--nv-line)"}`,
                background: wasPressed ? "rgba(255,90,60,0.3)" : boundAction ? "rgba(95,255,149,0.08)" : "rgba(255,255,255,0.02)",
                display: "flex", alignItems: "center", justifyContent: "center",
                fontFamily: "var(--nv-mono)", fontSize: 8,
                color: wasPressed ? "var(--nv-orange)" : boundAction ? "var(--nv-phos)" : "var(--nv-ink2)",
                position: "relative", boxShadow: wasPressed ? "0 0 8px rgba(255,90,60,0.6)" : "none",
                transition: "all 200ms",
              }}>
                {k}
                {boundAction && <div style={{ position: "absolute", top: -7, right: -2, fontSize: 6, color: "var(--nv-phos)", letterSpacing: "0.05em" }}>{boundAction}</div>}
              </div>
            );
          })}
        </div>
      ))}
    </div>
  );
}
function GamepadViz({ pressed, actionsBound }) {
  // SVG-based xbox-style pad
  const isPressed = (key) => pressed && pressed.includes(key);
  const boundFor = (k) => Object.keys(actionsBound).find(a => actionsBound[a] && actionsBound[a].includes(k));
  const t = useTime();
  const stickWobble = (Math.sin(t * 1.6) * 0.6, Math.cos(t * 1.7) * 0.6); // unused; just left/right thumbsticks idle wobble below
  return (
    <div style={{ display: "flex", justifyContent: "center", alignItems: "center" }}>
      <svg viewBox="0 0 320 180" width="100%" height={150} style={{ display: "block" }}>
        {/* body */}
        <path d="M40,90 Q40,40 100,40 L220,40 Q280,40 280,90 L290,140 Q290,170 260,170 Q230,170 220,150 L200,140 L120,140 L100,150 Q90,170 60,170 Q30,170 40,140 Z"
          fill="#0a0612" stroke="#3d1c5e" strokeWidth="1.5"/>
        {/* DPAD */}
        <g transform="translate(75,90)">
          <rect x="-4" y="-14" width="8" height="28" fill={isPressed("UP") || isPressed("DOWN") ? "#ff5a3c" : "#1d0d2e"} stroke="#3d1c5e"/>
          <rect x="-14" y="-4" width="28" height="8" fill={isPressed("LEFT") || isPressed("RIGHT") ? "#ff5a3c" : "#1d0d2e"} stroke="#3d1c5e"/>
          <text x="0" y="-18" fontSize="6" fill="#888" textAnchor="middle" fontFamily="monospace">DPAD</text>
        </g>
        {/* face buttons (A B X Y) */}
        <g transform="translate(230,90)">
          {[
            { k: "Y", x: 0, y: -16, c: "#ffb938" },
            { k: "X", x: -16, y: 0, c: "#0090ff" },
            { k: "B", x: 16, y: 0, c: "#ff3030" },
            { k: "A", x: 0, y: 16, c: "#00a050" },
          ].map(b => {
            const pr = isPressed(b.k);
            const bd = boundFor(b.k);
            return (
              <g key={b.k}>
                <circle cx={b.x} cy={b.y} r="7"
                  fill={pr ? b.c : "#1d0d2e"}
                  stroke={pr ? "#fff" : bd ? "#5fff95" : "#3d1c5e"} strokeWidth={pr ? 2 : 1}
                  style={{ filter: pr ? `drop-shadow(0 0 6px ${b.c})` : "none" }}/>
                <text x={b.x} y={b.y + 2} fontSize="8" fill={pr ? "#000" : b.c} textAnchor="middle" fontFamily="monospace" fontWeight="bold">{b.k}</text>
                {bd && <text x={b.x} y={b.y + 16} fontSize="5" fill="#5fff95" textAnchor="middle" fontFamily="monospace">{bd}</text>}
              </g>
            );
          })}
        </g>
        {/* sticks */}
        {[{cx:120,cy:120,k:"LS"},{cx:200,cy:120,k:"RS"}].map((s,i)=>{
          const dx = Math.sin(t * 1.6 + i) * 1.2;
          const dy = Math.cos(t * 1.7 + i) * 1.2;
          return (
            <g key={s.k}>
              <circle cx={s.cx} cy={s.cy} r="14" fill="#0a0612" stroke="#3d1c5e"/>
              <circle cx={s.cx + dx} cy={s.cy + dy} r="8" fill="#1d0d2e" stroke="#ff5a3c" strokeWidth="0.8"/>
              <text x={s.cx} y={s.cy + 2} fontSize="6" fill="#ff5a3c" textAnchor="middle" fontFamily="monospace">{s.k}</text>
            </g>
          );
        })}
        {/* shoulders */}
        {[
          { x: 70,  k: "LB" }, { x: 250, k: "RB" },
        ].map(s => {
          const pr = isPressed(s.k); const bd = boundFor(s.k);
          return (
            <g key={s.k}>
              <rect x={s.x} y="22" width="24" height="8" rx="2"
                fill={pr ? "#ff5a3c" : "#1d0d2e"}
                stroke={bd ? "#5fff95" : "#3d1c5e"}/>
              <text x={s.x + 12} y="28" fontSize="6" fill={pr ? "#000" : "#888"} textAnchor="middle" fontFamily="monospace" fontWeight="bold">{s.k}</text>
            </g>
          );
        })}
        {/* triggers */}
        {[{ x: 78, k: "LT" },{ x: 218, k: "RT" }].map(s => {
          const pr = isPressed(s.k); const bd = boundFor(s.k);
          return (
            <g key={s.k}>
              <rect x={s.x} y="10" width="16" height="10" rx="3"
                fill={pr ? "#ff5a3c" : "#1d0d2e"}
                stroke={bd ? "#5fff95" : "#3d1c5e"}/>
              <text x={s.x + 8} y="17" fontSize="6" fill="#888" textAnchor="middle" fontFamily="monospace">{s.k}</text>
            </g>
          );
        })}
        {/* center cluster */}
        <circle cx="160" cy="80" r="6" fill="#0a0612" stroke="#ff5a3c"/>
        <text x="160" y="83" fontSize="7" fill="#ff5a3c" textAnchor="middle" fontFamily="monospace" fontWeight="bold">F</text>
        <text x="140" y="84" fontSize="5" fill="#888" textAnchor="middle" fontFamily="monospace">SE</text>
        <text x="180" y="84" fontSize="5" fill="#888" textAnchor="middle" fontFamily="monospace">ST</text>
      </svg>
    </div>
  );
}

// ═════════════════════════════════════════════════════════════
// SETTINGS
// ═════════════════════════════════════════════════════════════
function SettingsView() {
  const [tab, setTab] = aS("net");
  const tabs = [["net","NET"],["av","A/V"],["chat","CHAT"],["adv","ADV"]];
  return (
    <div style={{ height: "100%", display: "flex", flexDirection: "column" }}>
      <div style={{ display: "flex", borderBottom: "1px solid var(--nv-line)" }}>
        {tabs.map(([id, k]) => (
          <div key={id} onClick={() => setTab(id)} style={{
            padding: "4px 10px", fontFamily: "var(--nv-mono)", fontSize: 9, letterSpacing: "0.1em", cursor: "pointer",
            color: tab === id ? "var(--nv-orange)" : "var(--nv-ink2)",
            borderBottom: tab === id ? "2px solid var(--nv-orange)" : "none",
            marginBottom: -1,
          }}>{k}</div>
        ))}
      </div>
      <div style={{ flex: 1, padding: "6px 10px" }}>
        {tab === "net" && <SettingsNet />}
        {tab === "av"  && <SettingsAV />}
        {tab === "chat"&& <SettingsChat />}
        {tab === "adv" && <SettingsAdv />}
      </div>
    </div>
  );
}
function Toggle({ on, label }) {
  return (
    <div style={{ display: "flex", alignItems: "center", gap: 6 }}>
      <div style={{ width: 22, height: 10, border: "1px solid var(--nv-line)", padding: 1, position: "relative", background: on ? "rgba(255,90,60,0.15)" : "transparent" }}>
        <div style={{ position: "absolute", top: 1, bottom: 1, width: 8, left: on ? 11 : 1, background: on ? "var(--nv-orange)" : "var(--nv-faint)", transition: "left 180ms" }} />
      </div>
      <span style={{ fontFamily: "var(--nv-mono)", fontSize: 9, color: "var(--nv-ink2)" }}>{label}</span>
    </div>
  );
}
function SettingsNet() {
  const ping = useTelemetry({ base: 14, jitter: 0.7, drift: 0.5, hz: 8, samples: 70 });
  return (
    <div>
      <NvRow k="STUN HOST">stun.relay-tokyo:3478</NvRow>
      <NvRow k="REGION" acc>JP-EAST · 14ms</NvRow>
      <NvRow k="PORT">UDP 9293–9303</NvRow>
      <NvRow k="MTU">1392 · auto</NvRow>
      <hr className="nv-hr" style={{ margin: "5px 0" }} />
      <Toggle on label="Auto-rebind on NAT change" />
      <div style={{ height: 4 }} />
      <Toggle on label="Prefer LAN peers when reachable" />
      <div style={{ height: 4 }} />
      <Toggle label="Fallback TCP relay (high-latency)" />
      <hr className="nv-hr" style={{ margin: "5px 0" }} />
      <div className="nv-label" style={{ fontSize: 8, marginBottom: 2 }}>RTT · live</div>
      <PingGraph buf={ping} w={420} h={28} color="var(--nv-orange)" max={28} />
    </div>
  );
}
function SettingsAV() {
  return (
    <div>
      <NvRow k="RENDERER">D3D11 · DXGI</NvRow>
      <NvRow k="VSYNC" acc>OFF · uncap</NvRow>
      <NvRow k="UPSCALE">CRT · ABERRATED</NvRow>
      <NvRow k="AUDIO">WASAPI · 48kHz · 64f</NvRow>
      <hr className="nv-hr" style={{ margin: "5px 0" }} />
      <Toggle on label="Integer scale" />
      <div style={{ height: 4 }} /><Toggle on label="Scanline filter" />
      <div style={{ height: 4 }} /><Toggle label="Force 60Hz refresh" />
    </div>
  );
}
function SettingsChat() {
  return (
    <div>
      <Toggle on label="Show system messages" />
      <div style={{ height: 4 }} /><Toggle on label="Show timestamps" />
      <div style={{ height: 4 }} /><Toggle label="Mute in-game chat" />
      <div style={{ height: 4 }} /><Toggle label="Auto-hide after match start" />
      <hr className="nv-hr" style={{ margin: "5px 0" }} />
      <NvRow k="HANDLE COLOR" acc>orange · #FF5A3C</NvRow>
      <NvRow k="IGNORE LIST">3 entries</NvRow>
    </div>
  );
}
function SettingsAdv() {
  return (
    <div>
      <NvRow k="ROLLBACK">8F</NvRow>
      <NvRow k="DELAY">3F</NvRow>
      <NvRow k="INPUT POLL">1000Hz</NvRow>
      <NvRow k="LOG LEVEL">INFO</NvRow>
      <hr className="nv-hr" style={{ margin: "5px 0" }} />
      <Toggle label="Verbose desync diagnostics" />
      <div style={{ height: 4 }} /><Toggle on label="Crash dump on K.O." />
    </div>
  );
}

// ═════════════════════════════════════════════════════════════
// STATS — demoscene-tier dashboard
// ═════════════════════════════════════════════════════════════
function StatsView() {
  const setsToday = useTelemetry({ base: 14, jitter: 1.2, drift: 0.6, hz: 4, samples: 50 });
  const peers     = useTelemetry({ base: 380, jitter: 6, drift: 4, hz: 4, samples: 50 });
  const chars = [["ANONYM",62,"#ff5a3c"],["MURAKUMO",18,"#0090ff"],["ADLER",11,"#ffb938"],["KAMI",9,"#5fff95"]];
  const regions = [["JP",42],["BR",18],["MX",14],["US",10],["EU",16]];
  const tod = Array.from({length:24},(_,i)=>0.2+Math.abs(Math.sin(i*0.3+1.4))*0.8);
  return (
    <div style={{ padding: "5px 8px", height: "100%", overflow: "hidden", display: "grid", gridTemplateRows: "auto auto 1fr", gap: 5 }}>
      <div style={{ display: "grid", gridTemplateColumns: "repeat(4, 1fr)", gap: 4 }}>
        {[
          ["SETS · TODAY","12","var(--nv-orange)"],
          ["WIN RATE","58%","var(--nv-phos)"],
          ["AVG SESSION","42m","var(--nv-amber)"],
          ["STREAK","+5","var(--nv-acc)"],
        ].map(([k,v,c],i)=>(
          <FadeIn key={k} delay={i*50} dur={300}>
            <div style={{ border: `1px solid ${c}`, padding: "3px 5px", background: `linear-gradient(135deg,${c}11,transparent 70%)` }}>
              <div className="nv-label" style={{ fontSize: 7, color: c }}>{k}</div>
              <div className="nv-display" style={{ fontSize: 18, color: "var(--nv-ink)", lineHeight: 1.0 }}>{v}</div>
            </div>
          </FadeIn>
        ))}
      </div>
      <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: 6 }}>
        <div style={{ border: "1px solid var(--nv-line)", padding: "3px 5px" }}>
          <div className="nv-eyebrow" style={{ fontSize: 7 }}>HUB · PEERS ONLINE</div>
          <PingGraph buf={peers} w={196} h={26} color="var(--nv-cyan)" max={Math.max(...peers)} />
        </div>
        <div style={{ border: "1px solid var(--nv-line)", padding: "3px 5px" }}>
          <div className="nv-eyebrow" style={{ fontSize: 7 }}>YOUR · SETS/HR</div>
          <PingGraph buf={setsToday} w={196} h={26} color="var(--nv-orange)" max={20} />
        </div>
      </div>
      <div style={{ display: "grid", gridTemplateColumns: "1.1fr 0.9fr 1fr", gap: 6, minHeight: 0 }}>
        <div style={{ border: "1px solid var(--nv-line)", padding: "3px 5px" }}>
          <div className="nv-eyebrow" style={{ fontSize: 7 }}>W/L · BY CHAR</div>
          <hr className="nv-hr" style={{ margin: "2px 0" }} />
          {chars.map(([n,p,c])=>(
            <div key={n} style={{ display: "grid", gridTemplateColumns: "60px 1fr 26px", gap: 4, alignItems: "center", padding: "1px 0", fontFamily: "var(--nv-mono)", fontSize: 9 }}>
              <span style={{ color: "var(--nv-ink2)" }}>{n}</span>
              <div style={{ height: 5, background: "var(--nv-line)", position: "relative" }}>
                <div style={{ position: "absolute", left: 0, top: 0, bottom: 0, width: `${p}%`, background: c }} />
              </div>
              <span style={{ color: c, textAlign: "right" }}>{p}%</span>
            </div>
          ))}
        </div>
        <div style={{ border: "1px solid var(--nv-line)", padding: "3px 5px" }}>
          <div className="nv-eyebrow" style={{ fontSize: 7 }}>BY REGION</div>
          <hr className="nv-hr" style={{ margin: "2px 0" }} />
          {regions.map(([r,p])=>(
            <div key={r} style={{ display: "grid", gridTemplateColumns: "16px 1fr 22px", gap: 4, alignItems: "center", padding: "1px 0", fontFamily: "var(--nv-mono)", fontSize: 9 }}>
              <FlagDot region={r} />
              <div style={{ height: 4, background: "var(--nv-line)", position: "relative" }}>
                <div style={{ position: "absolute", left: 0, top: 0, bottom: 0, width: `${p*2}%`, background: "var(--nv-phos)" }} />
              </div>
              <span style={{ color: "var(--nv-dim)", textAlign: "right" }}>{p}</span>
            </div>
          ))}
        </div>
        <div style={{ border: "1px solid var(--nv-line)", padding: "3px 5px" }}>
          <div className="nv-eyebrow" style={{ fontSize: 7 }}>TIME OF DAY · 24H</div>
          <hr className="nv-hr" style={{ margin: "2px 0" }} />
          <div style={{ display: "flex", alignItems: "flex-end", gap: 1, height: 50 }}>
            {tod.map((v,i)=>(
              <div key={i} style={{ flex: 1, height: `${v*100}%`, background: i===20?"var(--nv-orange)":"var(--nv-magenta)", opacity: i===20?1:0.6 }} />
            ))}
          </div>
          <div style={{ display: "flex", justifyContent: "space-between", fontFamily: "var(--nv-mono)", fontSize: 7, color: "var(--nv-faint)", marginTop: 1 }}>
            <span>00</span><span>06</span><span>12</span><span>18</span><span>24</span>
          </div>
        </div>
      </div>
    </div>
  );
}

// ═════════════════════════════════════════════════════════════
// MATCH FLOW · outgoing → handshake → booting → ingame → post / desync
// ═════════════════════════════════════════════════════════════
function MatchFlow({ stage, user, onAdvance, onCancel, onClose }) {
  if (!user) return null;
  const Wrap = ({ accent="var(--nv-orange)", children }) => (
    <div style={{
      position: "absolute", left: 64, top: 36, right: 168, bottom: 24, zIndex: 40,
      background: "rgba(10,6,18,0.97)", border: `1px solid ${accent}`,
      boxShadow: `0 0 24px ${accent}55, 0 0 0 100vmax rgba(0,0,0,0.55)`,
      display: "flex", flexDirection: "column",
    }}>{children}</div>
  );
  if (stage === "incoming")  return <Wrap><IncomingPanel user={user} onAccept={()=>onAdvance("handshake")} onClose={onClose} /></Wrap>;
  if (stage === "outgoing")  return <Wrap accent="var(--nv-cyan)"><OutgoingPanel user={user} onCancel={onCancel} /></Wrap>;
  if (stage === "handshake") return <Wrap accent="var(--nv-cyan)"><HandshakePanel user={user} onDone={()=>onAdvance("booting")} /></Wrap>;
  if (stage === "booting")   return <Wrap accent="var(--nv-amber)"><BootingPanel user={user} onDone={()=>onAdvance("ingame")} /></Wrap>;
  if (stage === "post")      return <Wrap><PostPanel user={user} onRematch={()=>onAdvance("handshake")} onQuit={onClose} /></Wrap>;
  if (stage === "desync")    return <Wrap accent="var(--nv-magenta)"><DesyncPanel user={user} onResync={()=>onAdvance("handshake")} onAbort={onClose} /></Wrap>;
  return null;
}
function IncomingPanel({ user, onAccept, onClose }) {
  const [t, setT] = aS(20);
  aE(() => { if (t<=0) return onClose(); const id=setTimeout(()=>setT(x=>x-1),1000); return ()=>clearTimeout(id); }, [t]);
  return (<>
    <div style={{ padding: "4px 8px", borderBottom: "1px solid var(--nv-orange)", background: "rgba(255,90,60,0.08)", display: "flex", justifyContent: "space-between" }}>
      <div className="nv-eyebrow nv-acc" style={{ fontSize: 8 }}>◆ INCOMING CHALLENGE</div>
      <div onClick={onClose} className="nv-data" style={{ fontSize: 9, color: "var(--nv-faint)", cursor: "pointer" }}>× CLOSE</div>
    </div>
    <div style={{ padding: "10px 14px", flex: 1, display: "grid", gridTemplateColumns: "1fr 1fr", gap: 14 }}>
      <FadeIn delay={50} dur={400}>
        <div>
          <div className="nv-eyebrow" style={{ color: "var(--nv-cyan)", fontSize: 8 }}>CHALLENGER</div>
          <div className="nv-display" style={{ fontSize: 22 }}>{user.name}</div>
          <div style={{ display: "flex", gap: 6, alignItems: "center", marginTop: 2 }}>
            <FlagDot region={user.region} /><span className="nv-data" style={{ fontSize: 9, color: "var(--nv-dim)" }}>{user.region} · ELO 1730</span>
          </div>
          <hr className="nv-hr" style={{ margin: "8px 0" }} />
          <NvRow k="PING">{user.ping}ms · stable</NvRow>
          <NvRow k="MAIN">MURAKUMO</NvRow>
          <NvRow k="W/L vs YOU">3 / 4</NvRow>
        </div>
      </FadeIn>
      <FadeIn delay={150} dur={400}>
        <div style={{ border: "1px solid var(--nv-line)", padding: 8 }}>
          <div className="nv-eyebrow" style={{ fontSize: 8 }}>RULESET</div>
          <hr className="nv-hr" style={{ margin: "4px 0" }} />
          <NvRow k="FORMAT" acc>FT3 · BO5</NvRow>
          <NvRow k="ROLLBACK">8F</NvRow><NvRow k="DELAY">3F</NvRow>
          <hr className="nv-hr" style={{ margin: "4px 0" }} />
          <div className="nv-data" style={{ fontSize: 8, color: "var(--nv-faint)" }}>EXPIRES</div>
          <div className="nv-display" style={{ fontSize: 28, color: "var(--nv-orange)" }}>{String(t).padStart(2,"0")}<span style={{ fontSize: 11 }}>s</span></div>
        </div>
      </FadeIn>
    </div>
    <div style={{ padding: "5px 14px", borderTop: "1px solid var(--nv-line)", display: "flex", gap: 6, justifyContent: "flex-end" }}>
      <BarBtn onClick={onClose}>↵ DECLINE</BarBtn>
      <BarBtn onClick={onAccept} accent>▶ ACCEPT · BEGIN HANDSHAKE</BarBtn>
    </div>
  </>);
}
function OutgoingPanel({ user, onCancel }) {
  const t = useTime();
  return (<>
    <div style={{ padding: "4px 8px", borderBottom: "1px solid var(--nv-cyan)", background: "rgba(0,144,255,0.08)" }}>
      <div className="nv-eyebrow" style={{ fontSize: 8, color: "var(--nv-cyan)" }}>◇ OUTGOING · WAITING FOR RESPONSE</div>
    </div>
    <div style={{ padding: "20px 14px", flex: 1, display: "flex", flexDirection: "column", alignItems: "center", justifyContent: "center", gap: 12 }}>
      <PulseRing size={64} color="var(--nv-cyan)" period={1400} />
      <div className="nv-display" style={{ fontSize: 24 }}>challenging <span style={{ color: "var(--nv-cyan)" }}>{user.name}</span></div>
      <Typewriter text={`pinging ${user.region.toLowerCase()}.relay → ${user.ping}ms · awaiting accept...`} cps={28} style={{ fontFamily: "var(--nv-mono)", fontSize: 10, color: "var(--nv-dim)" }} />
      <div className="nv-num" style={{ fontSize: 14, color: "var(--nv-faint)" }}>elapsed {t.toFixed(1)}s</div>
    </div>
    <div style={{ padding: "5px 14px", borderTop: "1px solid var(--nv-line)", display: "flex", justifyContent: "flex-end" }}>
      <BarBtn onClick={onCancel}>× CANCEL</BarBtn>
    </div>
  </>);
}
function HandshakePanel({ user, onDone }) {
  const rtt = useTelemetry({ base: user.ping, jitter: 0.7, drift: 0.4, hz: 12, samples: 80 });
  const [sync, setSync] = aS(0);
  aE(() => {
    if (sync >= 100) { const t = setTimeout(onDone, 600); return () => clearTimeout(t); }
    const id = setTimeout(() => setSync(s => Math.min(100, s + 8 + Math.random() * 14)), 220);
    return () => clearTimeout(id);
  }, [sync]);
  return (<>
    <div style={{ padding: "4px 8px", borderBottom: "1px solid var(--nv-cyan)", background: "rgba(0,144,255,0.08)" }}>
      <div className="nv-eyebrow" style={{ fontSize: 8, color: "var(--nv-cyan)" }}>◇ P2P · HOLEPUNCH · NEGOTIATING</div>
    </div>
    <div style={{ padding: "8px 14px", flex: 1, display: "grid", gridTemplateColumns: "1fr 110px 1fr", gap: 6, alignItems: "center" }}>
      <PeerCardV2 name="sloth" tag="#7421" elo={1842} char="ANONYM" region="JP" side="L" />
      <div style={{ textAlign: "center" }}>
        <div className="nv-eyebrow" style={{ color: "var(--nv-cyan)", fontSize: 8 }}>SYN/ACK</div>
        <div className="nv-display" style={{ fontSize: 26, color: "var(--nv-cyan)" }}><LiveNum value={rtt[rtt.length-1]} prec={0} /><span style={{ fontSize: 14 }}>ms</span></div>
        <OrbitDots w={110} h={36} count={18} color="var(--nv-cyan)" speed={0.9} amp={9} />
      </div>
      <PeerCardV2 name={user.name} tag="" elo={1730} char="MURAKUMO" region={user.region} side="R" />
    </div>
    <div style={{ padding: "5px 14px" }}>
      <div className="nv-eyebrow" style={{ fontSize: 8, marginBottom: 2 }}>SYNC RATIO</div>
      <div style={{ height: 6, background: "var(--nv-line)", position: "relative" }}>
        <div style={{ position: "absolute", left: 0, top: 0, bottom: 0, width: `${sync}%`, background: "var(--nv-cyan)", boxShadow: "0 0 8px var(--nv-cyan)", transition: "width 200ms" }} />
      </div>
      <div className="nv-data" style={{ fontSize: 9, color: "var(--nv-cyan)", marginTop: 2 }}>{sync.toFixed(1)}% · seed 0xA01F · crc match</div>
    </div>
    <div style={{ borderTop: "1px solid var(--nv-line)", padding: "4px 8px" }}>
      <div className="nv-label" style={{ fontSize: 7 }}>RTT · live</div>
      <PingGraph buf={rtt} w={400} h={24} color="var(--nv-cyan)" max={Math.max(...rtt) * 1.2} />
    </div>
  </>);
}
function BootingPanel({ user, onDone }) {
  const [step, setStep] = aS(0);
  const lines = [
    "loading rom · KOF98.bin · 0x6C9000",
    "applying patches · rollback-8f · delay-3f",
    "binding inputs · player1 ← KEYBOARD · player2 ← REMOTE",
    "calibrating audio · 48kHz · 64f buffer",
    "spawning emulator process · pid 0x4F3A",
    "stage select · T-EAST · seed 0xA01F",
    "ready · handing off",
  ];
  aE(() => {
    if (step >= lines.length) { const t = setTimeout(onDone, 400); return () => clearTimeout(t); }
    const t = setTimeout(() => setStep(s => s + 1), 380);
    return () => clearTimeout(t);
  }, [step]);
  return (<>
    <div style={{ padding: "4px 8px", borderBottom: "1px solid var(--nv-amber)", background: "rgba(255,185,56,0.08)" }}>
      <div className="nv-eyebrow" style={{ fontSize: 8, color: "var(--nv-amber)" }}>▶ BOOTING · vs {user.name}</div>
    </div>
    <div style={{ padding: "8px 14px", flex: 1, fontFamily: "var(--nv-mono)", fontSize: 10, lineHeight: 1.7, position: "relative" }}>
      <Stagger step={120} dur={300}>
        {lines.slice(0, step).map((l,i)=>(
          <div key={i}><span style={{ color: "var(--nv-faint)" }}>[boot]</span> <span style={{ color: "var(--nv-ink2)" }}>{l}</span> <span style={{ color: "var(--nv-phos)" }}>OK</span></div>
        ))}
      </Stagger>
      {step < lines.length && (
        <div style={{ marginTop: 4 }}><span style={{ color: "var(--nv-amber)" }}>[boot]</span> <Typewriter text={lines[step] || ""} cps={50} /></div>
      )}
    </div>
    <div style={{ padding: "4px 14px", display: "flex", alignItems: "center", gap: 8 }}>
      <div style={{ flex: 1, height: 5, background: "var(--nv-line)", position: "relative" }}>
        <div style={{ position: "absolute", left: 0, top: 0, bottom: 0, width: `${(step/lines.length)*100}%`, background: "var(--nv-amber)", transition: "width 200ms" }} />
      </div>
      <span className="nv-num" style={{ color: "var(--nv-amber)", fontSize: 11 }}>{Math.min(100, Math.round((step/lines.length)*100))}%</span>
    </div>
  </>);
}
function PostPanel({ user, onRematch, onQuit }) {
  return (<>
    <div style={{ padding: "4px 8px", borderBottom: "1px solid var(--nv-orange)", background: "rgba(255,90,60,0.08)" }}>
      <div className="nv-eyebrow nv-acc" style={{ fontSize: 8 }}>◆ MATCH · ENDED · GG</div>
    </div>
    <div style={{ padding: "10px 14px", flex: 1, display: "grid", gridTemplateColumns: "1fr 1fr", gap: 14 }}>
      <div>
        <div className="nv-eyebrow" style={{ fontSize: 8 }}>FINAL</div>
        <div className="nv-display" style={{ fontSize: 36, lineHeight: 1.0 }}>3<span style={{ color: "var(--nv-faint)" }}>:</span><span style={{ color: "var(--nv-magenta)" }}>2</span></div>
        <div className="nv-data" style={{ fontSize: 9, color: "var(--nv-dim)", marginTop: 4 }}>vs {user.name} · BO5 · T-EAST</div>
        <hr className="nv-hr" style={{ margin: "8px 0" }} />
        <NvRow k="ELO ∆" acc>+18 → 1860</NvRow>
        <NvRow k="STREAK">+6 🔥</NvRow>
      </div>
      <div style={{ border: "1px solid var(--nv-line)", padding: 8 }}>
        <div className="nv-eyebrow" style={{ fontSize: 8 }}>FRAME STATS</div>
        <hr className="nv-hr" style={{ margin: "4px 0" }} />
        <NvRow k="ROLLBACKS">142</NvRow>
        <NvRow k="AVG RTT">{user.ping}.4ms</NvRow>
        <NvRow k="JITTER">±0.6ms</NvRow>
        <NvRow k="DROPS">0</NvRow>
        <NvRow k="DESYNC">none</NvRow>
      </div>
    </div>
    <div style={{ padding: "5px 14px", borderTop: "1px solid var(--nv-line)", display: "flex", gap: 6, justifyContent: "flex-end" }}>
      <BarBtn onClick={onQuit}>× BACK TO LOBBY</BarBtn>
      <BarBtn onClick={onQuit}>↻ SAVE REPLAY</BarBtn>
      <BarBtn onClick={onRematch} accent>▶ REMATCH</BarBtn>
    </div>
  </>);
}
function DesyncPanel({ user, onResync, onAbort }) {
  return (<>
    <div style={{ padding: "4px 8px", borderBottom: "1px solid var(--nv-magenta)", background: "rgba(192,104,240,0.1)" }}>
      <div className="nv-eyebrow" style={{ fontSize: 8, color: "var(--nv-magenta)" }}>⚠ DESYNC · CONNECTION DEGRADED</div>
    </div>
    <div style={{ padding: "16px 14px", flex: 1, display: "flex", flexDirection: "column", justifyContent: "center", alignItems: "center", gap: 8 }}>
      <div className="nv-display" style={{ fontSize: 26, color: "var(--nv-magenta)" }}>STATE MISMATCH</div>
      <div className="nv-data" style={{ fontSize: 10, color: "var(--nv-dim)", textAlign: "center" }}>frame 4892 · crc local 0xA01F · remote 0x6F2E<br/>peer {user.name} · last ack 1.2s ago</div>
      <NvWarn>attempt resync or abort match · ELO will not be affected if aborted within 5s</NvWarn>
    </div>
    <div style={{ padding: "5px 14px", borderTop: "1px solid var(--nv-line)", display: "flex", gap: 6, justifyContent: "flex-end" }}>
      <BarBtn onClick={onAbort}>× ABORT MATCH</BarBtn>
      <BarBtn onClick={onResync} accent>↻ ATTEMPT RESYNC</BarBtn>
    </div>
  </>);
}

// ═════════════════════════════════════════════════════════════
// CHALLENGER WIDGET (legacy, kept for compatibility) ───────────
// ═════════════════════════════════════════════════════════════
function ChallengerWidget({ user, onClose, onAccept }) {
  const [min, setMin] = aS(false);
  const [t, setT] = aS(20);
  aE(() => {
    if (t <= 0) return onClose();
    const id = setTimeout(() => setT(x => x - 1), 1000);
    return () => clearTimeout(id);
  }, [t]);

  if (!user) return null;
  if (min) {
    // tiny pill bottom-right; chat keeps going underneath
    return (
      <div style={{
        position: "absolute", right: 158, bottom: 6, width: 168, zIndex: 50,
        border: "1px solid var(--nv-orange)",
        background: "rgba(10,6,18,0.95)", padding: "4px 6px",
        display: "grid", gridTemplateColumns: "12px 1fr auto auto", gap: 4, alignItems: "center",
        boxShadow: "0 0 16px rgba(255,90,60,0.4)",
      }}>
        <FlagDot region={user.region} />
        <div style={{ minWidth: 0 }}>
          <div className="nv-data" style={{ fontSize: 8, color: "var(--nv-orange)", letterSpacing: "0.1em" }}>CHALLENGE · {t}s</div>
          <div className="nv-data" style={{ fontSize: 9, color: "var(--nv-ink)", whiteSpace: "nowrap", overflow: "hidden", textOverflow: "ellipsis" }}>{user.name} · {user.ping}ms</div>
        </div>
        <div onClick={onAccept} style={{ padding: "2px 4px", border: "1px solid var(--nv-phos)", color: "var(--nv-phos)", fontFamily: "var(--nv-mono)", fontSize: 8, cursor: "pointer" }}>↵</div>
        <div onClick={() => setMin(false)} style={{ padding: "2px 4px", color: "var(--nv-faint)", fontFamily: "var(--nv-mono)", fontSize: 9, cursor: "pointer" }}>▢</div>
      </div>
    );
  }
  return (
    <div style={{
      position: "absolute", left: 64, top: 60, right: 168, bottom: 40, zIndex: 40,
      background: "rgba(10,6,18,0.97)", border: "1px solid var(--nv-orange)",
      boxShadow: "0 0 32px rgba(255,90,60,0.35), 0 0 0 100vmax rgba(0,0,0,0.55)",
      display: "flex", flexDirection: "column",
    }}>
      <div style={{ padding: "5px 8px", borderBottom: "1px solid var(--nv-orange)", display: "flex", alignItems: "center", justifyContent: "space-between", background: "rgba(255,90,60,0.08)" }}>
        <div className="nv-eyebrow nv-acc" style={{ fontSize: 8 }}>CHALLENGE · INCOMING REQUEST</div>
        <div style={{ display: "flex", gap: 4 }}>
          <div onClick={() => setMin(true)} className="nv-data" style={{ fontSize: 9, color: "var(--nv-faint)", cursor: "pointer", border: "1px solid var(--nv-line)", padding: "1px 5px" }}>− MIN</div>
          <div onClick={onClose} className="nv-data" style={{ fontSize: 9, color: "var(--nv-faint)", cursor: "pointer", border: "1px solid var(--nv-line)", padding: "1px 5px" }}>× CLOSE</div>
        </div>
      </div>
      <div style={{ padding: "10px 14px", flex: 1, display: "grid", gridTemplateColumns: "1fr 1fr", gap: 14, alignItems: "center" }}>
        <FadeIn delay={50} dur={400} from="x" dist={-12}>
          <div>
            <div className="nv-eyebrow" style={{ color: "var(--nv-cyan)", fontSize: 8 }}>CHALLENGER</div>
            <div className="nv-display" style={{ fontSize: 22, marginTop: 2 }}>{user.name}</div>
            <div style={{ display: "flex", gap: 6, alignItems: "center", marginTop: 2 }}>
              <FlagDot region={user.region} />
              <span className="nv-data" style={{ fontSize: 9, color: "var(--nv-dim)" }}>{user.region} · ELO {user.elo || 1730}</span>
            </div>
            <hr className="nv-hr" style={{ margin: "8px 0" }} />
            <NvRow k="PING">{user.ping}ms · stable</NvRow>
            <NvRow k="MAIN">{user.main || "MURAKUMO"}</NvRow>
            <NvRow k="W/L vs YOU">3 / 4</NvRow>
            <NvRow k="LAST SET">3d ago · L 1-2</NvRow>
          </div>
        </FadeIn>
        <FadeIn delay={150} dur={400} from="x" dist={12}>
          <div style={{ border: "1px solid var(--nv-line)", padding: 10 }}>
            <div className="nv-eyebrow" style={{ fontSize: 8 }}>RULESET · NEGOTIATED</div>
            <hr className="nv-hr" style={{ margin: "4px 0" }} />
            <NvRow k="FORMAT" acc>FT3 · BO5</NvRow>
            <NvRow k="ROLLBACK">8F</NvRow>
            <NvRow k="DELAY">3F</NvRow>
            <NvRow k="STAGE">RANDOM</NvRow>
            <hr className="nv-hr" style={{ margin: "4px 0" }} />
            <div className="nv-data" style={{ fontSize: 8, color: "var(--nv-faint)" }}>EXPIRES IN</div>
            <div className="nv-display" style={{ fontSize: 28, color: "var(--nv-orange)" }}>{String(t).padStart(2,"0")}<span style={{ fontSize: 11 }}>s</span></div>
          </div>
        </FadeIn>
      </div>
      <div style={{ padding: "6px 14px", borderTop: "1px solid var(--nv-line)", display: "flex", gap: 6, justifyContent: "flex-end" }}>
        <BarBtn onClick={onClose}>↵ DECLINE</BarBtn>
        <BarBtn onClick={() => setMin(true)}>▢ MIN · KEEP CHATTING</BarBtn>
        <BarBtn onClick={onAccept} accent>▶ ACCEPT · BEGIN HANDSHAKE</BarBtn>
      </div>
    </div>
  );
}

// ═════════════════════════════════════════════════════════════
// IN-GAME DOCK — game is running, app shrinks to a pill
// ═════════════════════════════════════════════════════════════
function InGameDock({ user, onEnd, onDesync }) {
  const t = useTime();
  const rtt = useTelemetry({ base: user?.ping || 60, jitter: 0.6, drift: 0.4, hz: 10, samples: 60 });
  const round = Math.min(5, 1 + Math.floor(t / 22));
  return (
    <div style={{
      position: "absolute", left: 64, right: 168, bottom: 24, top: 36,
      background: "rgba(2,1,5,0.92)", zIndex: 30,
      display: "flex", flexDirection: "column", alignItems: "center", justifyContent: "center", gap: 10,
      backdropFilter: "blur(2px)",
    }}>
      <div style={{ position: "absolute", inset: 0, opacity: 0.25, pointerEvents: "none" }}>
        <Lissajous w={428} h={388} a={3} b={4} color="rgba(0,144,255,0.55)" period={9000} />
      </div>
      <div className="nv-eyebrow" style={{ fontSize: 8, color: "var(--nv-amber)" }}>▶ GAME RUNNING · MIN TO TRAY</div>
      <div className="nv-display" style={{ fontSize: 32, lineHeight: 1.0 }}>
        <span style={{ color: "var(--nv-cyan)" }}>sloth</span>
        <span style={{ color: "var(--nv-faint)", margin: "0 8px" }}>vs</span>
        <span style={{ color: "var(--nv-orange)" }}>{user?.name}</span>
      </div>
      <div className="nv-data" style={{ fontSize: 9, color: "var(--nv-dim)" }}>BO5 · ROUND {round}/5 · T-EAST</div>
      <div style={{ width: 280 }}>
        <div className="nv-label" style={{ fontSize: 7 }}>RTT · live · {rtt[rtt.length-1].toFixed(1)}ms · jitter ±0.6</div>
        <PingGraph buf={rtt} w={280} h={20} color="var(--nv-cyan)" max={Math.max(...rtt)*1.3} />
      </div>
      <div style={{ display: "flex", gap: 5, marginTop: 4 }}>
        <BarBtn onClick={onDesync}>⚠ TRIGGER DESYNC</BarBtn>
        <BarBtn onClick={onEnd} accent>■ END MATCH · 3:2</BarBtn>
      </div>
      <div className="nv-num" style={{ fontSize: 10, color: "var(--nv-faint)" }}>elapsed {t.toFixed(1)}s · 142 rollbacks · 0 drops</div>
    </div>
  );
}

// Dev controls — invisible buttons in lower-right of right rail
function DevMatchControls({ onStep }) {
  const states = [["IN","incoming"],["HS","handshake"],["BT","booting"],["IG","ingame"],["PT","post"],["DS","desync"]];
  return (
    <div style={{ position: "absolute", right: 4, bottom: 22, zIndex: 50, display: "flex", gap: 2, opacity: 0.65 }}>
      {states.map(([k,s])=>(
        <div key={s} onClick={()=>onStep(s)} className="nv-data" style={{
          padding: "1px 3px", fontSize: 7, color: "var(--nv-faint)",
          border: "1px solid var(--nv-line)", background: "rgba(0,0,0,0.7)", cursor: "pointer",
        }}>{k}</div>
      ))}
    </div>
  );
}

// ═════════════════════════════════════════════════════════════
// APP SHELL
// ═════════════════════════════════════════════════════════════
function AppShell() {
  const games = [
    { id: "kof98", code: "KOF98",  short: "KING OF FIGHTERS '98",   tint: "linear-gradient(135deg,#5a2a85 30%, #ff3030)" },
    { id: "3s",    code: "SF3-3S", short: "SF III · 3rd STRIKE",     tint: "linear-gradient(135deg,#0040a0 30%, #4adacc)" },
    { id: "kof02", code: "KOF02",  short: "KOF 2002",                tint: "linear-gradient(135deg,#000 30%, #ff5a3c)" },
    { id: "k02p",  code: "KOF02P", short: "KOF 2002 PLUS",           tint: "linear-gradient(135deg,#3d1c5e 30%, #ffb938)" },
  ];
  const [view, setView] = aS("lobby");
  const [currentGame, setCurrentGame] = aS("kof98");
  const [challenger, setChallenger] = aS(null);
  // match state machine: idle | incoming | outgoing | handshake | booting | ingame | post | desync
  const [matchStage, setMatchStage] = aS("idle");
  const [matchUser, setMatchUser]   = aS(null);

  const game = games.find(g => g.id === currentGame) || games[0];

  // Right-rail user list (52 looking)
  const users = aM(() => [
    { name: "B2K (MVR)",       region: "MX", ping: 22 },
    { name: "GDK-TWK-KUMBIA",  region: "AR", ping: 145, in: "match" },
    { name: "ElRukoKofero",    region: "MX", ping: 28 },
    { name: "neftali.selene",  region: "MX", ping: 78 },
    { name: "DeathKaizer",     region: "BR", ping: 142 },
    { name: "elbebe",          region: "ES", ping: 18 },
    { name: "ALK_SaDen",       region: "ES", ping: 64 },
    { name: "Helikorn",        region: "DE", ping: 88 },
    { name: "~DanStroke~",     region: "FR", ping: 54 },
    { name: "[AFL]ChinitoJR",  region: "MX", ping: 110 },
    { name: "MVR-FC98.2002",   region: "MX", ping: 32 },
    { name: "SNEIDER3434",     region: "AR", ping: 156 },
    { name: "Jhonatan332",     region: "MX", ping: 92 },
    { name: "Soy el Bonus",    region: "ES", ping: 14 },
    { name: "Dmsilverio",      region: "BR", ping: 168 },
    { name: "purypury",        region: "DE", ping: 76 },
    { name: "lorraynecba",     region: "BR", ping: 138, in: "match" },
    { name: "Dnb^",            region: "ES", ping: 22 },
    { name: "semcombozueiro",  region: "BR", ping: 145 },
    { name: "perrolento2",     region: "MX", ping: 102 },
    { name: "Tonelli",         region: "AR", ping: 132 },
    { name: "M@rinho",         region: "BR", ping: 158 },
  ], []);

  const [chat, setChat] = aS([
    { ts: "02:38", who: "DONG_JACK_G_MAN", text: "looking for a kof98 set anyone?" },
    { ts: "02:39", who: "B2K (MVR)",       text: "down lemme finish this game" },
    { ts: "02:40", who: "system", sys: true, text: "ElRukoKofero joined channel" },
    { ts: "02:41", who: "elbebe",          text: "new patch dropped, frame data updated" },
    { ts: "02:41", who: "DeathKaizer",     text: "yo elbebe FT5?" },
    { ts: "02:42", who: "elbebe",          text: "claro vamonos" },
    { ts: "02:43", who: "system", sys: true, text: "elbebe vs DeathKaizer · room #6F2A · 18ms" },
    { ts: "02:44", who: "sloth#7421", me: true, text: "anyone wanna run a set on east stage" },
    { ts: "02:45", who: "ALK_SaDen",       text: "I'll join in 5" },
    { ts: "02:46", who: "purypury",        text: "ggs Helikorn that was clean" },
    { ts: "02:47", who: "system", sys: true, text: "EVENT REMINDER · BRASILEIRO 30 starts in 13d" },
  ]);

  const activeMatches = aM(() => [
    { a: "elbebe",         b: "DeathKaizer",   charA: "TERRY",     charB: "IORI",     colorA: "#a02020", colorB: "#5a2a85", scoreA: 1, scoreB: 0, firstTo: 3, format: "FT3", round: 2, time: "03:42", h2h: "12-9",  room: "#6F2A", ping: 18,  delay: 3, lead: "A", spectators: 14 },
    { a: "Helikorn",       b: "purypury",      charA: "KYO",       charB: "K'",       colorA: "#0040a0", colorB: "#c08000", scoreA: 2, scoreB: 2, firstTo: 5, format: "FT5", round: 5, time: "11:08", h2h: "33-31", room: "#A14B", ping: 78,  delay: 4, lead: "—", spectators: 38 },
    { a: "GDK-TWK-KUMBIA", b: "SNEIDER3434",   charA: "MAI",       charB: "ATHENA",   colorA: "#ff5a3c", colorB: "#5fff95", scoreA: 0, scoreB: 1, firstTo: 3, format: "FT3", round: 2, time: "01:55", h2h: "0-0",   room: "#B208", ping: 142, delay: 6, lead: "B", spectators: 4 },
    { a: "lorraynecba",    b: "Dmsilverio",    charA: "RYO",       charB: "TAKUMA",   colorA: "#a04020", colorB: "#3d1c5e", scoreA: 4, scoreB: 5, firstTo: 7, format: "FT7", round: 10,time: "22:14", h2h: "8-12",  room: "#D44C", ping: 56,  delay: 3, lead: "B", spectators: 62 },
    { a: "ALK_SaDen",      b: "Dnb^",          charA: "CHOI",      charB: "CHANG",    colorA: "#7a3070", colorB: "#306030", scoreA: 7, scoreB: 6, firstTo: 10,format: "FT10",round: 14,time: "31:02", h2h: "14-9",  room: "#E901", ping: 24,  delay: 2, lead: "A", spectators: 91 },
    { a: "MVR-FC98.2002",  b: "elbebe",        charA: "ROBERT",    charB: "TERRY",    colorA: "#0080d0", colorB: "#a02020", scoreA: 1, scoreB: 1, firstTo: 3, format: "FT3", round: 3, time: "06:18", h2h: "4-6",   room: "#F128", ping: 32,  delay: 3, lead: "—", spectators: 8 },
  ], []);

  const handleChallenge = (u) => {
    setMatchUser({ ...u, elo: 1700 + Math.floor(Math.random() * 300), main: ["MURAKUMO","KAGURA","K'","MAI"][Math.floor(Math.random()*4)] });
    setMatchStage("outgoing");
  };
  const acceptChallenge = () => { setChallenger(null); };
  const compact = matchStage === "ingame";

  return (
    <div style={{ position: "relative", width: 640, height: 448, background: "var(--nv-bg)", color: "var(--nv-ink)", fontFamily: "var(--nv-display)", overflow: "hidden" }}>
      {/* faint EnergyField backdrop, very subtle */}
      <div style={{ position: "absolute", inset: 0, opacity: 0.18, pointerEvents: "none" }}>
        <EnergyField w={640} h={448} count={36} color="rgba(255,106,0,0.4)" opacity={0.4} />
      </div>
      <div style={{ position: "relative", display: "grid", gridTemplateColumns: "48px 1fr 154px", gridTemplateRows: "30px 1fr 18px", height: "100%" }}>
        {/* left rail spans all rows */}
        <div style={{ gridRow: "1 / span 3" }}>
          <LeftRail games={games} currentGame={currentGame} onPickGame={setCurrentGame}
            onSettings={() => setView("cfg")} onMap={() => setView("map")}
            view={view} setView={setView} />
        </div>
        {/* topbar */}
        <div style={{ gridColumn: "2 / span 2" }}>
          <TopBar game={game} view={view} setView={setView}
            onTest={() => setView("map")}
            onTrain={() => setView("profile")}
            onBoot={() => alert("BOOT · OFFLINE · " + game.code)} />
        </div>
        {/* main */}
        <div style={{ gridColumn: 2, gridRow: 2, overflow: "hidden", borderRight: view === "map" || view === "cfg" ? "none" : "none" }}>
          {view === "browse"   && <BrowseView   onPickGame={(id) => { setCurrentGame(id); setView("lobby"); }} />}
          {view === "lobby"    && <LobbyView    game={game} chat={chat} />}
          {view === "replays"  && <ReplaysView  />}
          {view === "rankings" && <RankingsView game={game} />}
          {view === "stats"    && <StatsView    />}
          {view === "events"   && <EventsView   />}
          {view === "profile"  && <StatsView    />}
          {view === "cfg"      && <SettingsView />}
          {view === "map"      && <ControllerMapper onClose={() => setView("lobby")} />}
        </div>
        {/* right rail */}
        <div style={{ gridColumn: 3, gridRow: 2, overflow: "hidden" }}>
          <RightRail users={users} activeMatches={activeMatches} onPickUser={handleChallenge} />
        </div>
        {/* status bar */}
        <div style={{ gridColumn: "2 / span 2", borderTop: "1px solid var(--nv-line)", padding: "0 8px", display: "flex", alignItems: "center", gap: 10, fontFamily: "var(--nv-mono)", fontSize: 8, color: "var(--nv-faint)", background: "rgba(0,0,0,0.5)" }}>
          <span><span className="nv-acc">●</span> ONLINE · STUN.RELAY-TOKYO</span>
          <span>14ms</span>
          <span style={{ color: "var(--nv-phos)" }}>NAT · OPEN</span>
          <span>SLOTS 14/16</span>
          <span style={{ flex: 1, textAlign: "right" }}>FM2K_ROLLBACK · 0.2.8 · build 0xA01F</span>
        </div>
      </div>
      {/* Match flow overlays */}
      <MatchFlow stage={matchStage} user={matchUser}
        onAdvance={(s) => setMatchStage(s)}
        onCancel={() => { setMatchStage("idle"); setMatchUser(null); }}
        onClose={() => { setMatchStage("idle"); setMatchUser(null); }} />
      {/* Compact in-game pill — game is running, app shrinks out of the way */}
      {compact && <InGameDock user={matchUser} onEnd={() => setMatchStage("post")} onDesync={() => setMatchStage("desync")} />}
      {/* Dev: keyboard shortcuts to step through match flow */}
      <DevMatchControls
        onStep={(s) => { if (s === "incoming") { setMatchUser({ name: "GDK-TWK-KUMBIA", region: "AR", ping: 64 }); } setMatchStage(s); }} />
      {/* Legacy challenger overlay */}
      {challenger && <ChallengerWidget user={challenger} onClose={() => setChallenger(null)} onAccept={acceptChallenge} />}
    </div>
  );
}

Object.assign(window, { AppShell });
