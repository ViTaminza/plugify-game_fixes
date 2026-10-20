#include "scheduler.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <queue>
#include <vector>

namespace game_fixes::scheduler {
namespace {

double s_universalTime = 0.0;
double s_timerNextThink = 0.0;
// The first Tick has no baseline to advance from (see Tick); Init() arms it again.
bool s_firstTick = true;

std::vector<Timer*> s_onceTimers;
std::vector<Timer*> s_repeatTimers;
std::mutex s_nextFrameMutex;
std::queue<std::function<void()>> s_nextFrameQueue;

} // namespace

double UniversalTime() {
	return s_universalTime;
}

void Init() {
	s_universalTime = 0.0;
	s_timerNextThink = 0.0;
	s_firstTick = true;
}

void Shutdown() {
	for (Timer* timer : s_onceTimers) {
		delete timer;
	}
	for (Timer* timer : s_repeatTimers) {
		delete timer;
	}
	s_onceTimers.clear();
	s_repeatTimers.clear();

	std::lock_guard lock(s_nextFrameMutex);
	std::queue<std::function<void()>> empty;
	std::swap(s_nextFrameQueue, empty);
}

void NextFrame(std::function<void()> task) {
	std::lock_guard lock(s_nextFrameMutex);
	s_nextFrameQueue.emplace(std::move(task));
}

Timer* AddTimer(float interval, std::function<void()> callback, int flags) {
	auto* timer = new Timer();
	timer->interval = interval;
	timer->execTime = s_universalTime + interval;
	timer->callback = std::move(callback);
	timer->flags = flags;
	if (flags & kTimerFlagRepeat) {
		s_repeatTimers.push_back(timer);
	} else {
		s_onceTimers.push_back(timer);
	}
	return timer;
}

void KillTimer(Timer* timer) {
	if (!timer) {
		return;
	}
	if (timer->inExec) {
		timer->killMe = true;
		return;
	}

	auto kill = [](std::vector<Timer*>& list, Timer* target) {
		auto it = std::remove(list.begin(), list.end(), target);
		if (it != list.end()) {
			delete target;
			list.erase(it, list.end());
		}
	};

	if (timer->flags & kTimerFlagRepeat) {
		kill(s_repeatTimers, timer);
	} else {
		kill(s_onceTimers, timer);
	}
}

void Tick(double deltaSeconds) {
	std::queue<std::function<void()>> local;
	{
		std::lock_guard lock(s_nextFrameMutex);
		std::swap(local, s_nextFrameQueue);
	}
	while (!local.empty()) {
		local.front()();
		local.pop();
	}

	// The first tick after Init has nothing to measure from: the delta it carries may cover the
	// time before this plugin's first update (the host's whole uptime), and adding that would fire
	// every timer at once - an empty-server map reload at each boot. The reference scheduler has
	// the same guard (`s_dLastTickTime > 0.0`). Next-frame tasks above still run on this tick.
	if (!s_firstTick && deltaSeconds > 0.0 && std::isfinite(deltaSeconds)) {
		s_universalTime += deltaSeconds;
	}
	s_firstTick = false;

	if (s_universalTime < s_timerNextThink) {
		return;
	}

	for (int i = static_cast<int>(s_onceTimers.size()) - 1; i >= 0; --i) {
		Timer* timer = s_onceTimers[i];
		if (s_universalTime >= timer->execTime) {
			timer->inExec = true;
			timer->callback();
			delete timer;
			s_onceTimers.erase(s_onceTimers.begin() + i);
		}
	}

	for (int i = static_cast<int>(s_repeatTimers.size()) - 1; i >= 0; --i) {
		Timer* timer = s_repeatTimers[i];
		if (s_universalTime >= timer->execTime) {
			timer->inExec = true;
			timer->callback();
			if (timer->killMe) {
				delete timer;
				s_repeatTimers.erase(s_repeatTimers.begin() + i);
				continue;
			}
			timer->inExec = false;
			timer->execTime = s_universalTime + timer->interval;
		}
	}

	s_timerNextThink = s_universalTime + 0.1;
}

} // namespace game_fixes::scheduler
