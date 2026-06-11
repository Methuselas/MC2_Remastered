# Tacmap Formation Line v1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Default-off F6 tactical-overview command: press L, drag a line on terrain, selected squad receives individual move orders to evenly spaced slots along the line.

**Architecture:** State machine + slot math live in `code/tacticaloverview.*` (engine glue class that already owns F6). Input polled in the `code/mechcmd2.cpp` main loop next to the existing F6/F7/F8 edge-latches (mission command table is known-unreliable). Ghost line + pips drawn in `code/controlgui.cpp::ControlGui::render` next to the sensor/weapon overlays, using the proven projection rules. Orders issued per-mover via `pMover->handleTacticalOrder(tacOrder)` directly, bypassing `handleOrders`/`calcMoveGoals` group clustering.

**Tech Stack:** C++17 (per docs/cxx17-coding-rules.md), gos_* immediate-mode 2D draw, env-var feature gate.

**Spec:** `docs/superpowers/specs/2026-06-11-tacmap-formation-line-v1-design.md`

**Worktree:** `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev` — ALL paths below relative to it. Build/test MUST run from this cwd (bare `cmake --build build64` from wrong cwd silently builds stale ROOT build64).

**Verification model:** This codebase has NO unit-test framework for game code. Per-task gate = clean build; slice gate = tier1 smoke 5/5. Interactive acceptance is user-run at the end.

**Build command (verbatim):**
```powershell
cd A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev
& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --target mc2 --config RelWithDebInfo
```
Expected: exit 0, `mc2.exe` relinked. Do NOT pipe through `tail` (masks exit code).

**Smoke command (verbatim):**
```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --keep-logs
```
Expected: exit 0, 5/5 pass.

## Reference anchors (verified in worktree 2026-06-11; grep to confirm before editing)

- F6/F7/F8 edge-latch input site: `code/mechcmd2.cpp` ~line 2233 (`// Tactical Overview toggle (F6)` block inside `DoGameLogic` after `userInput->update()`).
- `TacticalOverview` class: `code/tacticaloverview.h` (g_tacticalOverview global, `active()` method).
- Single-mover order pattern: `code/missiongui.cpp:3651-3667` (`doMove`: `TacticalOrder` init/initWayPath/pack) and `:2382` (`pMover->handleTacticalOrder(tacOrder)`).
- Selection enumeration: `Team::home->getRosterSize()` / `getMover(i)` / `isSelected()` / `getCommander()->getId() == Commander::home->getId()` (`code/missiongui.cpp:3676-3681` pattern).
- Overlay draw pattern: `code/controlgui.cpp:321-417` (`drawSensorRing`/`renderSensorView`) — projectModernClipGL + viewport mapping + gos_DrawLines; `csgThickSeg` at `:563` for thick segments; render hook site `:706-728` (`ControlGui::render`, `g_sensorViewOn` / `g_tacticalOverview.active()` blocks).
- Screen→world: `Camera::screenToGroundPlaneApprox(long sx, long sy, Stuff::Vector3D& outWorld)` `mclib/camera.h:1009` (O(1), ground-plane approx — exactly what spec wants; do NOT use `inverseProject`, it is the legacy 40k-quad scanner).
- Mouse API (`mclib/userinput.h`): `getMouseX()/getMouseY()` (:413), `getMouseLeftButtonState()` (:460, compare `MC2_MOUSE_DOWN`), `leftMouseReleased()` (:584).
- Keys: `KEY_L = 'L'`, `KEY_ESCAPE = 0x1B` (`GameOS/include/gameos.hpp:1241/:1204`); edge-latch pattern = static `bool was` like F6 block.
- `userInput`, `eye` (Camera*), `Team::home`, `Commander::home` are globals available in both mechcmd2.cpp and controlgui.cpp.
- Rules: NO emoji in code/comments (docs/critical_inline_rules.md). Match existing tab indentation in each file.

---

### Task 1: Formation-line state + slot math in tacticaloverview.*

**Files:**
- Modify: `code/tacticaloverview.h` (add FormationLine API to class)
- Modify: `code/tacticaloverview.cpp` (impl)

- [ ] **Step 1: Add public API + state to `code/tacticaloverview.h`**

Inside `class TacticalOverview`, after the release-suppression block (after line ~50), add:

