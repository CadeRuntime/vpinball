// license:GPLv3+

#include "GrpcServer.h"

#include <chrono>
#include <set>
#include <sstream>
#include <ctime>

#include "plugins/LoggingPlugin.h"

namespace CadeBridge {

using namespace std::string_literals;

LPI_USE_CPP();
#define LOGI CadeBridge::LPI_LOGI_CPP
#define LOGW CadeBridge::LPI_LOGW_CPP
#define LOGE CadeBridge::LPI_LOGE_CPP

// ---------------------------------------------------------------------------
// GrpcServer
// ---------------------------------------------------------------------------

GrpcServer::GrpcServer() = default;

GrpcServer::~GrpcServer()
{
   Stop();
}

void GrpcServer::Start(int port, InboundCommandCallback callback, ReconnectCallback reconnectCb)
{
   if (m_running.load())
      return;

   m_callback = std::move(callback);
   m_reconnectCallback = std::move(reconnectCb);
   m_shutdownRequested.store(false);

   // Build server address.
   std::ostringstream addr;
   addr << "0.0.0.0:" << port;

   m_service = std::make_unique<PlatformServiceImpl>(*this);

   grpc::ServerBuilder builder;
   builder.AddListeningPort(addr.str(), grpc::InsecureServerCredentials());
   builder.RegisterService(m_service.get());

   // Keepalive: ping every 5min to detect dead connections.
   // The Go gRPC client's default EnforcementPolicy.MinTime is 5 minutes,
   // so pinging more aggressively triggers ENHANCE_YOUR_CALM and tears down
   // the transport. Use a generous timeout to handle localhost scheduling
   // jitter under heavy game-load (physics + rendering + audio).
   builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIME_MS, 5 * 60 * 1000);
   builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 10000);
   builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);

   // HTTP/2 transport hardening: increase concurrent streams and initial window size.
   // Default 64KB window can cause flow-control backpressure under bursty game event loads.
   builder.AddChannelArgument(GRPC_ARG_MAX_CONCURRENT_STREAMS, 100);
   builder.AddChannelArgument("grpc.http2.transport.send_ping", 1);
   builder.AddChannelArgument("grpc.initial_window_size", 1024 * 1024); // 1MB window

   m_server = builder.BuildAndStart();
   if (!m_server) {
      LOGE("CadeBridge: failed to start PlatformService on "s + addr.str());
      return;
   }

   m_running.store(true);

   // Run the gRPC event loop on dedicated threads, fully decoupled
   // from VPX's main loop and game lifecycle.
   // Multiple threads are required: the Go gRPC client sends periodic
   // HTTP/2 PING frames (keepalive every 6 min). With a single thread
   // blocked on stream->Read(), the sync server can't respond and the
   // Go client resets the connection after ~10 min.
   m_serverThreads.reserve(kServerThreadCount);
   for (int i = 0; i < kServerThreadCount; ++i) {
      m_serverThreads.emplace_back([this]() {
         m_server->Wait();
      });
   }

   LOGI("CadeBridge: PlatformService listening on "s + addr.str() + " (" + std::to_string(kServerThreadCount) + " handler threads)");
}

void GrpcServer::Stop()
{
   if (!m_running.load())
      return;

   LOGI("CadeBridge: shutting down PlatformService"s);

   m_shutdownRequested.store(true);

   // Wake queue waiters so they can exit.
   m_sendCv.notify_all();
   m_recvCv.notify_all();
   m_connectionCv.notify_all();

   // Clear active stream so QueueEvent stops trying to write.
   {
      std::lock_guard<std::mutex> lock(m_streamMutex);
      m_activeStream = nullptr;
   }

   if (m_server) {
      // Deadline-based shutdown to avoid blocking indefinitely.
      auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(3);
      m_server->Shutdown(deadline);
   }

   // Wait for all server threads to exit (Shutdown causes Wait to return).
   for (auto& t : m_serverThreads) {
      if (t.joinable())
         t.join();
   }
   m_serverThreads.clear();

   m_server.reset();
   m_service.reset();
   m_running.store(false);
   m_gameActive.store(false);
   m_connectionState.store(ConnectionState::Disconnected);

   LOGI("CadeBridge: PlatformService stopped"s);
}

