// license:GPLv3+

///////////////////////////////////////////////////////////////////////////////
// Cade Bridge plugin
//
// Bridges VPX's in-process plugin system with Cade's (Cade Runtime)
// gRPC EventService via bidirectional EventFlow streaming. Either side can
// originate events: VPX switch/device state changes flow to Cade, and Cade
// flipper/system events flow back to VPX.

#include "plugins/MsgPlugin.h"
#include "plugins/VPXPlugin.h"
#include "plugins/ControllerPlugin.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
using namespace std::string_literals;
#include <vector>
#include <mutex>

// Shared logging
#include "plugins/LoggingPlugin.h"

#include "GrpcServer.h"
#include "EventMapper.h"
#include "DeviceRegistry.h"
#include "EventDebouncer.h"

namespace CadeBridge {

LPI_USE_CPP();
#define LOGD CadeBridge::LPI_LOGD_CPP
#define LOGI CadeBridge::LPI_LOGI_CPP
#define LOGW CadeBridge::LPI_LOGW_CPP
#define LOGE CadeBridge::LPI_LOGE_CPP

///////////////////////////////////////////////////////////////////////////////
// Plugin state

const MsgPluginAPI* msgApi = nullptr;
VPXPluginAPI* vpxApi = nullptr;

uint32_t endpointId;
unsigned int getVpxApiId;
unsigned int onGameStartId, onGameEndId;
unsigned int onCtlGameStartId, onCtlGameEndId;
unsigned int getInputSrcId, getDevSrcId;
unsigned int onInputSrcChgId, onDevSrcChgId;
unsigned int onDisplaySrcChgId, onSegSrcChgId;
unsigned int onActionChangedId;
unsigned int onGameElementId;
unsigned int getGameElementsMsgId;

GrpcServer grpcServer;
DeviceRegistry deviceRegistry;
EventDebouncer elementDebouncer;

// Tracked input and device sources for change callbacks
struct TrackedInputSrc
{
   InputSrcId src;
};

struct TrackedDevSrc
{
   DevSrcId src;
};

std::mutex sourcesMutex;
std::vector<TrackedInputSrc> trackedInputs;
std::vector<TrackedDevSrc> trackedDevices;

// True while the game-active subscriptions registered in onGameStart are live.
// Guards teardown so unloading the plugin mid-game performs the same cleanup
// onGameEnd would have, instead of leaking those callbacks.
bool gameSubscriptionsActive = false;

///////////////////////////////////////////////////////////////////////////////
// Settings

// Note: the plugin's on/off is the core per-plugin "Enable" toggle (registered by
// VPX, which loads/unloads this plugin). No separate "Enabled" setting is needed.
MSGPI_INT_VAL_SETTING(listenPortProp, "ListenPort", "ListenPort", "PlatformService listen port", true, 0, 0xFFFF, 50052);
MSGPI_INT_VAL_SETTING(debounceMsProp, "DebounceMs", "DebounceMs", "Element event debounce holdoff in milliseconds (0 to disable)", true, 0, 5000, 250);

///////////////////////////////////////////////////////////////////////////////
// Transport helpers - route events through active transport

static void sendCadeEvent(const cade::events::CategorizedEvent& event)
{
   grpcServer.QueueDeviceEvent(event);
}

///////////////////////////////////////////////////////////////////////////////
// Input/device change callbacks - forward state changes to Cade

static void onInputChanged(unsigned int inputIndex, void* context)
{
   auto* src = static_cast<InputSrcId*>(context);
   if (!src || !src->GetInputState)
      return;
   int state = src->GetInputState(inputIndex);
   auto event = MapInputToCade(*src, inputIndex, state, &deviceRegistry);
   LOGI("CadeBridge: input changed -> device_key="s + event.device_key() + " state=" + std::to_string(state));
   sendCadeEvent(event);
}

static void onDeviceChanged(unsigned int deviceIndex, void* context)
{
   auto* src = static_cast<DevSrcId*>(context);
   if (!src)
      return;
   auto event = MapDeviceToCade(*src, deviceIndex, &deviceRegistry);
   LOGI("CadeBridge: device changed -> device_key="s + event.device_key() + " category=" + std::to_string(event.device_category()));
   sendCadeEvent(event);
}

///////////////////////////////////////////////////////////////////////////////
// VPX action change callback - forward flipper/start/coin/etc to Cade

static void onActionChanged(const unsigned int eventId, void* userData, void* eventData)
{
   auto* actionEvent = static_cast<VPXActionEvent*>(eventData);
   if (!actionEvent)
      return;

   auto event = MapActionToCade(actionEvent->action, actionEvent->isPressed, &deviceRegistry);
   const std::string& deviceKey = event.device_key();

   // Check if this action triggers local autofire rules (e.g., flippers).
   // For "hold" type rules, activate the coil on press and deactivate on release.
   // One action can drive multiple coils — e.g. two right flippers sharing the
   // right_flipper button — so fire every rule bound to the switch. Only enabled
   // rules fire: cade sends Enable/DisableAutofire to gate coil activation during
   // tilt, ball save, or inter-ball intervals, and because each coil carries its
   // own rule, the coils sharing an action can be inhibited independently.
   if (vpxApi && vpxApi->SetFlipperState)
   {
      for (const auto* rule : deviceRegistry.GetAutofireRulesForSwitch(deviceKey))
      {
         if (!rule->enabled || rule->type != "hold")
            continue;
         int result = vpxApi->SetFlipperState(rule->coilName.c_str(), actionEvent->isPressed ? 1 : 0);
         if (result != 0)
            LOGW("CadeBridge: autofire flipper failed for '"s + rule->coilName + "', result=" + std::to_string(result));
         else
            LOGD("CadeBridge: autofire "s + (actionEvent->isPressed ? "activate" : "deactivate") + " '" + rule->coilName + "'");
      }
   }

   LOGI("CadeBridge: action -> "s + deviceKey + " " + event.event_type_name());
   sendCadeEvent(event);
}

///////////////////////////////////////////////////////////////////////////////
// VPX game element event callback - forward bumper hits, target drops, etc. to Cade

// Filter out non-interactive element types that shouldn't generate Cade events
static bool IsForwardableElementType(int elementType)
{
   // ItemTypeEnum values from iselect.h
   switch (elementType)
   {
   case 0:  // eItemSurface — slingshots and config-referenced surfaces; plain walls
            // are filtered downstream in onGameElement (see IsConfiguredDevice gate)
   case 1:  // eItemFlipper
   case 3:  // eItemPlunger
   case 5:  // eItemBumper
   case 6:  // eItemTrigger
   case 7:  // eItemLight
   case 8:  // eItemKicker
   case 10: // eItemGate
   case 11: // eItemSpinner
   case 22: // eItemHitTarget
      return true;
   default:
      return false; // walls, rubbers, primitives, ramps, decals, etc.
   }
}

// Only trigger-type elements (rollovers, opto switches, lane sensors) are
// debounced. Triggers exhibit mechanical contact bounce — a ball rolling over
// one can cause a rapid HIT→UNHIT→HIT sequence — so the debouncer suppresses
// the intermediate spurious UNHIT+HIT pair.
//
// Kickers (trough, scoop, VUK) are NOT debounced: each HIT is a deliberate
// ball-capture event. After a ball is destroyed in the kicker, the resulting
// UNHIT is artificial (not mechanical bounce), and holding it for the debounce
// window would suppress the next ball's HIT if it arrives within that window.
static bool IsZoneElement(int elementType)
{
   switch (elementType)
   {
   case 6:  // eItemTrigger — rollovers, opto switches, lane sensors
      return true;
   default:
      return false;
   }
}

static void onGameElement(const unsigned int eventId, void* userData, void* eventData)
{
   auto* elemEvent = static_cast<VPXGameElementEvent*>(eventData);
   if (!elemEvent || !elemEvent->elementName)
      return;

   if (!IsForwardableElementType(elemEvent->elementType))
      return;

   // Autofire-registered devices (bumpers, slingshots, flippers) are only forwarded
   // while their rule is enabled. cade controls the lifecycle via Enable/DisableAutofire
   // commands — e.g. disable during tilt, ball save, or between balls.
   const auto* autofireRule = deviceRegistry.GetAutofireRuleForSwitch(elemEvent->elementName);
   if (autofireRule && !autofireRule->enabled)
      return;

   // Surfaces (eItemSurface) are forwardable to capture named slingshots, but the
   // type alone can't tell a slingshot from a plain wall. A wall with "Has Hit
   // Event" enabled (e.g. Wall37) would otherwise emit a switch.hit on every
   // ball-wall collision — a flood of events cade has no rule for. Forward a
   // surface only when it has a cade role: an autofire rule (slingshots) or a
   // resolved scoring/event-name trigger (surfaces referenced by the .cade config).
   if (elemEvent->elementType == 0 /* eItemSurface */
       && !autofireRule
       && !deviceRegistry.IsConfiguredDevice(elemEvent->elementName))
      return;

   auto event = MapGameElementToCade(elemEvent->elementName, elemEvent->eventId, elemEvent->elementType, &deviceRegistry);
   LOGI("CadeBridge: element -> "s + event.device_key() + " " + event.event_type_name());

   // Only zone elements (triggers/kickers) get debounced — they produce
   // hit/unhit pairs and can bounce. Everything else forwards immediately.
   if (debounceMsProp_Val > 0 && IsZoneElement(elemEvent->elementType))
   {
      if (elemEvent->eventId == VPXELEMENT_EVT_HIT)
         elementDebouncer.SubmitHit(event);
      else if (elemEvent->eventId == VPXELEMENT_EVT_UNHIT)
         elementDebouncer.SubmitUnhit(event);
      else
         sendCadeEvent(event); // other events on zone elements pass through
   }
   else
   {
      sendCadeEvent(event);
   }
}

///////////////////////////////////////////////////////////////////////////////
// Source discovery - query and track available inputs/devices

static void discoverInputSources()
{
   std::lock_guard<std::mutex> lock(sourcesMutex);
   trackedInputs.clear();

   // First pass: get count
   GetInputSrcMsg msg {};
   msg.maxEntryCount = 0;
   msg.count = 0;
   msg.entries = nullptr;
   msgApi->BroadcastMsg(endpointId, getInputSrcId, &msg);

   if (msg.count == 0)
   {
      LOGI("CadeBridge: no input sources found"s);
      return;
   }

   // Second pass: get entries
   unsigned int count = msg.count;
   std::vector<InputSrcId> entries(count);
   msg.maxEntryCount = count;
   msg.count = 0;
   msg.entries = entries.data();
   msgApi->BroadcastMsg(endpointId, getInputSrcId, &msg);

   unsigned int actual = std::min(msg.count, count);
   trackedInputs.resize(actual);
   for (unsigned int i = 0; i < actual; i++)
   {
      trackedInputs[i].src = entries[i];

      // Register change callbacks for each input in this source
      if (entries[i].SetChangeCallback)
      {
         for (unsigned int j = 0; j < entries[i].nInputs; j++)
            entries[i].SetChangeCallback(j, 1, onInputChanged, &trackedInputs[i].src);
      }
   }

   LOGI("CadeBridge: discovered "s + std::to_string(actual) + " input source(s)");
}

static void discoverDeviceSources()
{
   std::lock_guard<std::mutex> lock(sourcesMutex);
   trackedDevices.clear();

   // First pass: get count
   GetDevSrcMsg msg {};
   msg.maxEntryCount = 0;
   msg.count = 0;
   msg.entries = nullptr;
   msgApi->BroadcastMsg(endpointId, getDevSrcId, &msg);

   if (msg.count == 0)
   {
      LOGI("CadeBridge: no device sources found"s);
      return;
   }

   // Second pass: get entries
   unsigned int count = msg.count;
   std::vector<DevSrcId> entries(count);
   msg.maxEntryCount = count;
   msg.count = 0;
   msg.entries = entries.data();
   msgApi->BroadcastMsg(endpointId, getDevSrcId, &msg);

   unsigned int actual = std::min(msg.count, count);
   trackedDevices.resize(actual);
   for (unsigned int i = 0; i < actual; i++)
   {
      trackedDevices[i].src = entries[i];

      // Register change callbacks for each device in this source
      if (entries[i].SetChangeCallback)
      {
         for (unsigned int j = 0; j < entries[i].nDevices; j++)
            entries[i].SetChangeCallback(j, 1, onDeviceChanged, &trackedDevices[i].src);
      }
   }

   LOGI("CadeBridge: discovered "s + std::to_string(actual) + " device source(s)");
}

static void sendInitialStateSnapshot()
{
   std::lock_guard<std::mutex> lock(sourcesMutex);

   for (auto& tracked : trackedInputs)
   {
      if (!tracked.src.GetInputState)
         continue;
      for (unsigned int i = 0; i < tracked.src.nInputs; i++)
      {
         int state = tracked.src.GetInputState(i);
         if (state) // Only send active states
            sendCadeEvent(MapInputToCade(tracked.src, i, state, &deviceRegistry));
      }
   }

   for (auto& tracked : trackedDevices)
   {
      if (!tracked.src.GetByteState)
         continue;
      for (unsigned int i = 0; i < tracked.src.nDevices; i++)
      {
         uint8_t state = tracked.src.GetByteState(i);
         if (state) // Only send active states
            sendCadeEvent(MapDeviceToCade(tracked.src, i, &deviceRegistry));
      }
   }
}

///////////////////////////////////////////////////////////////////////////////
// Device command handling - process outbound commands from Cade

static void handleDeviceCommand(const cade::events::DeviceCommand& cmd)
{
   if (cmd.has_enable_autofire())
   {
      deviceRegistry.EnableAutofireRule(cmd.enable_autofire().rule_name());
   }
   else if (cmd.has_disable_autofire())
   {
      deviceRegistry.DisableAutofireRule(cmd.disable_autofire().rule_name());
   }
   else if (cmd.has_pulse_coil())
   {
      const auto& coil = cmd.pulse_coil();
      const std::string& name = coil.device_name();

      // Autofire-managed coils (slingshots, bumpers, kickbacks) are fired by
      // VPX directly from the physics side. Suppress any pulse from cade to
      // avoid a double-fire.
      if (deviceRegistry.IsAutofireCoil(name))
      {
         LOGD("CadeBridge: suppressing PulseCoil for autofire-managed coil '"s + name + "'");
         return;
      }

      // The VPX plugin API has no generic "pulse coil" primitive — every
      // pulsable element has its own typed entry point. Dispatch based on the
      // device category established during manifest exchange.
      //
      //   BALL_DEVICE (kicker)    → NOT dispatched. Use the `kick_ball` cade
      //                             action instead, which maps to this
      //                             bridge's KickBallCommand handler below
      //                             and calls VPXPluginAPI::KickBall (kick-
      //                             only, no CreateBall). `pulse_coil` has
      //                             no meaningful semantics on a kicker —
      //                             the old EjectBall-based route spawned a
      //                             phantom ball on every pulse.
      //   FLIPPER                 → Not pulsable — warn and direct caller to
      //                             `enable_coil` which maps to SetFlipperState.
      //   SWITCH/COIL/GENERAL     → Best-effort SetDropTargetState(raise). If
      //                             the named element is a HitTarget the drop
      //                             resets; otherwise VPX returns -2/-3 and we
      //                             log and move on.
      cade::events::DeviceCategory cat = deviceRegistry.GetDeviceCategory(name);
      int result = -1;
      const char* action = "none";

      switch (cat)
      {
      case cade::events::DEVICE_CATEGORY_BALL_DEVICE:
         LOGW("CadeBridge: PulseCoil '"s + name + "' targets a kicker/saucer -- "
            + "use the `kick_ball` action instead. Command dropped.");
         return;

      case cade::events::DEVICE_CATEGORY_FLIPPER:
         LOGW("CadeBridge: PulseCoil '"s + name + "' targets a flipper -- use enable_coil instead");
         return;

      case cade::events::DEVICE_CATEGORY_SWITCH:
      case cade::events::DEVICE_CATEGORY_COIL:
      case cade::events::DEVICE_CATEGORY_GENERAL:
      case cade::events::DEVICE_CATEGORY_UNSPECIFIED:
      default:
         if (vpxApi && vpxApi->SetDropTargetState)
         {
            result = vpxApi->SetDropTargetState(name.c_str(), 0);
            action = "SetDropTargetState(raise)";
         }
         break;
      }

      if (result == 0)
      {
         LOGD("CadeBridge: PulseCoil '"s + name + "' → " + action
            + " (ms=" + std::to_string(coil.pulse_ms()) + ")");
      }
      else
      {
         // -1 = no game active, -2 = element not found, -3 = wrong element type.
         // These are expected when a cade-side virtual coil has no matching VPX
         // element (e.g. a knocker or chime unit), so log at warn-level but don't
         // fail the command stream.
         LOGW("CadeBridge: PulseCoil '"s + name + "' via " + action
            + " failed (result=" + std::to_string(result)
            + ", category=" + cade::events::DeviceCategory_Name(cat) + ")");
      }
   }
   else if (cmd.has_enable_coil())
   {
      const auto& coil = cmd.enable_coil();
      LOGD("CadeBridge: EnableCoil '"s + coil.device_name() + "' enable=" + std::to_string(coil.enable()));
      // EnableCoil is used for held coils like flippers. Try as flipper first,
      // fall back to logging if not a flipper element.
      if (vpxApi && vpxApi->SetFlipperState)
      {
         int result = vpxApi->SetFlipperState(coil.device_name().c_str(), coil.enable() ? 1 : 0);
         if (result == -2 || result == -3)
            LOGD("CadeBridge: EnableCoil '"s + coil.device_name() + "' is not a flipper (result=" + std::to_string(result) + ")");
      }
   }
   else if (cmd.has_set_light())
   {
      const auto& light = cmd.set_light();
      float state = light.on() ? (light.brightness() > 0 ? light.brightness() / 255.0f : 1.0f) : 0.0f;
      LOGD("CadeBridge: SetLight '"s + light.device_name() + "' state=" + std::to_string(state));
      if (vpxApi && vpxApi->SetLightState)
      {
         int result = vpxApi->SetLightState(light.device_name().c_str(), state);
         if (result != 0)
            LOGW("CadeBridge: SetLight failed for '"s + light.device_name() + "', result=" + std::to_string(result));
      }
   }
   else if (cmd.has_flash_light())
   {
      const auto& flash = cmd.flash_light();
      float state = flash.brightness() > 0 ? flash.brightness() / 255.0f : 1.0f;
      LOGD("CadeBridge: FlashLight '"s + flash.device_name() + "' on_ms=" + std::to_string(flash.on_ms()) + " count=" + std::to_string(flash.count()));
      if (vpxApi && vpxApi->SetLightState)
      {
         // Turn light on immediately. Cade's executor schedules a SetLight(off)
         // after the flash duration completes to turn the light back off.
         int result = vpxApi->SetLightState(flash.device_name().c_str(), state);
         if (result != 0)
            LOGW("CadeBridge: FlashLight failed for '"s + flash.device_name() + "', result=" + std::to_string(result));
      }
   }
   else if (cmd.has_set_gi())
   {
      LOGD("CadeBridge: SetGI '"s + cmd.set_gi().gi_string_name() + "' on=" + std::to_string(cmd.set_gi().on()) + " (not yet implemented)");
   }
   else if (cmd.has_reset_target())
   {
      const auto& reset = cmd.reset_target();
      LOGD("CadeBridge: ResetTarget '"s + reset.target_group_name() + "'");
      if (vpxApi && vpxApi->SetDropTargetState)
      {
         int result = vpxApi->SetDropTargetState(reset.target_group_name().c_str(), 0); // 0 = raised (reset)
         if (result != 0)
            LOGW("CadeBridge: ResetTarget failed for '"s + reset.target_group_name() + "', result=" + std::to_string(result));
      }
   }
   else if (cmd.has_eject_ball())
   {
      const auto& eject = cmd.eject_ball();
      LOGI("CadeBridge: EjectBall device='"s + eject.device_name()
         + "' angle=" + std::to_string(eject.angle())
         + " strength=" + std::to_string(eject.strength()));
      if (vpxApi && vpxApi->EjectBall)
      {
         int result = vpxApi->EjectBall(eject.device_name().c_str(), eject.angle(), eject.strength(), 0.0f);
         if (result != 0)
            LOGW("CadeBridge: EjectBall failed, result="s + std::to_string(result));
      }
      else
      {
         LOGW("CadeBridge: EjectBall not available (vpxApi missing EjectBall)");
      }
   }
   else if (cmd.has_destroy_ball())
   {
      const auto& destroy = cmd.destroy_ball();
      LOGI("CadeBridge: DestroyBall device='"s + destroy.device_name() + "'");
      if (vpxApi && vpxApi->DestroyBall)
      {
         int result = vpxApi->DestroyBall(destroy.device_name().c_str());
         if (result < 0)
            LOGW("CadeBridge: DestroyBall failed, result="s + std::to_string(result));
         else
            LOGD("CadeBridge: DestroyBall destroyed "s + std::to_string(result) + " ball(s)");
      }
      else
      {
         LOGW("CadeBridge: DestroyBall not available (vpxApi missing DestroyBall)");
      }
   }
   else if (cmd.has_kick_ball())
   {
      const auto& kick = cmd.kick_ball();
      const std::string& name = kick.device_name();

      // Kicker kick parameters (angle, strength) were pushed once during
      // registration / config-update via KickerConfig, stored locally by
      // DeviceRegistry. Look them up by device name here — the runtime
      // command only carries the name.
      const auto* params = deviceRegistry.GetKickerParams(name);
      if (!params)
      {
         LOGW("CadeBridge: KickBall '"s + name + "' has no KickerConfig registered -- "
            + "add `settings { kick_angle = ... kick_strength = ... }` to the kicker "
            + "device declaration in the .cade file. Command dropped.");
         return;
      }

      LOGI("CadeBridge: KickBall device='"s + name
         + "' angle=" + std::to_string(params->angle)
         + " strength=" + std::to_string(params->strength));
      if (vpxApi && vpxApi->KickBall)
      {
         int result = vpxApi->KickBall(name.c_str(), params->angle, params->strength, 0.0f);
         if (result == -4)
         {
            // Distinct from Kicker::KickXYZ's silent no-op: return -4 means
            // "kicker has no ball held". This is diagnosable — likely a
            // misordered handler (gate fired before kicker captured) or a
            // race. Log with the device name so the user can trace back to
            // the handler config.
            LOGW("CadeBridge: KickBall '"s + name + "' -- kicker holds no ball (result=-4). "
               + "The gate handler likely fired before the ball settled into the kicker.");
         }
         else if (result != 0)
         {
            LOGW("CadeBridge: KickBall failed, result="s + std::to_string(result));
         }
      }
      else
      {
         LOGW("CadeBridge: KickBall not available (vpxApi missing KickBall)");
      }
   }
   else
   {
      LOGD("CadeBridge: unknown device command type, seq="s + std::to_string(cmd.sequence()));
   }
}

///////////////////////////////////////////////////////////////////////////////
// Reconnect callback - invoked from gRPC thread when cade (re)connects

static void onCadeReconnect()
{
   LOGI("CadeBridge: cade (re)connected"s);

   // If a game is currently active, push table_ready + state snapshot
   // so cade can resync without waiting for a new game cycle.
   if (grpcServer.IsGameActive()) {
      LOGI("CadeBridge: game active, sending table_ready + snapshot to reconnected cade"s);
      sendCadeEvent(MapTableReadyToCade(nullptr));
      sendInitialStateSnapshot();
   }
}

///////////////////////////////////////////////////////////////////////////////
// PlatformCommand handler - process commands from cade (server mode)

static void executeDeviceCommandOnMain(void* userData)
{
   auto* cmd = static_cast<cade::events::DeviceCommand*>(userData);
   handleDeviceCommand(*cmd);
   delete cmd;
}

static void onPlatformCommand(const cade::events::PlatformCommand& cmd)
{
   if (cmd.has_device_command())
   {
      auto* copy = new cade::events::DeviceCommand(cmd.device_command());
      msgApi->RunOnMainThread(endpointId, 0.0, executeDeviceCommandOnMain, copy);
   }
   else if (cmd.has_device_command_batch())
   {
      for (const auto& c : cmd.device_command_batch().commands())
      {
         auto* copy = new cade::events::DeviceCommand(c);
         msgApi->RunOnMainThread(endpointId, 0.0, executeDeviceCommandOnMain, copy);
      }
   }
   else if (cmd.has_config_update())
   {
      const auto& update = cmd.config_update();
      LOGI("CadeBridge: received config update v"s + std::to_string(update.config_version()));
      deviceRegistry.ApplyConfigUpdate(update);

      // Send ACK back via the stream
      cade::events::PlatformEvent ackEvent;
      auto* ack = ackEvent.mutable_config_ack();
      ack->set_config_version(update.config_version());
      ack->set_success(true);
      grpcServer.QueueEvent(ackEvent);

      // Send initial state snapshot now that mappings are applied
      sendInitialStateSnapshot();
   }
   else if (cmd.has_control())
   {
      // ACTION_HEARTBEAT is cade's periodic no-op wake that lets this single-
      // threaded stream reader flush queued outbound events; sent continuously
      // (~20/s), so it is intentionally not logged.
      (void)cmd.control();
   }
   else if (cmd.has_subscribe())
   {
      LOGD("CadeBridge: subscription request from cade"s);
   }
}

///////////////////////////////////////////////////////////////////////////////
// Source change callbacks - re-discover on dynamic changes

static void onInputSourcesChanged(const unsigned int eventId, void* userData, void* eventData)
{
   discoverInputSources();
}

static void onDeviceSourcesChanged(const unsigned int eventId, void* userData, void* eventData)
{
   discoverDeviceSources();
}

///////////////////////////////////////////////////////////////////////////////
// Stub subscriptions - display/segment (log only, no forwarding)

static void onDisplaySourcesChanged(const unsigned int eventId, void* userData, void* eventData)
{
   LOGD("CadeBridge: display sources changed (not forwarded)"s);
}

static void onSegSourcesChanged(const unsigned int eventId, void* userData, void* eventData)
{
   LOGD("CadeBridge: segment display sources changed (not forwarded)"s);
}

///////////////////////////////////////////////////////////////////////////////
// Game lifecycle

// Tear down the subscriptions and tracked sources registered in onGameStart.
// Guarded by gameSubscriptionsActive so it is safe to call from both onGameEnd
// and plugin unload: unloading the plugin while a game is active (e.g. toggling
// it off in play mode) would otherwise leak these callbacks, leaving dangling
// pointers into freed plugin code that fire on the next action/element event.
static void unsubscribeGameEvents()
{
   if (!gameSubscriptionsActive)
      return;
   gameSubscriptionsActive = false;

   msgApi->UnsubscribeMsg(onActionChangedId, onActionChanged, nullptr);
   msgApi->UnsubscribeMsg(onGameElementId, onGameElement, nullptr);
   msgApi->UnsubscribeMsg(onInputSrcChgId, onInputSourcesChanged, nullptr);
   msgApi->UnsubscribeMsg(onDevSrcChgId, onDeviceSourcesChanged, nullptr);
   msgApi->UnsubscribeMsg(onDisplaySrcChgId, onDisplaySourcesChanged, nullptr);
   msgApi->UnsubscribeMsg(onSegSrcChgId, onSegSourcesChanged, nullptr);

   // Clear tracked sources (callbacks auto-unregister on source change broadcast)
   {
      std::lock_guard<std::mutex> lock(sourcesMutex);
      trackedInputs.clear();
      trackedDevices.clear();
   }
}

static void onGameStart(const unsigned int eventId, void* userData, void* eventData)
{
   const char* gameId = nullptr;

   // Try controller game start for gameId
   // The VPX game start event doesn't carry a gameId, but the controller one does
   // We subscribe to both and use whichever fires

   // Subscribe to VPX action changes (flippers, start, coin, nudge, etc.)
   msgApi->SubscribeMsg(endpointId, onActionChangedId, onActionChanged, nullptr);

   // Subscribe to VPX game element events (bumper hits, target drops, spinner spins, etc.)
   msgApi->SubscribeMsg(endpointId, onGameElementId, onGameElement, nullptr);

   // Subscribe to dynamic source changes
   msgApi->SubscribeMsg(endpointId, onInputSrcChgId, onInputSourcesChanged, nullptr);
   msgApi->SubscribeMsg(endpointId, onDevSrcChgId, onDeviceSourcesChanged, nullptr);
   msgApi->SubscribeMsg(endpointId, onDisplaySrcChgId, onDisplaySourcesChanged, nullptr);
   msgApi->SubscribeMsg(endpointId, onSegSrcChgId, onSegSourcesChanged, nullptr);
   gameSubscriptionsActive = true;

   // Discover available sources and register callbacks
   discoverInputSources();
   discoverDeviceSources();

   // Build device manifest
   cade::events::DeviceManifest manifest;
   {
      std::lock_guard<std::mutex> lock(sourcesMutex);
      std::vector<InputSrcId> inputs;
      for (const auto& t : trackedInputs)
         inputs.push_back(t.src);
      std::vector<DevSrcId> devices;
      for (const auto& t : trackedDevices)
         devices.push_back(t.src);
      manifest = deviceRegistry.BuildManifest(inputs, devices, msgApi, endpointId, getGameElementsMsgId);
   }

   grpcServer.SetManifest(manifest);
   grpcServer.SetGameActive(true);

   // Start element debouncer if configured.
   if (debounceMsProp_Val > 0) {
      double holdoff = debounceMsProp_Val / 1000.0;
      LOGI("CadeBridge: element debounce holdoff = "s + std::to_string(debounceMsProp_Val) + "ms");
      elementDebouncer.Start(holdoff, sendCadeEvent);
   }

   // If cade is already connected, push table_ready + state snapshot immediately.
   if (grpcServer.IsConnected()) {
      LOGI("CadeBridge: cade already connected, sending table_ready + snapshot"s);
      sendCadeEvent(MapTableReadyToCade(nullptr));
      sendInitialStateSnapshot();
   }

   LOGI("CadeBridge: table ready, waiting for cade"s);
}

static void onCtlGameStart(const unsigned int eventId, void* userData, void* eventData)
{
   auto* msg = static_cast<CtlOnGameStartMsg*>(eventData);
   const char* gameId = msg ? msg->gameId : nullptr;
   LOGI("CadeBridge: sending table_ready event, gameId="s + (gameId ? gameId : "(null)"));
   sendCadeEvent(MapTableReadyToCade(gameId));
}

static void onGameEnd(const unsigned int eventId, void* userData, void* eventData)
{
   grpcServer.SetGameActive(false);
   elementDebouncer.Stop();

   // Send table_stopped event but keep the server running for reconnection.
   if (grpcServer.IsConnected())
   {
      LOGI("CadeBridge: sending table_stopped event"s);
      sendCadeEvent(MapTableStoppedToCade());
   }

   // Unsubscribe from action/element changes and dynamic source changes,
   // and clear tracked sources.
   unsubscribeGameEvents();

   deviceRegistry.Clear();
   LOGI("CadeBridge: game ended"s);
}

LPI_IMPLEMENT_CPP // Implement shared log support

} // namespace CadeBridge

