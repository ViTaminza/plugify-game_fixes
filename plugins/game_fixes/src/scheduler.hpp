// game_fixes — the port of the fork's scheduler (ref/src/GameFixes/src/scheduler.*): repeat /
// once timers and a next-frame queue, ticked from the plugin's per-frame pump.
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

struct Timer {
	float interval = 0.0f;
	double execTime = 0.0;
	std::function<void()> callback;
	int flags = 0;
	bool killMe = false;
	bool inExec = false;
};

// Seconds since scheduler::Init(); advanced by Tick().
double UniversalTime();

void Init();
void Shutdown();
void Tick(double deltaSeconds);

Timer* AddTimer(float interval, std::function<void()> callback, int flags = 0);
void KillTimer(Timer* timer);

// Runs at the start of the next Tick.
void NextFrame(std::function<void()> task);

} // namespace game_fixes::scheduler