void GrpcServer::SetManifest(const cade::events::DeviceManifest& manifest)
{
   std::lock_guard<std::mutex> lock(m_manifestMutex);
   m_manifest = manifest;
}

void GrpcServer::QueueEvent(const cade::events::PlatformEvent& event)
{
   {
      std::lock_guard<std::mutex> lock(m_sendMutex);
      if (m_sendQueue.size() >= kMaxSendQueueSize) {
         // Drop oldest to prevent unbounded growth.
         m_sendQueue.pop();
      }
      m_sendQueue.push(event);
   }
   m_sendCv.notify_one();
}

void GrpcServer::QueueDeviceEvent(const cade::events::CategorizedEvent& event)
{
   cade::events::PlatformEvent pe;
   *pe.mutable_device_event() = event;
   QueueEvent(pe);
}

void GrpcServer::DrainSendQueue()
{
   std::lock_guard<std::mutex> lock(m_sendMutex);
   std::queue<cade::events::PlatformEvent> empty;
   m_sendQueue.swap(empty);
}

// ---------------------------------------------------------------------------
// PlatformServiceImpl
// ---------------------------------------------------------------------------

GrpcServer::PlatformServiceImpl::PlatformServiceImpl(GrpcServer& owner)
   : m_owner(owner)
{
}

grpc::Status GrpcServer::PlatformServiceImpl::GetDeviceManifest(
   grpc::ServerContext* /*context*/,
   const cade::events::GetDeviceManifestRequest* request,
   cade::events::DeviceManifest* response)
{
   std::lock_guard<std::mutex> lock(m_owner.m_manifestMutex);

   // If categories filter is specified, only include matching devices.
   if (request->categories_size() > 0) {
      response->set_game_id(m_owner.m_manifest.game_id());
      response->set_vpx_version(m_owner.m_manifest.vpx_version());

      std::set<cade::events::DeviceCategory> wanted;
      for (int cat : request->categories())
         wanted.insert(static_cast<cade::events::DeviceCategory>(cat));

      for (const auto& dev : m_owner.m_manifest.devices()) {
         if (wanted.count(dev.category())) {
            *response->add_devices() = dev;
         }
      }
   }
   else {
      *response = m_owner.m_manifest;
   }

   LOGI("CadeBridge: GetDeviceManifest returning "s + std::to_string(response->devices_size()) + " devices");

   return grpc::Status::OK;
}