using namespace CadeBridge;

///////////////////////////////////////////////////////////////////////////////
// Plugin entry points

MSGPI_EXPORT void MSGPIAPI CadeBridgePluginLoad(const uint32_t sessionId, const MsgPluginAPI* api)
{
   msgApi = api;
   endpointId = sessionId;

   LPISetup(endpointId, msgApi);

   // Register settings (plugin on/off is the core per-plugin "Enable" toggle)
   msgApi->RegisterSetting(endpointId, &listenPortProp);
   msgApi->RegisterSetting(endpointId, &debounceMsProp);

   // Get VPX API
   msgApi->BroadcastMsg(endpointId, getVpxApiId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_MSG_GET_API), &vpxApi);

   // Subscribe to game lifecycle events
   msgApi->SubscribeMsg(endpointId, onGameStartId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_GAME_START), onGameStart, nullptr);
   msgApi->SubscribeMsg(endpointId, onGameEndId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_GAME_END), onGameEnd, nullptr);

   // Get controller event IDs
   onCtlGameStartId = msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_EVT_ON_GAME_START);
   msgApi->SubscribeMsg(endpointId, onCtlGameStartId, onCtlGameStart, nullptr);
   onCtlGameEndId = msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_EVT_ON_GAME_END);

   // Get VPX action/element change event IDs
   onActionChangedId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_ACTION_CHANGED);
   onGameElementId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_GAME_ELEMENT);
   getGameElementsMsgId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_MSG_GET_GAME_ELEMENTS);

   // Get source discovery message IDs
   getInputSrcId = msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_INPUT_GET_SRC_MSG);
   getDevSrcId = msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_DEVICE_GET_SRC_MSG);
   onInputSrcChgId = msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_INPUT_ON_SRC_CHG_MSG);
   onDevSrcChgId = msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_DEVICE_ON_SRC_CHG_MSG);
   onDisplaySrcChgId = msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_DISPLAY_ON_SRC_CHG_MSG);
   onSegSrcChgId = msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_SEG_ON_SRC_CHG_MSG);

   // Start the gRPC server on a dedicated background thread at plugin load.
   // It persists across game start/end cycles so cade can connect early and
   // survive reconnects without being tied to VPX's main loop scheduling.
   LOGI("CadeBridge: starting PlatformService on port "s + std::to_string(listenPortProp_Val));
   grpcServer.Start(listenPortProp_Val, onPlatformCommand, onCadeReconnect);

   LOGI("CadeBridge plugin loaded"s);
}

