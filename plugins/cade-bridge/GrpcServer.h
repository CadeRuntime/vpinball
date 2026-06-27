// license:GPLv3+

#pragma once

#include <string>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <queue>
#include <memory>
#include <vector>

#include <grpcpp/grpcpp.h>
#include "events.grpc.pb.h"

namespace CadeBridge {

// Callback invoked when cade sends a command through the PlatformEventFlow stream.
using InboundCommandCallback = std::function<void(const cade::events::PlatformCommand&)>;

// Callback invoked when cade (re)connects a new PlatformEventFlow stream.
using ReconnectCallback = std::function<void()>;

enum class ConnectionState {
   Disconnected, // No active PlatformEventFlow stream
   Connected,    // Stream is active, cade is connected
};

/// GrpcServer hosts a PlatformService that the cade orchestrator connects to.
/// It serves the device manifest, handles bidirectional event/command streams,
/// and dispatches inbound commands (device actuation, config updates) to the
/// plugin via a callback.
///
/// The server runs on a dedicated background thread, fully decoupled from the
/// VPX main loop. It starts at plugin load and persists across game boundaries,
/// allowing cade to maintain a connection (or reconnect) without being affected
/// by game start/end cycles.
class GrpcServer
{
public:
   GrpcServer();
   ~GrpcServer();

   /// Start the gRPC server on a dedicated background thread. Non-blocking.
   /// @param port  TCP port to listen on (e.g. 50052).
   /// @param callback  Invoked for each inbound PlatformCommand from cade.
   /// @param reconnectCb  Invoked when cade (re)connects a new stream.
   void Start(int port, InboundCommandCallback callback, ReconnectCallback reconnectCb = nullptr);

   /// Gracefully shut down the server and close all streams.
   void Stop();

   /// Set the device manifest that will be returned by GetDeviceManifest.
   /// Thread-safe; can be called at any time.
   void SetManifest(const cade::events::DeviceManifest& manifest);

   /// Enqueue an outbound event (device hit, state change) for sending to cade.
   /// Non-blocking; drops the event if the send buffer is full.
   void QueueEvent(const cade::events::PlatformEvent& event);

   /// Convenience: wrap a CategorizedEvent in a PlatformEvent and queue it.
   void QueueDeviceEvent(const cade::events::CategorizedEvent& event);

   /// Drain all pending events from the send queue.
   void DrainSendQueue();

   /// Notify the server that a game has started/ended.
   /// Does NOT start or stop the server itself.
   void SetGameActive(bool active) { m_gameActive.store(active); }

   bool IsRunning() const { return m_running.load(); }
   bool IsGameActive() const { return m_gameActive.load(); }
   bool IsConnected() const { return m_connectionState.load() == ConnectionState::Connected; }
   ConnectionState GetConnectionState() const { return m_connectionState.load(); }

private:
   // PlatformServiceImpl implements the generated PlatformService::Service.
   class PlatformServiceImpl final : public cade::events::PlatformService::Service
   {
   public:
      PlatformServiceImpl(GrpcServer& owner);

      grpc::Status GetDeviceManifest(
         grpc::ServerContext* context,
         const cade::events::GetDeviceManifestRequest* request,
         cade::events::DeviceManifest* response) override;

      grpc::Status PlatformEventFlow(
         grpc::ServerContext* context,
         grpc::ServerReaderWriter<cade::events::PlatformEvent,
                                  cade::events::PlatformCommand>* stream) override;

      grpc::Status PlatformHealthCheck(
         grpc::ServerContext* context,
         const google::protobuf::Empty* request,
         cade::events::HealthCheckResponse* response) override;

   private:
      GrpcServer& m_owner;
   };

   // Manifest storage (set before start, read by RPC handler).
   std::mutex m_manifestMutex;
   cade::events::DeviceManifest m_manifest;

   // Outbound event queue (platform events → cade).
   std::mutex m_sendMutex;
   std::condition_variable m_sendCv;
   std::queue<cade::events::PlatformEvent> m_sendQueue;
   static constexpr size_t kMaxSendQueueSize = 1024;

   // Inbound command queue (cade commands → VPX). Decouples gRPC recv
   // thread from command processing so VPX API calls don't stall the stream.
   std::mutex m_recvMutex;
   std::condition_variable m_recvCv;
   std::queue<cade::events::PlatformCommand> m_recvQueue;
   static constexpr size_t kMaxRecvQueueSize = 256;

   // Inbound command callback.
   InboundCommandCallback m_callback;

   // Reconnect callback.
   ReconnectCallback m_reconnectCallback;

   // gRPC server.
   std::unique_ptr<grpc::Server> m_server;
   std::unique_ptr<PlatformServiceImpl> m_service;

   // Dedicated server thread pool for handling RPCs and keepalive pings.
   // The Go gRPC client sends periodic HTTP/2 PING frames (every 6 min).
   // With a single thread blocked on stream->Read(), the sync server
   // can't respond to pings and the Go client resets the connection
   // after ~10 min. A pool of threads ensures keepalive pings are
   // processed even when handler threads are blocked in Read().
   static constexpr int kServerThreadCount = 4;
   std::vector<std::thread> m_serverThreads;

   // Lifecycle.
   std::atomic<bool> m_running { false };
   std::atomic<bool> m_shutdownRequested { false };

   // Game-active flag: whether a game is currently running.
   std::atomic<bool> m_gameActive { false };

   // Connection state tracking.
   std::atomic<ConnectionState> m_connectionState { ConnectionState::Disconnected };
   std::mutex m_connectionMutex;
   std::condition_variable m_connectionCv;
   uint64_t m_connectionGeneration { 0 };

   // Track active stream for sending.
   std::mutex m_streamMutex;
   grpc::ServerReaderWriter<cade::events::PlatformEvent,
                            cade::events::PlatformCommand>* m_activeStream { nullptr };
};

} // namespace CadeBridge
