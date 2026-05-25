# Critical inline rules (every session reads these)

Extracted from CLAUDE.md 2026-05-24. **Authoritative.** CLAUDE.md links here.

Rules are organized loosely by category but every rule applies whenever the
matching condition is met. Read them all before touching anything.

## Style

- **No emoji in any file, ever.** Unicode punctuation (em/en-dash, ellipsis) OK; pictographic emoji not. Full scope: `memory/feedback_no_emoji_in_files.md`.
- **No wall-clock time projections.** Describe complexity in code dimensions (subsystems, files, parity gates, soak windows), not time.

## Grounding (every cited fact)

- **Grep before citing file:line.** Every file:line in any output must be verified at write-time. Symbols stable; line numbers drift. Rationale: `memory/brainstorm_code_grounding_lesson.md`. Plan-stage: `.claude/skills/adversarial-plan-review.md`.
- **Negative claims need opposite-direction grep.** "X is NOT consumed by Y" requires grep'ing Y, not the obvious-named consumer. See `memory/feedback_data_flow_audit_asymmetry.md`.

## Build / deploy

- **Build:** ALWAYS `--config RelWithDebInfo`. Release crashes with `GL_INVALID_ENUM`.
- **Full relink before deploy** when load-bearing functions change: `rm build64/RelWithDebInfo/mc2.exe` (+ changed `.obj`) before `cmake --build`, or `--clean-first`. Incremental leaks stale linkage when inline funcs / templates / static state change. Class-layout changes: `memory/feedback_class_layout_change_needs_clean_first.md`.
- **Deploy:** NEVER `cp -r`. ALWAYS `cp -f` per file + `diff -q`. `cp -r` silently fails on Windows/MSYS2.
- **Shaders deploy in lockstep with exe.** Any slice touching a shader MUST redeploy the shader tree, not just mc2.exe. See `memory/shader_exe_deploy_lockstep.md`.
- **Git:** NEVER push to `alariq/mc2` origin. All work is local.

## Shaders / GL

- **Shader `#version`:** Never in shader files. Pass `"#version 430\n"` as prefix to `makeProgram()` (4.3 for SSBO / std430).
- **Uniform API:** `setFloat` / `setInt` BEFORE `apply()`, direct `glUniform*` AFTER. `apply()` flushes dirty uniforms.
- **GL_FALSE for terrainMVP:** direct-uploaded row-major matrices use `GL_FALSE`. Material cache uses `GL_TRUE`. The `gamecam.cpp` comment claiming `GL_TRUE` is wrong; do not "fix" it.
- **Explicit-program uniform upload:** any GOS API or helper that takes a `GLuint program` and calls `glGetUniformLocation(program, ...)` MUST upload via `glProgramUniformMatrix4fv(program, loc, ...)` (or its sibling for the type). `glUniformMatrix4fv(loc, ...)` uploads to the currently-bound program (`glUseProgram` state), NOT the named one — silent wrong-shader upload bug. Engine uses GL 4.5; the explicit-program family is unconditionally available. Full: `memory/glprogramuniform_vs_gluniform_explicit_program_trap.md`.
- **GLSL macros do NOT inherit C++ build flags.** `-DMY_FLAG` in `CMAKE_CXX_FLAGS` reaches only `.cpp` compilation. To gate a GLSL `#ifdef`, extend the `makeProgram()` prefix at C++ level: `prefix += "#define MY_FLAG 1\n"` inside `#ifdef MY_FLAG`. Verify by dumping compiled shader source before `glCompileShader`. Full: `memory/glsl_preprocessor_does_not_inherit_cpp_build_flags.md`.
- **Shader hot-reload fails silently:** bad compile = old shader stays active. Always check console after editing.

## Architecture / change discipline

- **Vulkan-prep:** new GPU-resource code uses explicit device-mediated binding (`device.bindVertexBuffer(vb)`, NOT `vb.bind()`); assume zero implicit cross-call GL state. PREP not a port. Full: `memory/vulkan_prep_explicit_device_discipline.md`.
- **Change discipline:** don't touch what you don't have to (every touch has blast radius); when you must, bring it to modern standard. Standalone cleanup slices need a blocking/debt justification. Full: `memory/minimal_touch_modern_when_touched.md`.
- **C++17 features:** project standard is C++17 since 2026-05-24 (commit `5c03835`). For which features are allowed / cautioned / avoided, see `docs/cxx17-coding-rules.md`. Rule: language upgrade != feature spree; minimal touch + measured wins.

## Editor discipline

- **Editor is a GPU-only test bed.** `EditRel.exe` renders ONLY the 3D world. It has no menus, FMV cinematics, briefing maps, or mode-switch state machines — all legacy CPU paths in `mc2.exe` exist for those other modes, and the editor needs none. Adding a CPU fallback to editor TUs (or forcing the editor onto a legacy path via env gate) is wrong-direction and must be rejected at review time. Correct fix for "X breaks in editor" is to make X work on the GPU path the game uses. 2D UI overlays (selection brackets, brush gizmos, BuildingLink arrows, ImGui, MFC widgets) are NOT in scope of this ban — those are immediate-mode UI, same category as the game's HUD. Full rationale + historical context + env-gate polarity table: `memory/editor_is_gpu_only_testbed.md`.
