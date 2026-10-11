# game_fixes (Plugify)

The CS2 server fixes of the GameFixes fork (ezekutor/game_fixes, formerly SlynxCZ/GameFixes_mm) as one Plugify plugin, each toggled in configs/game_fixes/game_fixes.ini: beam_crash (CBeam::SetBeamOrigin/SetBeamEndPos loop on a parentless beam), demo_record (GOTV client kept, session maxplayers +1), hammer_id (SetSchemaHammerUniqueId patched jnz->jmp), slow_animation (empty-server map reload by reload_interval, mp_timelimit carried over), workshop_voice (distinct per-listener xuid in svc_VoiceData), steam_ban (whitelist + cooldown stripping in the engine's GC-ban pass), team_limit (spawnable/max T and CT raised to maxplayers every round_start), input_activator_crash (TestActivator without an activator dropped), sv_cheats (noclip pawns back to MOVETYPE_WALK when it turns off), rampbug (CS2KZ's TryPlayerMove ramp fix), water_jump (the CategorizePosition after WaterMove skipped), keyvalues_flood, team_switch_spam and voice_flood (message rate limits). Hooks run through the polyhook plugin (reusing s2sdk's hooks where they exist); signatures/offsets/vtables/the one byte patch live in assets/gamedata.jsonc. server_list_players is NOT ported: s2sdk already pushes player data to the Steam game server API every 5 seconds.

Plugify port (fork) of **GameFixes** by Slynx (Metamod (C++)).
Original: https://github.com/SlynxCZ/GameFixes_mm
The code was rewritten for Plugify / s2sdk; behaviour follows the original.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugify dependencies: s2sdk, polyhook, hudmenu (optional)

## Installation
Download `game_fixes-1.0.1.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/game_fixes`.

## License
GPL-3.0, see LICENSE.

