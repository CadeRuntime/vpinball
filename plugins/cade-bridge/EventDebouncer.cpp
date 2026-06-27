// license:GPLv3+

#include "EventDebouncer.h"

#include <vector>

#include "plugins/LoggingPlugin.h"

namespace CadeBridge {

using namespace std::string_literals;

LPI_USE_CPP();
#define LOGD CadeBridge::LPI_LOGD_CPP

EventDebouncer::EventDebouncer() = default;

EventDebouncer::~EventDebouncer()
{
   Stop();
}

void EventDebouncer::Start(double holdoffSeconds, ForwardFn forwardFn)
{
   if (m_running.load())
      return;

   m_holdoffSeconds = holdoffSeconds;
   m_forwardFn = std::move(forwardFn);
   m_running.store(true);
   m_timerThread = std::thread([this]() { timerLoop(); });
}

void EventDebouncer::Stop()
{
   if (!m_running.load())
      return;

   m_running.store(false);
   m_cv.notify_all();

   if (m_timerThread.joinable())
      m_timerThread.join();

   std::lock_guard<std::mutex> lock(m_mutex);
   m_pending.clear();
}

void EventDebouncer::SubmitHit(const cade::events::CategorizedEvent& event)
{
   const std::string& key = event.device_key();

   {
      std::lock_guard<std::mutex> lock(m_mutex);
      auto it = m_pending.find(key);
      if (it != m_pending.end()) {
         // Bounce detected: an "unhit" is pending and a new "hit" arrived
         // before the holdoff expired. Suppress both.
         LOGD("CadeBridge: debounce suppressed bounce on "s + key);
         m_pending.erase(it);
         return;
      }
   }

   // No pending unhit — forward immediately.
   if (m_forwardFn)
      m_forwardFn(event);
}

void EventDebouncer::SubmitUnhit(const cade::events::CategorizedEvent& event)
{
   const std::string& key = event.device_key();
   auto holdoff = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::duration<double>(m_holdoffSeconds));

   {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_pending[key] = PendingUnhit { event, std::chrono::steady_clock::now() + holdoff };
   }
   m_cv.notify_one();
}

void EventDebouncer::Clear()
{
   std::lock_guard<std::mutex> lock(m_mutex);
   m_pending.clear();
}

void EventDebouncer::timerLoop()
{
   while (m_running.load()) {
      std::vector<cade::events::CategorizedEvent> toForward;
      auto nextWake = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);

      {
         std::lock_guard<std::mutex> lock(m_mutex);
         auto now = std::chrono::steady_clock::now();

         for (auto it = m_pending.begin(); it != m_pending.end(); ) {
            if (now >= it->second.expiresAt) {
               toForward.push_back(std::move(it->second.event));
               it = m_pending.erase(it);
            }
            else {
               if (it->second.expiresAt < nextWake)
                  nextWake = it->second.expiresAt;
               ++it;
            }
         }
      }

      // Forward expired unhit events outside the lock.
      for (auto& event : toForward) {
         LOGD("CadeBridge: debounce forwarding unhit "s + event.device_key());
         if (m_forwardFn)
            m_forwardFn(event);
      }

      // Sleep until the next pending event expires or a new event arrives.
      std::unique_lock<std::mutex> lock(m_mutex);
      m_cv.wait_until(lock, nextWake, [this]() {
         return !m_running.load();
      });
   }
}

} // namespace CadeBridge