MSGPI_EXPORT void MSGPIAPI CadeBridgePluginUnload()
{
   elementDebouncer.Stop();
   grpcServer.Stop();

   // If the plugin is unloaded mid-game (e.g. disabled in play mode), onGameEnd
   // has not run, so tear down the game-active subscriptions here to avoid
   // leaking them. No-op if no game is active.
   unsubscribeGameEvents();

   // Unsubscribe from all events
   msgApi->UnsubscribeMsg(onGameStartId, onGameStart, nullptr);
   msgApi->UnsubscribeMsg(onGameEndId, onGameEnd, nullptr);
   msgApi->UnsubscribeMsg(onCtlGameStartId, onCtlGameStart, nullptr);

   // Release all message IDs
   msgApi->ReleaseMsgID(onActionChangedId);
   msgApi->ReleaseMsgID(onGameElementId);
   msgApi->ReleaseMsgID(getGameElementsMsgId);
   msgApi->ReleaseMsgID(getVpxApiId);
   msgApi->ReleaseMsgID(onGameStartId);
   msgApi->ReleaseMsgID(onGameEndId);
   msgApi->ReleaseMsgID(onCtlGameStartId);
   msgApi->ReleaseMsgID(onCtlGameEndId);
   msgApi->ReleaseMsgID(getInputSrcId);
   msgApi->ReleaseMsgID(getDevSrcId);
   msgApi->ReleaseMsgID(onInputSrcChgId);
   msgApi->ReleaseMsgID(onDevSrcChgId);
   msgApi->ReleaseMsgID(onDisplaySrcChgId);
   msgApi->ReleaseMsgID(onSegSrcChgId);

   vpxApi = nullptr;
   msgApi = nullptr;

   LOGI("CadeBridge plugin unloaded"s);
}
