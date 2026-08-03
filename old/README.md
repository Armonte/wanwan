# old/ -- reference only, NOT built

Nothing in this directory is compiled. It is not referenced by any
`CMakeLists.txt`, build script, or test. ~19,500 lines across 22 files.

Kept because it is useful to read, not because it runs:

- `cccaster/` -- the CCCaster rollback implementation this project's
  architecture was originally informed by. Handy when comparing approaches to
  save-state layout, spectator handling, or the DLL-injection lifecycle.
- `hook.cpp`, `hook_manager.cpp`, `simple_input_hooks.cpp`, `game_loop.cpp`,
  `initGame.cpp`, `ctx.cpp` -- superseded early hook attempts, before the
  current `FM2KHook/src/` structure existed.
- `sdl3_context.cpp`, `sdl3_directdraw_compat_new.cpp`,
  `surface_management.cpp` -- earlier SDL3/DirectDraw takeover experiments.
- `FM2K_LauncherUI_ui_branch_BROKENUI_workingIPC.cpp` -- exactly what the name
  says; kept for the IPC half.

If you are looking for the live equivalents: the hook is `FM2KHook/src/`, the
launcher is `launcher/`, and the render path is `launcher/render/` plus
`FM2KHook/src/hooks/hooks_render.cpp`.

This file exists so the periodic dead-code audit stops re-discovering the
directory. If something here is genuinely worth reviving, move it out and wire
it into a build; otherwise leave it be.