```cpp
    // --- Formation line (MC2_TACMAP_FORMATION_LINE, default OFF) ---
    // Draw a line in F6 overview; selected squad gets one move order per
    // evenly spaced slot. State machine: IDLE -> ARMED (L) -> DRAGGING (LMB)
    // -> issue on release. Esc / RMB / exiting F6 cancels.
    enum FormationLineState { FL_IDLE = 0, FL_ARMED, FL_DRAGGING };
    static bool formationLineEnabled();        // env gate, cached
    void flOnHotkeyL();                        // L pressed while F6 active
    void flOnCancel();                         // Esc / right-click / F6 exit
    void flOnDragStart( const Stuff::Vector3D& worldStart );
    void flOnDragMove( const Stuff::Vector3D& worldEnd );
    void flOnRelease();                        // issue orders, back to IDLE
    FormationLineState flState() const { return flState_; }
    const Stuff::Vector3D& flStart() const { return flStart_; }
    const Stuff::Vector3D& flEnd()   const { return flEnd_; }
    // Evenly spaced slots start->end inclusive; N==1 -> midpoint. Returns count.
    int flComputeSlots( Stuff::Vector3D* outSlots, int maxSlots ) const;
```

And in the private section, after `bool suppressRelease_ = false;`:

```cpp
    FormationLineState flState_ = FL_IDLE;
    Stuff::Vector3D    flStart_;
    Stuff::Vector3D    flEnd_;
    static const int   kFlMaxMovers = 32;
    void*              flMovers_[kFlMaxMovers];   // MoverPtr snapshot (void* keeps header engine-free)
    int                flMoverCount_ = 0;
```

`tacticaloverview.h` includes only `tacticaloverview_state.h` — keep it that way. `Stuff::Vector3D` needs a forward include: check top of file; if `Stuff` types are not visible, add `#include <stuff/vector.hpp>` matching whatever `tacticaloverview.cpp` already includes (grep `#include` in tacticaloverview.cpp first and reuse its vector header). If pulling Stuff into the header is ugly, store floats (`flStartX_/flStartY_/flStartZ_` etc.) instead — executor's choice, keep header light.

- [ ] **Step 2: Implement state machine + slot math in `code/tacticaloverview.cpp`**

