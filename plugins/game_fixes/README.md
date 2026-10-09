# game_fixes — Plugify port

The CS2 server fixes of **GameFixes** (`ref/src/GameFixes`, the fork `ezekutor/game_fixes` of
`SlynxCZ/GameFixes_mm`, a Metamod:Source plugin) rebuilt as a native Plugify plugin on
**s2sdk + polyhook**. GPL-3.0, same as the original.

| | original (Metamod) | this port |
|---|---|---|
| source | `ref/src/GameFixes/src/**` | `plugins/game_fixes/src/**` |
| lines | 5909 (incl. the fork's private SDK headers and license banners) | 3995 across 20 files |
| engine access | Metamod + its own `sdk/` reimplementations | s2sdk bindings + polyhook hooks |
| signatures/offsets | hard-coded patterns per fix | one gamedata file, `assets/gamedata.jsonc` |
| config | `addons/configs/game_fixes/game_fixes.ini` (KeyValues) | same file, parsed by pisexcompat's KeyValues |

## The fixes

Block in `game_fixes.ini` → what the original hooked/patched → how this port does it. Offsets
marked *(gamedata)* are in `assets/gamedata.jsonc`; `server`/`engine2` are the game libraries.

1. **beam_crash** — detours `CBeam::SetBeamOrigin` and `CBeam::SetBeamEndPos` (both by signature,
   `server`). A beam with no parent (no scene-node parent and no owner) makes those two call each
   other forever; the fork supersedes that path and its Post makes the one assignment the engine
   would have made ("the unparented path is reimplemented"). The port does the same assignment in
   the Pre callback itself (polyhook's Supersede ends the call) — scene node `m_vecAbsOrigin` for
   the origin, the `CBeam` schema field `m_vecEndPos` for the end position — and parented beams go
   through the game code untouched. *(gamedata: the two code signatures; schema offsets for the
   body/scene-node/abs-origin chain, resolved at runtime.)*
2. **demo_record** — hooks `CServerSideClient::Disconnect` (engine2 vtable, index 8, *gamedata*)
   and supersedes it for the GOTV client (`m_bIsHLTV`, offset 322) while `tv_enable` is 1, so the
   recording client is never let go; on the Pre hook of `INetworkServerService::StartupServer`
   (vtable index 27) it raises `GameSessionConfiguration_t::maxPlayers` (offset 0x7C) by one
   *before* the engine reads it, so the GOTV slot is on top of the configured player count.
3. **hammer_id** — patches the `jnz` that skips `m_sUniqueHammerID` in `SetSchemaHammerUniqueId`
   to a `jmp` (1 byte, `EB`), so every entity gets a hammer id (the CS2Fixes fix). The patch is a
   `Patches` entry in the gamedata and is applied (and restored on unload) by s2sdk's patch
   manager when the plugin loads the game config. s2sdk's *own* gamedata carries the same patch
   commented out, so nothing else applies it.
4. **slow_animation** — `curtime` is a 32-bit float and goes slack after a day or two on one map.
   A plugin timer every `reload_interval` s (min 60) checks the server; while no human is on, it
   reloads the current map (`changelevel`, or `ds_workshop_changelevel` for a workshop map) and
   carries the remaining `mp_timelimit` over (restored one frame after the next StartupServer,
   past the map cfg). "Human" matches the fork exactly: any client present at a slot that is
   neither a bot (`m_bFakePlayer`, 160) nor GOTV (`m_bIsHLTV`, 322), in *any* signon state — the
   fork deliberately does not require IsConnected so a player who is still connecting is never
   caught by the reload; and a 300 s pause after the last human guards "join, leave, join".
5. **workshop_voice** — clients key voice playback off the xuid in `svc_VoiceData`; on workshop
   maps streams collide and players stop hearing each other. The original hooked
   `CServerSideClient::SendNetMessage` (which runs once per recipient) and rewrote the xuid for
   that listener. The port uses s2sdk's `OnSendNetMessage` listener for the same interception:
   for `svc_VoiceData` the speaker's field is rewritten to a per-speaker xuid from a per-map seed
   (seeds 66 apart, reset on every map). *(gamedata: the protobuf field offsets.)*
6. **steam_ban** — detours `GameSystem_Think_CheckSteamBan` (the engine's per-frame GC ban /
   competitive-cooldown kick pass). Pre strips the whitelisted accounts (the ini gives SteamID64s,
   the map is keyed by the 32-bit account id) and, while `sv_kick_players_with_cooldown` < 2, the
   competitive-cooldown entries (reasons 20, 22, 23); Post (`clear_after_pass`, default 1) empties
   the map with the engine's own `RemoveAll` writes (free-list rebuild over every slot, then
   `m_Root = -1` / `m_NumElements = 0` in one store). The original removed entries from the RB
   tree (`RemoveAt`); the port *neutralizes* them (key and reason = `0xFFFFFFFF`, which the pass
   cannot match — it reads exactly those two node fields) instead, so the tree's structure is
   never touched; `clear_after_pass` then leaves the map in the engine's own empty state. Map
   layout (node stride 0x38, key at node+0x10, reason at node+0x18, capacity at map+0x08, array at
   +0x10, root +0x18, count +0x1C, free head +0x20) verified against the current libserver.so
   disassembly. *(gamedata: the pass and map signatures + the map/node offsets.)*
7. **team_limit** — the map's spawn points cap how many players may spawn per team, so a join gets
   refused as "team full" on maps with few spawns. On every `round_start` the four
   `CCSGameRules` fields (spawnable/max T and CT) are set to `maxplayers` *(schema offsets, read
   at runtime)*.
8. **input_activator_crash** — `CEntityIdentity::AcceptInput` → `CBaseFilter::InputTestActivator`
   dereferences the input's activator without checking it; a `TestActivator` input fired with no
   activator (a map's output wired straight to a filter) takes the server down. The port detours
   `CEntityInstance::AcceptInput` — the entry point s2sdk maintains per game build (the fork
   hooked the identity-level one) — and drops `filter_` + `TestActivator` with a null activator.
   *(gamedata: the fork's `CEntityIdentity_AcceptInput` is shipped alongside; s2sdk resolves the
   instance-level signature.)*
9. **sv_cheats** — watches the cvar (`FindConVar` + `HookConVarChange` via s2sdk) and, the moment
   it flips to 0, puts every pawn still in `MOVETYPE_NOCLIP` back to `MOVETYPE_WALK`
   (`SetClientMoveType`), which is the one thing the engine forgets to do.
10. **rampbug** — surf ramps: a movement trace landing on a ramp edge feeds `TryPlayerMove` a
    plane the player never hit and the player is flung off. The port is CS2KZ's fix as carried by
    CS2Fixes-RampbugFix: `CCSPlayer_MovementServices::TryPlayerMove` is detoured and the game's
    own walk redone with every trace nudged off the last valid plane, a bad result put back, plus
    the `CategorizePosition` nudge; the engine interfaces are rebuilt from the fork's binary
    (`CNavPhysicsInterface::Nav_TraceShape`, vtable index 5, `CNavPhysicsInterface` and
    `CTraceFilter` vtables, `Ray_t`/`CGameTrace`/`CTraceFilter` layouts, `CMoveData` velocity/
    origin). It lessens rampbugs, it does not end them.
11. **water_jump** — touching the bottom of water, the `CategorizePosition` after `WaterMove`
    sees ground and treats the player as standing on it, so a jump out no longer takes. The one
    `CategorizePosition` following each `WaterMove` is skipped (shared movement listener, flag
    per slot, reset per map).
12. **keyvalues_flood** — a client sprays `clc_CmdKeyValues` and the server parses it. Hooks
    `CServerSideClient::ProcessCmdKeyValues` (vtable index 35): a payload over `max_bytes`, or
    more than `max_per_second` messages (bucket with `burst`), is swallowed for good — the
    original also made every message from a caught client a no-op — and with `kick 1` the client
    is kicked (deferred to the next frame). *(gamedata: the protobuf `data` field offset and the
    client-vtable indices.)*
13. **team_switch_spam** — a client spamming `jointeam`/`spectate` makes the server walk the
    team-change path every time. Past `max_attempts` within `window_seconds` the client is
    kicked. *Deviation:* the fork counted the raw `clc_StringCmd` messages inside
    `CServerSideClient::ProcessStringCmd`; the port counts the *dispatched* `jointeam`/`spectate`
    console commands instead (s2sdk's command hooks), which skips unrecognized-command spam the
    engine's own rate limiter already covers.
14. **voice_flood** — the "server lagger": a client floods `clc_VoiceData` (empty voice, thousands
    of `packet_offsets`) and the server lags copying it to every listener. Hooks
    `CServerSideClient::ProcessVoiceData` (vtable index 39): more `packet_offsets` than
    `max_packets` (or than there are voice bytes), more than `max_bytes` of voice, or more than
    `max_per_second` messages swallow the message and (with `cooldown`) drop the client's voice
    for that long; `kick_strikes` catches within a minute of each other and `kick 1` kick the
    client. *(gamedata: protobuf offsets + vtable index.)*
15. **server_list_players** — **not ported**, on purpose: s2sdk's player manager already calls
    `ISteamGameServer::BUpdateUserData` for every client every 5 s
    (`ref/s2sdk-src/src/core/player_manager.cpp:346`), which is what this block (and its
    `update_interval`) exists for. Porting it would fight over the call; the block is logged and
    skipped.

## Config

`assets/game_fixes.ini` is a byte copy of the fork's `configs/addons/game_fixes/game_fixes.ini`
(same blocks, keys, comments and defaults; also the file the deployed 27035 server runs — the
deployed copy differs only in that its `voice_flood` block predates `max_bytes`, which is
optional here too). Every block is `"enable"`-toggled; a disabled or failed fix logs why and the
rest still load. The plugin resolves `game_fixes.ini` through pisexcompat: server working
directory first, then `<configs>/game_fixes/game_fixes.ini`, then `<configs>/game_fixes.ini`
(the packaged layout is `configs/game_fixes/game_fixes.ini`).

The original registers no convars of its own; it *reads* `tv_enable`, `mp_timelimit`, `sv_cheats`
and `sv_kick_players_with_cooldown`, and the port does the same through s2sdk's cvars, with no
new cvars.

## What s2sdk already does (nothing is applied twice)

* `SetSchemaHammerUniqueId` patch: s2sdk's gamedata has it **commented out** — the port's
  `Patches` entry is the only one. s2sdk's *active* `GetHammerIdPatch` (address
  `GetHammerUniqueId`, `B0 01`) is the getter side and stays as it is; they complement.
* The movement patches (`FixWaterFloorJumpPatch`, `ServerMovementUnlockPatch`,
  `BotNavIgnorePatch`) are commented out in s2sdk's gamedata, so water_jump/rampbug do not
  double-apply.
* `server_list_players` — see fix 15.
* Where s2sdk hooks the same engine function, the port reuses the existing hook
  (`FindVirtual2`/`FindDetour` first, `s2sdk`-installed hooks included) instead of installing a
  second one: `INetworkServerService::StartupServer` (s2sdk has Post, this plugin adds Pre) and
  any `CServerSideClient` vtable slot s2sdk may have taken. Message fixes use s2sdk's listeners
  (`OnSendNetMessage`) rather than another vtable hook where one exists.

## Deviations from the fork (all deliberate)

| # | deviation | why |
|---|---|---|
| 6 | whitelist/cooldown entries are neutralized (key+reason = 0xFFFFFFFF), not `RemoveAt`-ed | the port never touches the RB tree's structure; the pass cannot match a neutralized entry. With `clear_after_pass 1` the map ends empty exactly like the original |
| 8 | hooks `CEntityInstance::AcceptInput` instead of `CEntityIdentity::AcceptInput` | s2sdk maintains the instance-level signature per build; the fork's identity-level pattern is shipped in the gamedata and still verified unique |
| 13 | counts dispatched `jointeam`/`spectate` instead of raw `clc_StringCmd` | avoids the protobuf/string-command ABI; unrecognized-command spam (already covered by the engine's limiter) is the one case not counted |
| 5 | s2sdk listener instead of a second `SendNetMessage` vtable hook | the vtable slot is already used by s2sdk; both hooks would work, one is enough |
| — | protobuf field reads (`voice_flood`, `keyvalues_flood`) use the fork's own compiled offsets and its `data-0x18` length read | the protobuf ABI is not exported; the constants are DWARF-verified against the fork's build and live in the gamedata |

## Files

```
plugins/game_fixes/
  CMakeLists.txt              plugin + tests (build/tests/game_fixes_{signature_check,core}_tests)
  game_fixes.pplugin.in       manifest (deps: s2sdk >= 2.18.2, polyhook >= 2.5.6, optional hudmenu)
  sym/version_script.lds      exports Plugify_*, __s2sdk_*, __polyhook_*, __hudmenu_*
  package.sh                  build/package/game_fixes/{bin,configs,assets}
  assets/gamedata.jsonc       signatures (10), offsets, vtables, the hammer-id patch
  assets/game_fixes.ini       the fork's config, byte for byte
  src/api.{hpp,cpp}           gamedata loading, hook helpers, environment resolution
  src/ban_map.hpp             the ban-map operations steam_ban performs (engine-free, tested)
  src/load_policy.hpp         when a fix may load (the schema-deferral rule, tested)
  src/scheduler.{hpp,cpp}     universal time / timers / next-frame queue
  src/fixes.hpp               CFix + IMovementListener interfaces
  src/plugin.cpp              lifecycle, config, shared hooks (StartupServer Pre, movement detours)
  src/fixes/*.cpp             one file per fix (14)
  tests/signature_check_tests.cpp  ELF pattern scan: every gamedata pattern matches exactly once
  tests/core_logic_tests.cpp       ban-map writes, scheduler semantics, Itanium index encoding
  tools/verify_signatures.py  standalone re-check of the same patterns (dev aid)
```

## Schema-load crash (live, 2026-10-08)

`GetSchemaOffset` was called from `CRampbugFix::Load()` during `OnPluginStart` and segfaulted inside
s2sdk (`CollectSchemaFields` -> `CUtlRBTree::Find` on the null `g_pEntityNetworkSerializerInfo`
class-info map; minidumps `ref/crash/dumps/153326/153332/153648/153652`). The previous round's
deferral only kicked in *after* a fix's `Load()` had returned false - `beam_crash` was deferred that
way, but `rampbug::Load()` calls the schema system itself and crashed before returning.

Fixed:

* The load decision now happens **before** `Load()`: a fix with `NeedsSchema()` is put aside while
  the schema offsets are unresolved and never calls into the schema system at plugin start
  (`src/load_policy.hpp`, pinned by `tests/core_logic_tests.cpp` - the decision table that
  prevents this crash).
* Schema resolution itself is gated on "a map is running" (our `StartupServer` Pre callback, or
  pisexcompat's map-running state for a late plugin load) and is avoided during shutdown/
  changelevel, when the entity network serializer info is torn down again.
* No fix touches clients while a map is not running: `sv_cheats` (cvar changes also fire during
  shutdown) checks `pisex::IsGameReady()` and then `PlayerSlotToClientPtr` - the one client lookup
  in s2sdk that is silent *and* checks the client list's count - before any move-type call;
  `slow_animation` reports "players present" instead of counting when no map runs; the three
  deferred kicks (`voice_flood`, `keyvalues_flood`, `team_switch_spam`) re-check map-running next
  to the slot/user-id, since s2sdk's `KickClient` does not validate the slot; the name/steam-id
  logging in those paths is skipped outside a running map (those go through
  `PlayerManager::Player::GetClient` -> `utils::GetClientBySlot`, whose `Element(slot)` is
  unchecked and crashes on the emptied client list). `IsClientInGame` alone is *not* a safe gate
  for the same reason - it is only used after the client-object check has passed.

## Crash-audit round (2026-10-08)

The full production set crashed the test server; every plugin was audited. Findings and fixes
here (in addition to the load-order bugs the live log had already shown):

* **Two fixes never loaded on the live server, for load-order reasons** — fixed:
  * `beam_crash` refused to load because the schema offsets (`m_pParent` chain) are only resolved
    once a map is running, which is after `OnPluginStart`. Config fixes that need the schema
    (`beam_crash`, `rampbug`) are now *deferred*: they are retried from `OnPluginUpdate` the
    moment the offsets resolve (bounded to 60 attempts, then they report why). The offsets are
    deliberately not queried at plugin load: s2sdk caches a class lookup's result for the whole
    process, so a too-early miss would poison it for every plugin.
  * `workshop_voice` refused to load because `svc_VoiceData` is not a registered message id yet
    at plugin load. It now starts from the protocol constant `SVC_Messages::svc_VoiceData = 47`
    (gamedata entry, taken from the fork's own build) and refines it with the registry lookup at
    the first map start.
* **Hooks are now ownership-aware.** `hook::HookRef` records whether *this plugin* created a
  hook or reused one s2sdk (or another plugin) had already installed. Teardown always removes
  our callback and only unhooks a hook we created *and* that has no other callbacks left — the
  previous code could unhook a shared hook (silently disarming s2sdk) or, on a failed load,
  unhook something this plugin never installed. A partially installed pair of detours
  (`beam_crash`) now also cleans up after itself.
* **`UninstallHooks` no longer skips the hooks that did install** when one of the four movement
  detours failed to install — those callbacks would otherwise point into the unloaded library.
* **`Supercede` on a non-void function now sets the return value** (`SetSupersededReturn(true)`,
  what the fork's Supersede does in all of its handlers). `voice_flood` and `keyvalues_flood`
  skip the engine's `bool`-returning message handlers; without this the engine read a stale
  return register. `input_activator_crash`'s target returns void, so only the skip is needed
  there.
* **`steam_ban`'s `RemoveAll` rebuild was wrong in two ways** (found by the new unit test, both
  would corrupt the container's free list into a cycle): the rebuild must start from an *empty*
  head, not from the map's current one, and it must relink every slot unconditionally (a slot
  whose `m_Left` already equals its index — e.g. slot 0 of a never-used container — must not be
  skipped). The header still ends `m_Root = -1`, `m_NumElements = 0`.
* **`input_activator_crash`**: the invalid-handle test compared `EntPointerToEntHandle()` against
  `0`, but the invalid handle is `-1` — so an invalid entity walked into
  `GetEntityClassname()`, which logs a warning per call ("Cannot execute ... on invalid entity
  handle"). It now tests `< 0`, and the cheap input-name check runs before the class lookup.
* **`sv_cheats`** queried `GetClientMoveType` for every slot, which makes s2sdk log "invalid
  player slot"/"invalid player pawn" for each empty slot (up to ~60 lines per `sv_cheats`
  toggle). It now only touches slots where `IsClientInGame` is true (silent, and InGame
  guarantees the controller/pawn exist).
* **`rampbug`** read the filter's owner handle through `GetEntityOwner()`, which walks the script
  VM (`g_pScriptVM->GetInstanceValue`) per movement — expensive in a per-tick path. It reads the
  schema field `m_hOwnerEntity` directly instead (same handle), with `GetEntityOwner` kept as a
  fallback. The trace-filter byte image was re-verified field by field against the engine's own
  DWARF (`CTraceFilter` = vptr + `RnQueryShapeAttr_t` at +0x08 + `m_bIterateEntities` at +0x3A;
  the ignore arrays are 2×4-byte handles at +0x20/+0x28), so the writes are handle-sized and
  cannot be mistaken for pointers.
* Hook argument indices were re-verified against the fork's own handler signatures and s2sdk's
  handlers (`this` at 0 for member functions; StartupServer config=1/session=2/mapName=3;
  `CEntityInstance::AcceptInput` = self, inputName, activator, caller, variant*, outputId, void*;
  the message handlers take `this` + the message). No s2sdk hook overlaps one of ours
  (`SendNetMessage`/`ProcessRespondCvarValue` are s2sdk's slots; the movement functions,
  `SetBeamOrigin/EndPos`, `CheckSteamBan` and `AcceptInput` are hooked by nobody else).

The new `tests/core_logic_tests.cpp` (58 checks) covers the ban-map writes with canaries around
header and nodes, the scheduler's timer/next-frame semantics and the Itanium index encoding.

## Build, test, package

```sh
./build.sh plugify-game_fixes        # -> build/plugins/game_fixes/libgame_fixes.so
./test.sh                            # game_fixes_core_tests + game_fixes_signature_check_tests
bash plugins/game_fixes/package.sh   # -> build/package/game_fixes/
```

`tests/signature_check_tests.cpp` also dumps, into `SIGSCAN.md`, the xrefs of the strings around
the two ban functions, which is how the two re-derived patterns were verified.

## Needs a live-server check (not verifiable offline)

1. Re-run the signature scan against the *deployed* `/srv/cs2-go/.../libserver.so` (the workspace
   copies are from 2026-10-06; `./test.sh` reads those when `/srv` is not mounted). All 10
   patterns matched exactly once there. Also re-check `SIGSCAN.md`'s VAs against the deployed
   module base.
2. That the hammer-id patch byte is written (log line "patch bytes now 'EB'") and that entities
   really carry unique hammer ids.
3. That the hooks fire: `disconnect` on a GOTV client with `tv_enable 1`, `StartupServer` max-
   players bump, `round_start` team limits, a `TestActivator` map output, `sv_cheats 0` leaving
   noclip, a workshop map's voice.
4. That no fix fights another plugin's hook on the same function (startup log lines name the
   functions hooked; s2sdk's Post hook on StartupServer must still run).
5. Rampbug/water_jump feel on a surf server — they are the two with engine ABI assumptions
   (traces/filters/movement structs).
6. After this audit: the startup log must now show `beam_crash` and `rampbug` as "waiting for the
   schema offsets" followed by "loaded" on the first map update, and `workshop_voice` must load
   right away (message id 47, refined at the first map start). `sv_cheats 0` must produce exactly
   one log line and no "invalid player slot" warnings.
