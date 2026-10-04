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
#include <memory>

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
VPXGameElementAPI* elementApi = nullptr; // Cade fork extension, null if VPX does not provide it

uint32_t endpointId;
unsigned int getGameElementApiId;
unsigned int getVpxApiId;
unsigned int onGameStartId, onGameEndId;
unsigned int onPrepareFrameId;
unsigned int onDisplaySrcChgId, onSegSrcChgId;
unsigned int onActionChangedId;
unsigned int onGameElementId;
unsigned int getGameElementsMsgId;

GrpcServer grpcServer;
DeviceRegistry deviceRegistry;
EventDebouncer elementDebouncer;

// Controller states (switches, solenoids, lamps, ...) exposed by controller plugins (PinMAME, B2S, ...).
// The controller API has no change notification, so states are polled once per frame and diffed
// against the last polled value. The list is subscribed while a game is active.
std::unique_ptr<PinballPlugin::Controller::CtrlItemConsumer<StateSrcId>> stateSources;

// Controllers running the game, used to send the game id to cade once the controller is started
std::unique_ptr<PinballPlugin::Controller::CtrlItemConsumer<ControllerDef>> controllers;
std::string announcedGameId;

// Last polled value of each controller state. Only accessed on the plugin API thread.
struct PolledState
{
   double value = 0.0;
   cade::events::DeviceCategory category = cade::events::DEVICE_CATEGORY_GENERAL;
   bool valid = false; // false if the state can not be polled (no getter, string format)
};
std::vector<std::vector<PolledState>> polledStates; // One entry per state block, empty until first poll

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
// Controller state polling - forward state changes to Cade

// Switch, coil and light states are forwarded on on/off transitions only, as
// modulated outputs (PWM solenoids, fading lamps) change brightness every frame.
// Other states (scores, mechs, ...) are forwarded on any value change.
static bool IsStateChange(const PolledState& prev, double value)
{
   switch (prev.category)
   {
   case cade::events::DEVICE_CATEGORY_SWITCH:
   case cade::events::DEVICE_CATEGORY_COIL:
   case cade::events::DEVICE_CATEGORY_LIGHT:
      return (prev.value != 0.0) != (value != 0.0);
   default:
      return prev.value != value;
   }
}

