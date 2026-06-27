// license:GPLv3+

#pragma once

#include <string>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <unordered_map>
#include <chrono>

#include "events.pb.h"

namespace CadeBridge {

/// Schmitt-trigger debounce filter for zone-based game elements (triggers,
/// kickers) that produce hit/unhit pairs.
///
/// The algorithm:
///   - "hit" events are forwarded IMMEDIATELY (zero latency).
///   - "unhit" events are HELD for a configurable holdoff period.
///   - If a new "hit" arrives while an "unhit" is held, the pending "unhit"
///     is suppressed and the new "hit" is also suppressed (bounce detected).
///   - If the holdoff expires with no new "hit", the held "unhit" is forwarded
///     (real release).
///
/// This matches how hardware switch debouncers (e.g., FAST) behave:
/// impulse events (bumpers, spinners) pass through untouched, while zone
/// elements that exhibit contact bounce are cleaned up at the platform layer
/// so cade's game logic sees a uniform, clean signal regardless of driver.
class EventDebouncer
{
public:
   using ForwardFn = std::function<void(const cade::events::CategorizedEvent&)>;

   EventDebouncer();
   ~EventDebouncer();

   /// Start the debouncer with the given holdoff and forwarding function.
   /// Spawns a dedicated timer thread.
   void Start(double holdoffSeconds, ForwardFn forwardFn);

   /// Stop the timer thread and discard all pending events.
   void Stop();

   /// Submit a "hit" event. If there is a pending "unhit" for the same device
   /// key (bounce), both are suppressed. Otherwise the hit is forwarded
   /// immediately.
   void SubmitHit(const cade::events::CategorizedEvent& event);

   /// Submit an "unhit" event. It is held for the holdoff period. If no new
   /// "hit" arrives before the holdoff expires, it is forwarded.
   void SubmitUnhit(const cade::events::CategorizedEvent& event);

   /// Discard all pending events (e.g., on game end).
   void Clear();

private:
   void timerLoop();

   struct PendingUnhit {
      cade::events::CategorizedEvent event;
      std::chrono::steady_clock::time_point expiresAt;
   };

   ForwardFn m_forwardFn;
   double m_holdoffSeconds { 0.25 };

   std::mutex m_mutex;
   std::unordered_map<std::string, PendingUnhit> m_pending;
   std::condition_variable m_cv;

   std::thread m_timerThread;
   std::atomic<bool> m_running { false };
};

} // namespace CadeBridge