Add at file scope (match the file's existing env-cache pattern used by `enabled()`):

```cpp
// Formation line env gate (MC2_TACMAP_FORMATION_LINE=1), cached on first call.
bool TacticalOverview::formationLineEnabled()
{
    static int cached = -1;
    if ( cached < 0 )
    {
        const char* v = getenv( "MC2_TACMAP_FORMATION_LINE" );
        cached = ( v && v[0] == '1' ) ? 1 : 0;
    }
    return cached == 1;
}

void TacticalOverview::flOnHotkeyL()
{
    if ( !formationLineEnabled() || !state_.active() )
        return;
    if ( flState_ == FL_IDLE )
        flState_ = FL_ARMED;
    else
        flState_ = FL_IDLE;     // L again disarms
}

void TacticalOverview::flOnCancel()
{
    flState_ = FL_IDLE;
    flMoverCount_ = 0;
}

void TacticalOverview::flOnDragStart( const Stuff::Vector3D& worldStart )
{
    if ( flState_ != FL_ARMED )
        return;
    flStart_ = worldStart;
    flEnd_   = worldStart;
    flMoverCount_ = 0;          // snapshot filled by caller via flAddMover
    flState_ = FL_DRAGGING;
}

void TacticalOverview::flOnDragMove( const Stuff::Vector3D& worldEnd )
{
    if ( flState_ == FL_DRAGGING )
        flEnd_ = worldEnd;
}

int TacticalOverview::flComputeSlots( Stuff::Vector3D* outSlots, int maxSlots ) const
{
    int n = flMoverCount_;
    if ( n <= 0 || maxSlots <= 0 )
        return 0;
    if ( n > maxSlots ) n = maxSlots;
    if ( n == 1 )
    {
        outSlots[0].x = ( flStart_.x + flEnd_.x ) * 0.5f;
        outSlots[0].y = ( flStart_.y + flEnd_.y ) * 0.5f;
        outSlots[0].z = ( flStart_.z + flEnd_.z ) * 0.5f;
        return 1;
    }
    for ( int i = 0; i < n; i++ )
    {
        float t = (float)i / (float)( n - 1 );
        outSlots[i].x = flStart_.x + ( flEnd_.x - flStart_.x ) * t;
        outSlots[i].y = flStart_.y + ( flEnd_.y - flStart_.y ) * t;
        outSlots[i].z = flStart_.z + ( flEnd_.z - flStart_.z ) * t;
    }
    return n;
}
```

The mover snapshot needs a setter; add to the header public block:

```cpp
    void flSetMovers( void* const* movers, int n );
    int  flMoverCount() const { return flMoverCount_; }
    void* flMover( int i ) const { return ( i >= 0 && i < flMoverCount_ ) ? flMovers_[i] : 0; }
```

and impl:

```cpp
void TacticalOverview::flSetMovers( void* const* movers, int n )
{
    if ( n > kFlMaxMovers ) n = kFlMaxMovers;
    for ( int i = 0; i < n; i++ )
        flMovers_[i] = movers[i];
    flMoverCount_ = n;
}
```

`flOnRelease()` is implemented in Task 3 (order issuance lives game-side). For now stub it:

```cpp
void TacticalOverview::flOnRelease()
{
    // Order issuance wired in controlgui/mechcmd2 (Task 3); state reset here.
    flState_ = FL_IDLE;
}
```

Note: do NOT reset `flMoverCount_` in `flOnRelease` — Task 3's issuer reads the snapshot after calling it. Reset only in `flOnCancel`.

- [ ] **Step 3: Also cancel formation line on F6 exit**

In `tacticaloverview.cpp` find `onHotkey()` (the F6 snap-toggle). After the existing toggle logic, add:

```cpp
    // Formation line never survives an overview toggle.
    flOnCancel();
```

- [ ] **Step 4: Build**

Run the verbatim build command. Expected: exit 0.

- [ ] **Step 5: Commit**

```bash
git add code/tacticaloverview.h code/tacticaloverview.cpp
git commit -m "feat(tacmap): formation line state machine + slot math (MC2_TACMAP_FORMATION_LINE, default OFF)"
```

---

### Task 2: Input wiring in mechcmd2.cpp

**Files:**
- Modify: `code/mechcmd2.cpp` (the F6/F7/F8 edge-latch block in `DoGameLogic`, ~line 2233)

- [ ] **Step 1: Add L / mouse / Esc polling after the F8 block**

Directly after the `// Weapon view toggle (F8).` block, add (tabs, matching file style):

```cpp
			// Formation line (MC2_TACMAP_FORMATION_LINE): L arms while F6
			// overview active; LMB drag draws; release issues orders (handled
			// in ControlGui::render where eye/selection live); Esc/RMB cancels.
			if ( TacticalOverview::formationLineEnabled() )
			{
				static bool s_flLWas = false;
				bool lDown = userInput->getKeyDown( KEY_L )
					&& !userInput->ctrl() && !userInput->alt() && !userInput->shift();
				if ( lDown && !s_flLWas )
					g_tacticalOverview.flOnHotkeyL();
				s_flLWas = lDown;

				if ( g_tacticalOverview.flState() != TacticalOverview::FL_IDLE )
				{
					static bool s_flEscWas = false;
					bool escDown = userInput->getKeyDown( KEY_ESCAPE );
					if ( escDown && !s_flEscWas )
						g_tacticalOverview.flOnCancel();
					s_flEscWas = escDown;
					if ( userInput->rightMouseReleased() )
						g_tacticalOverview.flOnCancel();
				}
			}
```

Notes:
- `KEY_L` may collide with an existing mission hotkey ONLY while ARMED-capable; gate is `flOnHotkeyL()` itself (no-ops unless F6 active + env on), so global `L` behavior is unchanged when overview is off. That satisfies "do not steal L globally".
- `tacticaloverview.h` is already included by mechcmd2.cpp (the F6 block uses `g_tacticalOverview`); verify with grep `#include "tacticaloverview.h"` and add if missing.
- Mouse down/drag/release handling does NOT go here — it needs `eye` projection + selection enumeration, which live game-side; that is Task 3 in controlgui.cpp where the per-frame overlay hook already runs with `eye` valid.

- [ ] **Step 2: Build**

Run verbatim build command. Expected: exit 0.

- [ ] **Step 3: Commit**

```bash
git add code/mechcmd2.cpp
git commit -m "feat(tacmap): formation line input polling (L arm, Esc/RMB cancel) at F6 input site"
```

---

### Task 3: Drag tracking, order issuance, ghost line + pips in controlgui.cpp

**Files:**
- Modify: `code/controlgui.cpp` (new static functions next to `renderSensorView`; hook in `ControlGui::render` inside the `g_tacticalOverview.active()` block)

- [ ] **Step 1: Add includes if missing**

`controlgui.cpp` already uses `Team::home`, movers, `eye`. Check (grep) that `tacordr.h` (TacticalOrder), `tacticaloverview.h`, and `comndr.h` (Commander) are included; add any missing next to existing includes.

- [ ] **Step 2: Add the per-frame formation-line update+render function**

Place after `renderWeaponView` (~line 686), before `ControlGui::render`:

```cpp
// Formation line (MC2_TACMAP_FORMATION_LINE): per-frame drag tracking, ghost
// line + slot pips, and order issuance on release. Runs only while the F6
// overview is active; input arming/cancel is polled in mechcmd2.cpp.
static void updateAndRenderFormationLine( Camera* eye )
{
	if ( !eye || !Team::home || !TacticalOverview::formationLineEnabled() )
		return;
	TacticalOverview::FormationLineState st = g_tacticalOverview.flState();
	if ( st == TacticalOverview::FL_IDLE )
		return;

	long mx = userInput->getMouseX();
	long my = userInput->getMouseY();

	// --- state advance ---
	if ( st == TacticalOverview::FL_ARMED )
	{
		if ( userInput->getMouseLeftButtonState() == MC2_MOUSE_DOWN )
		{
			Stuff::Vector3D w;
			if ( eye->screenToGroundPlaneApprox( mx, my, w ) )
			{
				// Snapshot selected friendly movers at drag start.
				void* movers[32];
				int   nm = 0;
				Team* pTeam = Team::home;
				for ( long i = 0; i < pTeam->getRosterSize() && nm < 32; i++ )
				{
					Mover* pMover = (Mover*)pTeam->getMover( i );
					if ( pMover && pMover->isSelected()
						&& pMover->getCommander()->getId() == Commander::home->getId()
						&& pMover->getExists() && !pMover->isDestroyed() && !pMover->isDisabled() )
						movers[nm++] = pMover;
				}
				if ( nm == 0 )
				{
					g_tacticalOverview.flOnCancel();	// nothing selected: cancel
					return;
				}
				g_tacticalOverview.flSetMovers( movers, nm );
				g_tacticalOverview.flOnDragStart( w );
				st = g_tacticalOverview.flState();
			}
		}
	}
	if ( st == TacticalOverview::FL_DRAGGING )
	{
		Stuff::Vector3D w;
		if ( eye->screenToGroundPlaneApprox( mx, my, w ) )
			g_tacticalOverview.flOnDragMove( w );

		if ( userInput->leftMouseReleased() )
		{
			// --- issue orders: greedy nearest mover->slot, one MOVETO each ---
			Stuff::Vector3D slots[32];
			int ns = g_tacticalOverview.flComputeSlots( slots, 32 );
			bool slotUsed[32] = { false };
			int nm = g_tacticalOverview.flMoverCount();
			for ( int i = 0; i < nm; i++ )
			{
				Mover* pMover = (Mover*)g_tacticalOverview.flMover( i );
				// Revalidate: mover may have died mid-drag.
				if ( !pMover || !pMover->getExists()
					|| pMover->isDestroyed() || pMover->isDisabled() )
					continue;
				// Nearest free slot to this mover.
				int   best = -1;
				float bestD2 = 0.0f;
				Stuff::Vector3D mp = pMover->getPosition();
				for ( int s = 0; s < ns; s++ )
				{
					if ( slotUsed[s] ) continue;
					float dx = slots[s].x - mp.x, dy = slots[s].y - mp.y;
					float d2 = dx * dx + dy * dy;
					if ( best < 0 || d2 < bestD2 ) { best = s; bestD2 = d2; }
				}
				if ( best < 0 ) break;
				slotUsed[best] = true;

				LocationNode path;
				path.location = slots[best];
				path.run = true;
				path.next = NULL;
				TacticalOrder tacOrder;
				tacOrder.init( ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_MOVETO_POINT, false );
				tacOrder.initWayPath( &path );
				tacOrder.moveParams.wait = false;
				tacOrder.moveParams.wayPath.mode[0] = TRAVEL_MODE_FAST;
				tacOrder.pack( NULL, NULL );
				// Single-unit dispatch on purpose: handleOrders/calcMoveGoals
				// would re-cluster the slots and defeat the line.
				pMover->handleTacticalOrder( tacOrder );
			}
			g_tacticalOverview.flOnRelease();
			soundSystem->playDigitalSample( BUTTON5 );
			return;		// nothing to draw this frame
		}
	}

	// --- render ---
	bool prevExempt = gos_GetHudScaleExempt();
	gos_SetHudScaleExempt( true );
	gos_SetRenderState( gos_State_Texture, 0 );
	gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha );
	gos_SetRenderState( gos_State_AlphaTest, 0 );
	gos_SetRenderState( gos_State_ZCompare, 0 );
	gos_SetRenderState( gos_State_ZWrite, 0 );

	const unsigned long aBits = 0xcc000000;
	const unsigned long kGreen = 0x0000cc00;

	if ( st == TacticalOverview::FL_ARMED )
	{
		// Cursor hint while armed.
		if ( gosFontHandle )
		{
			gos_TextSetAttributes( gosFontHandle, aBits | kGreen, 1.0f, false, true, false, false );
			gos_TextSetPosition( (int)mx + 14, (int)my + 14 );
			gos_TextDraw( "DRAW FORMATION LINE" );
		}
	}
	else if ( st == TacticalOverview::FL_DRAGGING )
	{
		float vmx, vmy, vax, vay;
		gos_GetViewport( &vmx, &vmy, &vax, &vay );

		// Ghost line: project both world endpoints (w>0 cull only).
		ModernClipResult r0 = eye->projectModernClipGL( g_tacticalOverview.flStart() );
		ModernClipResult r1 = eye->projectModernClipGL( g_tacticalOverview.flEnd() );
		if ( r0.clip.w > 0.05f && r1.clip.w > 0.05f )
		{
			float x0 = vax + ( r0.clip.x / r0.clip.w * 0.5f + 0.5f ) * vmx;
			float y0 = vay + ( 1.0f - ( r0.clip.y / r0.clip.w * 0.5f + 0.5f ) ) * vmy;
			float x1 = vax + ( r1.clip.x / r1.clip.w * 0.5f + 0.5f ) * vmx;
			float y1 = vay + ( 1.0f - ( r1.clip.y / r1.clip.w * 0.5f + 0.5f ) ) * vmy;
			csgThickSeg( x0, y0, x1, y1, 1.5f, aBits | kGreen );
		}

		// Slot pips: small filled quads at each slot.
		Stuff::Vector3D slots[32];
		int ns = g_tacticalOverview.flComputeSlots( slots, 32 );
		for ( int s = 0; s < ns; s++ )
		{
			ModernClipResult r = eye->projectModernClipGL( slots[s] );
			if ( r.clip.w <= 0.05f ) continue;
			float cx = vax + ( r.clip.x / r.clip.w * 0.5f + 0.5f ) * vmx;
			float cy = vay + ( 1.0f - ( r.clip.y / r.clip.w * 0.5f + 0.5f ) ) * vmy;
			const float hp = 3.0f;
			gos_VERTEX q[4];
			for ( int v = 0; v < 4; ++v ) { q[v].z = 0; q[v].rhw = .5f; q[v].argb = aBits | kGreen; q[v].frgb = 0; q[v].u = q[v].v = 0; }
			q[0].x = cx - hp; q[0].y = cy - hp; q[1].x = cx - hp; q[1].y = cy + hp;
			q[2].x = cx + hp; q[2].y = cy + hp; q[3].x = cx + hp; q[3].y = cy - hp;
			gos_DrawQuads( q, 4 );
		}

		// Selected-unit count at the line end.
		if ( gosFontHandle )
		{
			char buf[16];
			sprintf( buf, "%d", g_tacticalOverview.flMoverCount() );
			gos_TextSetAttributes( gosFontHandle, aBits | kGreen, 1.0f, false, true, false, false );
			gos_TextSetPosition( (int)mx + 14, (int)my + 14 );
			gos_TextDraw( buf );
		}
	}

	gos_SetHudScaleExempt( prevExempt );
}
```

Adaptation notes for the executor:
- `MC2_MOUSE_DOWN`: confirm exact constant name in `mclib/userinput.h` (~line 288 uses `MC2_MOUSE_UP`); use whatever the down-state constant is.
- `LocationNode`, `TacticalOrder`, `TRAVEL_MODE_FAST`, `ORDER_ORIGIN_PLAYER`: copy includes/usage from `missiongui.cpp:3646-3667`. If `LocationNode` lives in a header controlgui.cpp lacks, include it (grep `LocationNode` for the header).
- `soundSystem->playDigitalSample(BUTTON5)`: same call as doMove; if BUTTON5 undeclared here, drop the sound line rather than adding includes.
- `tacOrder.pack(NULL, NULL)` before dispatch — copy doMove exactly.
- A mouse press on a squad card also starts a drag here; v1 accepts this overlap (cards consume on press via `armReleaseSuppression`, line still issues). If trivially avoidable with `cardHitAt(mx,my)` returning null check at drag start, add it; do not build more than that.

- [ ] **Step 3: Hook into `ControlGui::render`**

Inside the existing `if ( g_tacticalOverview.active() )` block (~line 713), at its top (before the tint/markers), add:

```cpp
			// Formation line ghost + pips + order issuance (env-gated, no-op IDLE).
			updateAndRenderFormationLine( eye );
```

- [ ] **Step 4: Build**

Run verbatim build command. Expected: exit 0.

- [ ] **Step 5: Commit**

```bash
git add code/controlgui.cpp
git commit -m "feat(tacmap): formation line drag tracking, ghost line + slot pips, per-mover move orders"
```

---

### Task 4: Slice gate — tier1 smoke + env-off invariance

**Files:** none (verification only)

- [ ] **Step 1: tier1 smoke, env OFF (default)**

Run the verbatim smoke command (no MC2_TACMAP_FORMATION_LINE set). Expected: exit 0, 5/5 pass. Smoke runs the DEPLOYED exe — first copy the fresh build:

```powershell
Copy-Item A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64\RelWithDebInfo\mc2.exe A:\Games\mc2-opengl\mc2-win64-v0.4\mc2.exe
```

- [ ] **Step 2: env-off code invariance sanity**

Grep check: every new behavior path is behind `formationLineEnabled()` or `flState() != FL_IDLE`. Confirm:

```powershell
powershell -NoProfile -Command "Select-String -Path code\mechcmd2.cpp,code\controlgui.cpp -Pattern 'formationLineEnabled|flState|flOn' | Measure-Object | Select-Object Count"
```
Every call site must be inside an enabled()/state guard (manual read of the hits).

- [ ] **Step 3: Commit any fixups, then report**

Interactive acceptance (USER, env ON, in v0.4 deploy):
- `MC2_TACMAP_FORMATION_LINE=1`, enter mission, F6, select 2-8 movers, L, drag, see ghost+pips+count, release -> movers walk to line; Esc and F6-exit cancel; L with overview off does nothing.

---

## Self-review notes

- Spec coverage: gate (T1), scope (all tasks minimal), input/state machine (T1+T2), world mapping via screenToGroundPlaneApprox (T3), unit snapshot at drag start + revalidate on release (T3), slot gen N==0/1/N (T1), greedy nearest (T3), single-unit orders bypassing handleOrders (T3), visuals incl. ARMED hint + pips + count + 3 projection rules (T3), forbidden files untouched (no mclib/pathfinding edits; userinput.h read-only), acceptance (T4). 
- Type consistency: `flState()/flOnHotkeyL/flOnCancel/flOnDragStart/flOnDragMove/flOnRelease/flComputeSlots/flSetMovers/flMover/flMoverCount` used identically in T1 declarations and T2/T3 call sites.
- Known deviation risks called out inline: MC2_MOUSE_DOWN constant name, LocationNode include, Stuff::Vector3D in header, card-press overlap. Executor verifies each with grep before use.