static void onPrepareFrame(const unsigned int eventId, void* userData, void* eventData)
{
   if (!stateSources)
      return;
   stateSources->With([](const std::vector<StateSrcId>& sources)
   {
      if (polledStates.size() != sources.size())
         polledStates.resize(sources.size());
      for (size_t s = 0; s < sources.size(); s++)
      {
         const StateSrcId& src = sources[s];
         std::vector<PolledState>& polled = polledStates[s];
         const bool firstPoll = polled.size() != src.nStates;
         if (firstPoll)
         {
            polled.assign(src.nStates, PolledState {});
            for (unsigned int i = 0; i < src.nStates; i++)
               polled[i].category = ClassifyControllerState(src, src.stateDefs[i]);
         }
         for (unsigned int i = 0; i < src.nStates; i++)
         {
            PolledState& prev = polled[i];
            double value;
            if (!ReadControllerState(src.stateDefs[i], value))
               continue;
            // On first poll of a state block, only active states are forwarded (same as the initial snapshot)
            const bool changed = firstPoll ? value != 0.0 : (!prev.valid || IsStateChange(prev, value));
            prev.valid = true;
            prev.value = value;
            if (!changed)
               continue;
            auto event = MapStateToCade(src, i, value, &deviceRegistry);
            LOGI("CadeBridge: state changed -> device_key="s + event.device_key() + " value=" + std::to_string(value));
            sendCadeEvent(event);
         }
      }
   });
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
   if (elementApi)
   {
      for (const auto* rule : deviceRegistry.GetAutofireRulesForSwitch(deviceKey))
      {
         if (!rule->enabled || rule->type != "hold")
            continue;
         int result = elementApi->SetFlipperState(rule->coilName.c_str(), actionEvent->isPressed ? 1 : 0);
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
// Controller state discovery

static void updateManifest()
{
   const auto states = stateSources->With([](const std::vector<StateSrcId>& sources) { return sources; });
   grpcServer.SetManifest(deviceRegistry.BuildManifest(states, msgApi, endpointId, getGameElementsMsgId));
}

// May be called from the gRPC thread (reconnect, config update): GetState is thread safe
// and the state list is accessed through With, which synchronizes against list changes.
static void sendInitialStateSnapshot()
{
   if (!stateSources)
      return;
   stateSources->With([](const std::vector<StateSrcId>& sources)
   {
      for (const StateSrcId& src : sources)
      {
         for (unsigned int i = 0; i < src.nStates; i++)
         {
            double value;
            if (ReadControllerState(src.stateDefs[i], value) && value != 0.0) // Only send active states
               sendCadeEvent(MapStateToCade(src, i, value, &deviceRegistry));
         }
      }
   });
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
         if (elementApi)
         {
            result = elementApi->SetDropTargetState(name.c_str(), 0);
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
      if (elementApi)
      {
         int result = elementApi->SetFlipperState(coil.device_name().c_str(), coil.enable() ? 1 : 0);
         if (result == -2 || result == -3)
            LOGD("CadeBridge: EnableCoil '"s + coil.device_name() + "' is not a flipper (result=" + std::to_string(result) + ")");
      }
   }
   else if (cmd.has_set_light())
   {
      const auto& light = cmd.set_light();
      float state = light.on() ? (light.brightness() > 0 ? light.brightness() / 255.0f : 1.0f) : 0.0f;
      LOGD("CadeBridge: SetLight '"s + light.device_name() + "' state=" + std::to_string(state));
      if (elementApi)
      {
         int result = elementApi->SetLightState(light.device_name().c_str(), state);
         if (result != 0)
            LOGW("CadeBridge: SetLight failed for '"s + light.device_name() + "', result=" + std::to_string(result));
      }
   }
   else if (cmd.has_flash_light())
   {
      const auto& flash = cmd.flash_light();
      float state = flash.brightness() > 0 ? flash.brightness() / 255.0f : 1.0f;
      LOGD("CadeBridge: FlashLight '"s + flash.device_name() + "' on_ms=" + std::to_string(flash.on_ms()) + " count=" + std::to_string(flash.count()));
      if (elementApi)
      {
         // Turn light on immediately. Cade's executor schedules a SetLight(off)
         // after the flash duration completes to turn the light back off.
         int result = elementApi->SetLightState(flash.device_name().c_str(), state);
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
      if (elementApi)
      {
         int result = elementApi->SetDropTargetState(reset.target_group_name().c_str(), 0); // 0 = raised (reset)
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
      if (elementApi)
      {
         int result = elementApi->EjectBall(eject.device_name().c_str(), eject.angle(), eject.strength(), 0.0f);
         if (result != 0)
            LOGW("CadeBridge: EjectBall failed, result="s + std::to_string(result));
      }
      else
      {
         LOGW("CadeBridge: EjectBall not available (VPX build lacks the game element API)");
      }
   }
   else if (cmd.has_destroy_ball())
   {
      const auto& destroy = cmd.destroy_ball();
      LOGI("CadeBridge: DestroyBall device='"s + destroy.device_name() + "'");
      if (elementApi)
      {
         int result = elementApi->DestroyBall(destroy.device_name().c_str());
         if (result < 0)
            LOGW("CadeBridge: DestroyBall failed, result="s + std::to_string(result));
         else
            LOGD("CadeBridge: DestroyBall destroyed "s + std::to_string(result) + " ball(s)");
      }
      else
      {
         LOGW("CadeBridge: DestroyBall not available (VPX build lacks the game element API)");
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
      if (elementApi)
      {
         int result = elementApi->KickBall(name.c_str(), params->angle, params->strength, 0.0f);
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
         LOGW("CadeBridge: KickBall not available (VPX build lacks the game element API)");
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
// Controller list/state change callbacks (plugin API thread)

static void onStateSourcesAboutToChange()
{
   // Cached states refer to the state blocks that are about to be discarded
   polledStates.clear();
}

static void onStateSourcesChanged()
{
   const size_t nSources = stateSources->With([](const std::vector<StateSrcId>& sources) { return sources.size(); });
   LOGI("CadeBridge: "s + std::to_string(nSources) + " controller state source(s)");
   // Controllers usually expose their states after the game start (when the script starts the ROM), so the manifest is refreshed
   if (grpcServer.IsGameActive())
      updateManifest();
}

static void onControllersChanged()
{
   const std::string gameId = controllers->With([](const std::vector<ControllerDef>& items)
      { return (items.empty() || items.front().gameId == nullptr) ? std::string() : std::string(items.front().gameId); });
   if (gameId.empty() || gameId == announcedGameId)
      return;
   announcedGameId = gameId;
   LOGI("CadeBridge: sending table_ready event, gameId="s + gameId);
   sendCadeEvent(MapTableReadyToCade(gameId.c_str()));
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
   msgApi->UnsubscribeMsg(onPrepareFrameId, onPrepareFrame, nullptr);
   msgApi->UnsubscribeMsg(onDisplaySrcChgId, onDisplaySourcesChanged, nullptr);
   msgApi->UnsubscribeMsg(onSegSrcChgId, onSegSourcesChanged, nullptr);

   if (controllers->IsSubscribed())
      controllers->Unsubscribe();
   if (stateSources->IsSubscribed())
      stateSources->Unsubscribe();
   polledStates.clear();
   announcedGameId.clear();
}

static void onGameStart(const unsigned int eventId, void* userData, void* eventData)
{
   if (elementApi == nullptr && vpxApi != nullptr)
      vpxApi->PushNotification("Cade Bridge: incompatible VPX build (no game element API), see log", 10000);

   // Subscribe to VPX action changes (flippers, start, coin, nudge, etc.)
   msgApi->SubscribeMsg(endpointId, onActionChangedId, onActionChanged, nullptr);

   // Subscribe to VPX game element events (bumper hits, target drops, spinner spins, etc.)
   msgApi->SubscribeMsg(endpointId, onGameElementId, onGameElement, nullptr);

   // Poll controller states once per frame
   msgApi->SubscribeMsg(endpointId, onPrepareFrameId, onPrepareFrame, nullptr);

   // Subscribe to dynamic source changes
   msgApi->SubscribeMsg(endpointId, onDisplaySrcChgId, onDisplaySourcesChanged, nullptr);
   msgApi->SubscribeMsg(endpointId, onSegSrcChgId, onSegSourcesChanged, nullptr);
   gameSubscriptionsActive = true;

   // Discover available controller states, and build device manifest
   stateSources->Subscribe();
   updateManifest();
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

   // Announce the game id once a controller (PinMAME, ...) is started by the table script
   controllers->Subscribe();

   LOGI("CadeBridge: table ready, waiting for cade"s);
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

   // Get the game element API. It is only provided by the Cade fork of VPX, matching this plugin's version:
   // element events and actuation can not work without it, so report it instead of failing silently.
   msgApi->BroadcastMsg(endpointId, getGameElementApiId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_MSG_GET_GAME_ELEMENT_API), &elementApi);
   if (elementApi == nullptr || elementApi->version < 1)
   {
      elementApi = nullptr;
      LOGE("CadeBridge: this VPX build does not provide the game element API ("s + VPXPI_MSG_GET_GAME_ELEMENT_API
         + "). Table element events and device commands are disabled: use the Cade fork of VPX matching this plugin.");
   }

   // Subscribe to game lifecycle events
   msgApi->SubscribeMsg(endpointId, onGameStartId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_GAME_START), onGameStart, nullptr);
   msgApi->SubscribeMsg(endpointId, onGameEndId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_GAME_END), onGameEnd, nullptr);

   // Get VPX action/element/frame event IDs
   onActionChangedId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_ACTION_CHANGED);
   onGameElementId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_GAME_ELEMENT);
   getGameElementsMsgId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_MSG_GET_GAME_ELEMENTS);
   onPrepareFrameId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_PREPARE_FRAME);

   // Controller discovery (subscribed while a game is active)
   stateSources = std::make_unique<PinballPlugin::Controller::CtrlItemConsumer<StateSrcId>>(
      msgApi, endpointId, CTLPI_STATE_GET_SRC_MSG, CTLPI_STATE_ON_SRC_CHG_MSG, nullptr, onStateSourcesAboutToChange, onStateSourcesChanged);
   controllers = std::make_unique<PinballPlugin::Controller::CtrlItemConsumer<ControllerDef>>(
      msgApi, endpointId, CTLPI_CONTROLLERS_GET_MSG, CTLPI_CONTROLLERS_ON_CHG_MSG, nullptr, nullptr, onControllersChanged);

   // Get display/segment source change IDs
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

   // Release controller discovery (unsubscribed above)
   stateSources.reset();
   controllers.reset();

   // Unsubscribe from all events
   msgApi->UnsubscribeMsg(onGameStartId, onGameStart, nullptr);
   msgApi->UnsubscribeMsg(onGameEndId, onGameEnd, nullptr);

   // Release all message IDs
   msgApi->ReleaseMsgID(onActionChangedId);
   msgApi->ReleaseMsgID(onGameElementId);
   msgApi->ReleaseMsgID(getGameElementsMsgId);
   msgApi->ReleaseMsgID(getVpxApiId);
   msgApi->ReleaseMsgID(getGameElementApiId);
   msgApi->ReleaseMsgID(onGameStartId);
   msgApi->ReleaseMsgID(onGameEndId);
   msgApi->ReleaseMsgID(onPrepareFrameId);
   msgApi->ReleaseMsgID(onDisplaySrcChgId);
   msgApi->ReleaseMsgID(onSegSrcChgId);

   vpxApi = nullptr;
   elementApi = nullptr;
   msgApi = nullptr;

   LOGI("CadeBridge plugin unloaded"s);
}
