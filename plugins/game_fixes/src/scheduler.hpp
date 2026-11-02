// game_fixes — the port of the fork's scheduler: repeat / once timers and a next-frame queue,
// ticked from the plugin's per-frame pump.
//
// Differences from the fork: the fork ticked from the engine's GameFrame hook and advanced its
// clock by the engine's simulated time; here the pump is OnPluginUpdate (the plugify module calls
// it from ServerPostSimulate, i.e. once per frame with a live map) and the clock advances by the
// wall-clock delta the module reports. That keeps the timers meaningful while the server is empty
// and hibernating, which is exactly when slow_animation runs.

#pragma once

#include <functional>

namespace game_fixes::scheduler {

constexpr int kTimerFlagRepeat = 1 << 0;
constexpr int kTimerFlagNoMapChange = 1 << 1;

// One engine tick at the default 64 tickrate; the same floor s2sdk's own timer system uses
// (engineFixedTickInterval). No delay below this is ever scheduled: a repeating timer with a 0
// delay re-arms itself inside a single engine frame and hangs the main thread (PITFALLS C.9 /
// PLUGIN_STANDARD §7 item 5). AddTimer() clamps every interval to it, so no caller can create one.
constexpr double kMinTimerInterval = 1.0 / 64.0;

struct Timer {
	float interval = 0.0f;
	double execTime = 0.0;
	std::function<void()> callback;
	int flags = 0;
	bool killMe = false;
	bool inExec = false;
};

// Seconds since scheduler::Init(); advanced by Tick(). The first Tick after Init does not advance
// the clock: its delta may cover the time before this plugin's first update (the host's whole
// uptime), which would fire every timer at once.
double UniversalTime();

void Init();
void Shutdown();
void Tick(double deltaSeconds);

// Schedules a timer. The interval is clamped to at least kMinTimerInterval (a non-positive or NaN
// interval means "as soon as possible", which is one tick). Timers are created from the plugin's
// own setup/update paths and from StartupServer - never from inside a timer callback, where the
// engine-side timer systems (s2sdk's included) are the ones that behave dangerously; this
// scheduler is plugin-local and only runs from OnPluginUpdate.
Timer* AddTimer(float interval, std::function<void()> callback, int flags = 0);
void KillTimer(Timer* timer);

// Runs at the start of the next Tick.
void NextFrame(std::function<void()> task);

// Drops every piece of per-map delayed work: the queued next-frame tasks (the deferred kicks and
// the blank_map restart) and every one-shot timer (the workshop_voice registry lookup).
//
// Called from the OnMapEnd listener *after* every fix got CFix::OnMapEnd(), so a fix has already
// dropped its own per-map timer handle and this is the final sweep; a one-shot timer whose owner
// did not do that must not be kept in a member afterwards. Repeating timers stay alive - they are
// the periodic checks (slow_animation's reload/watch, fake_messages' signon seed, cstv_bot_quota's
// offset resolve) and every callback of theirs re-validates the map gate before touching a client
// or an entity (PITFALLS B.4). Nothing that was meant for the old map can run against the new one.
void ClearDelayedWork();

} // namespace game_fixes::scheduler