grpc::Status GrpcServer::PlatformServiceImpl::PlatformEventFlow(
   grpc::ServerContext* context,
   grpc::ServerReaderWriter<cade::events::PlatformEvent,
                            cade::events::PlatformCommand>* stream)
{
   LOGI("CadeBridge: PlatformEventFlow stream opened by cade (peer="s + context->peer() + ")");

   // Drain stale events from any previous session.
   m_owner.DrainSendQueue();

   // Register as the active stream for outbound events.
   {
      std::lock_guard<std::mutex> lock(m_owner.m_streamMutex);
      m_owner.m_activeStream = stream;
   }

   // Update connection state.
   {
      std::lock_guard<std::mutex> lock(m_owner.m_connectionMutex);
      m_owner.m_connectionState.store(ConnectionState::Connected);
      m_owner.m_connectionGeneration++;
      m_owner.m_connectionCv.notify_all();
   }

   // Send initial connected status.
   {
      cade::events::PlatformEvent statusEvent;
      auto* status = statusEvent.mutable_status();
      status->set_state(cade::events::StreamStatus::STATE_CONNECTED);
      stream->Write(statusEvent);
   }

   // Push current device manifest through the stream.
   // This eliminates the need for a separate GetDeviceManifest unary RPC
   // on the same connection, which would conflict with the active stream
   // in grpc++'s sync server model.
   {
      cade::events::DeviceManifest currentManifest;
      {
         std::lock_guard<std::mutex> lock(m_owner.m_manifestMutex);
         currentManifest = m_owner.m_manifest;
      }
      if (currentManifest.devices_size() > 0) {
         cade::events::PlatformEvent manifestEvent;
         *manifestEvent.mutable_manifest() = currentManifest;
         if (!stream->Write(manifestEvent)) {
            LOGW("CadeBridge: failed to send manifest on stream open");
         } else {
            LOGI("CadeBridge: sent manifest on stream open, devices="
                 + std::to_string(currentManifest.devices_size()));
         }
      }
   }

   // Invoke reconnect callback so CadeBridge can push fresh state.
   if (m_owner.m_reconnectCallback)
      m_owner.m_reconnectCallback();

   // --- Single-thread stream I/O with pre/post-Read flush ---
   //
   // grpc++ sync server on Windows corrupts internal state when Read() and
   // Write() happen simultaneously on the same ServerReaderWriter, causing
   // TCP RST after a few seconds. ALL stream I/O must happen on one thread.
   //
   // Previous approaches and why they failed:
   //   1. Single-thread I/O (pre-Read flush only): no TCP RST, but config
   //      ACK queued during Read() was never sent → 5s timeout.
   //   2. Mutex-serialized dual-thread: reader held mutex during blocking
   //      Read(), permanently starving the flusher.
   //   3. Atomic inRead flag + flusher spin: flusher could never write while
   //      reader was in Read(). Post-Read flush still needed.
   //
   // Current approach: single reader thread with dual flush points.
   //   - Pre-Read flush: delivers events queued before Read() (manifest,
   //     table_ready, game events during active command flow)
   //   - Post-Read flush: delivers events queued DURING Read() (config ACK
   //     from dispatchThread, any events queued while reader was blocked)
   //   - Go client sends a wake-up command after config update to trigger
   //     the post-Read flush (see SendConfigUpdate in platform_client.go)
   //
   // readerThread loop:
   //   1. Flush pending events (pre-Read) — delivers manifest, game events
   //   2. stream->Read()  (blocking until Go sends a command)
   //   3. Flush pending events (post-Read) — delivers config ACK, etc.
   //   4. Enqueue command for dispatchThread
   //   → repeat
   //
   // dispatchThread: processes commands via VPX API (unchanged)
   // handler thread: joins readerThread and returns (frees pool slot)
   //
   // Why this works:
   //   - Zero concurrent access: single thread touches the stream
   //   - Pre-Read flush delivers manifest/table_ready before first Read()
   //   - Post-Read flush delivers config ACK after dispatchThread processes
   //   - Go wake-up command triggers post-Read flush for ACK delivery
   //   - During active gameplay: both flushes deliver events in <1ms

   std::atomic<bool> streamDone { false };
   int readCount = 0;
   int writeCount = 0;

   // Helper: flush pending outbound events via the stream.
   // Returns true if stream is still healthy, false if Write failed.
   auto flushEvents = [&](const char* source) -> bool {
      std::lock_guard<std::mutex> lock(m_owner.m_sendMutex);
      while (!m_owner.m_sendQueue.empty()) {
         auto event = std::move(m_owner.m_sendQueue.front());
         m_owner.m_sendQueue.pop();
         writeCount++;
         if (!stream->Write(event)) {
            LOGW("CadeBridge: stream Write failed during "s + source
               + " flush, cancelled="s + std::to_string(context->IsCancelled()));
            return false;
         }
      }
      return true;
   };

   // --- dispatchThread: process commands from the recv queue ---
   // VPX API calls happen here, NOT in the reader thread.
   std::thread dispatchThread([&]() {
      try {
         while (!streamDone.load() && !m_owner.m_shutdownRequested.load()) {
            cade::events::PlatformCommand cmd;
            {
               std::unique_lock<std::mutex> lock(m_owner.m_recvMutex);
               m_owner.m_recvCv.wait_for(lock, std::chrono::milliseconds(100), [&]() {
                  return !m_owner.m_recvQueue.empty() ||
                         streamDone.load() ||
                         m_owner.m_shutdownRequested.load();
               });

               if (m_owner.m_recvQueue.empty())
                  continue;

               cmd = std::move(m_owner.m_recvQueue.front());
               m_owner.m_recvQueue.pop();
            }

            if (m_owner.m_callback)
               m_owner.m_callback(cmd);
         }
      }
      catch (const std::exception& e) {
         LOGE("CadeBridge: dispatchThread exception: "s + e.what());
         streamDone.store(true);
         m_owner.m_recvCv.notify_all();
      }
   });

   // --- readerThread: ALL stream I/O on this single thread ---
   // Dual flush points ensure events are delivered both before and after
   // each blocking Read(). No concurrent access to the stream object.
   std::thread readerThread([&]() {
      try {
         while (!streamDone.load() && !m_owner.m_shutdownRequested.load()) {
            // --- Pre-Read flush ---
            // Deliver events queued before this Read() (manifest, table_ready,
            // game events from active gameplay, etc.).
            if (!flushEvents("pre-Read")) {
               streamDone.store(true);
               break;
            }

            // Block on Read until the next command arrives.
            // This is the ONLY thread that accesses the stream object.
            cade::events::PlatformCommand cmd;
            bool ok = stream->Read(&cmd);
            readCount++;

            if (ok) {
               // --- Post-Read flush ---
               // Deliver events queued DURING Read() (config ACK from
               // dispatchThread, etc.). Go client sends a wake-up command
               // after config update to trigger this flush.
               if (!flushEvents("post-Read")) {
                  streamDone.store(true);
                  break;
               }

               // Enqueue command for dispatchThread.
               {
                  std::lock_guard<std::mutex> lock(m_owner.m_recvMutex);
                  if (m_owner.m_recvQueue.size() >= m_owner.kMaxRecvQueueSize)
                     m_owner.m_recvQueue.pop();
                  m_owner.m_recvQueue.push(std::move(cmd));
               }
               m_owner.m_recvCv.notify_one();
            } else {
               // EOF — stream closed by client.
               streamDone.store(true);
            }
         }
      }
      catch (const std::exception& e) {
         LOGE("CadeBridge: readerThread exception: "s + e.what());
         streamDone.store(true);
      }
   });

   // Handler thread waits for readerThread to finish.
   // This frees the gRPC thread pool slot once the stream is done.
   readerThread.join();

   // Stream closed (cade disconnected or shutdown).
   streamDone.store(true);
   m_owner.m_sendCv.notify_all();
   m_owner.m_recvCv.notify_all();
   dispatchThread.join();

   {
      std::lock_guard<std::mutex> lock(m_owner.m_streamMutex);
      if (m_owner.m_activeStream == stream)
         m_owner.m_activeStream = nullptr;
   }

   // Update connection state.
   {
      std::lock_guard<std::mutex> lock(m_owner.m_connectionMutex);
      m_owner.m_connectionState.store(ConnectionState::Disconnected);
      m_owner.m_connectionCv.notify_all();
   }

   // Summarize why the stream closed.
   LOGW("CadeBridge: PlatformEventFlow stream closed -- cancelled="s
      + std::to_string(context->IsCancelled())
      + " reads="s + std::to_string(readCount)
      + " writes="s + std::to_string(writeCount)
      + " peer="s + context->peer());
   LOGI("CadeBridge: server still listening for reconnect"s);
   return grpc::Status::OK;
}

grpc::Status GrpcServer::PlatformServiceImpl::PlatformHealthCheck(
   grpc::ServerContext* /*context*/,
   const google::protobuf::Empty* /*request*/,
   cade::events::HealthCheckResponse* response)
{
   response->set_status(cade::events::HealthCheckResponse::SERVING_STATUS_SERVING);
   response->set_version("cade-bridge-1.0");
   return grpc::Status::OK;
}

} // namespace CadeBridge
