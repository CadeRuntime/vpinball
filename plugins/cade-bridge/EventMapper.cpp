// license:GPLv3+

#include "EventMapper.h"
#include "DeviceRegistry.h"
#include <chrono>
#include <cstdint>
#include <google/protobuf/timestamp.pb.h>

namespace CadeBridge {

static void SetTimestampNow(google::protobuf::Timestamp* ts)
{
   auto now = std::chrono::system_clock::now();
   auto secs = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch());
   auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()) - std::chrono::duration_cast<std::chrono::nanoseconds>(secs);
   ts->set_seconds(secs.count());
   ts->set_nanos(static_cast<int32_t>(nanos.count()));
}

std::string MakeDeviceKey(const DeviceDef& def)
{
   return MakeDeviceKey(def.id.groupId, def.id.deviceId);
}

std::string MakeDeviceKey(uint16_t groupId, uint16_t deviceId)
{
   return std::to_string(groupId) + ":" + std::to_string(deviceId);
}

cade::events::CategorizedEvent MapInputToCade(const InputSrcId& src, unsigned int inputIndex, int state, const DeviceRegistry* registry)
{
   cade::events::CategorizedEvent event;
   event.set_category(cade::events::EVENT_CATEGORY_GENERAL);
   event.set_device_category(cade::events::DEVICE_CATEGORY_SWITCH);

   int vpxEvent = state ? VPXDEV_EVENT_ON : VPXDEV_EVENT_OFF;
   std::string deviceName;

   if (inputIndex < src.nInputs)
   {
      const DeviceDef& def = src.inputDefs[inputIndex];
      deviceName = def.name ? SanitizeUTF8(def.name) : MakeDeviceKey(def);
      event.set_device_key(deviceName);
      if (def.name)
         event.set_device_type(deviceName);
   }

   if (registry && registry->IsRegistered())
   {
      int cadeType = registry->GetCadeEventType(deviceName, vpxEvent);
      if (cadeType >= 0)
         event.set_event_type(cadeType);
      event.set_device_category(registry->GetDeviceCategory(deviceName));
   }
   else
   {
      event.set_event_type(state ? CadeEventType::SwitchActivate : CadeEventType::SwitchDeactivate);
   }

   // Use negotiated scoring trigger name if available
   std::string scoringName;
   if (registry && registry->IsRegistered())
      scoringName = registry->GetCadeEventName(deviceName, vpxEvent);
   event.set_event_type_name(!scoringName.empty() ? scoringName : (state ? "switch_closed" : "switch_open"));
   (*event.mutable_metadata())["state"] = std::to_string(state);
   SetTimestampNow(event.mutable_timestamp());
   return event;
}

cade::events::CategorizedEvent MapDeviceToCade(const DevSrcId& src, unsigned int deviceIndex, const DeviceRegistry* registry)
{
   cade::events::CategorizedEvent event;
   event.set_category(cade::events::EVENT_CATEGORY_GENERAL);

   uint8_t byteState = 0;
   if (src.GetByteState)
      byteState = src.GetByteState(deviceIndex);

   // Determine device category from the device def's groupId
   // groupId conventions: 0=coil, 1=lamp/light, others=general
   cade::events::DeviceCategory devCat = cade::events::DEVICE_CATEGORY_GENERAL;
   std::string deviceName;
   if (deviceIndex < src.nDevices)
   {
      const DeviceDef& def = src.deviceDefs[deviceIndex];
      switch (def.id.groupId)
      {
      case 0: devCat = cade::events::DEVICE_CATEGORY_COIL; break;
      case 1: devCat = cade::events::DEVICE_CATEGORY_LIGHT; break;
      default: devCat = cade::events::DEVICE_CATEGORY_GENERAL; break;
      }
      deviceName = def.name ? SanitizeUTF8(def.name) : MakeDeviceKey(def);
      event.set_device_key(deviceName);
      if (def.name)
         event.set_device_type(deviceName);
   }

   int vpxEvent = byteState ? VPXDEV_EVENT_ON : VPXDEV_EVENT_OFF;

   if (registry && registry->IsRegistered())
   {
      int cadeType = registry->GetCadeEventType(deviceName, vpxEvent);
      if (cadeType >= 0)
         event.set_event_type(cadeType);
      event.set_device_category(registry->GetDeviceCategory(deviceName));
   }
   else
   {
      event.set_device_category(devCat);
      // Use category-appropriate defaults
      switch (devCat)
      {
      case cade::events::DEVICE_CATEGORY_COIL:
         event.set_event_type(byteState ? CadeEventType::CoilEnable : CadeEventType::CoilDisable);
         break;
      case cade::events::DEVICE_CATEGORY_LIGHT:
         event.set_event_type(byteState ? CadeEventType::LightOn : CadeEventType::LightOff);
         break;
      default:
         event.set_event_type(byteState ? CadeEventType::SwitchActivate : CadeEventType::SwitchDeactivate);
         break;
      }
   }

   // Use negotiated scoring trigger name if available
   std::string scoringName;
   if (registry && registry->IsRegistered())
      scoringName = registry->GetCadeEventName(deviceName, vpxEvent);
   event.set_event_type_name(!scoringName.empty() ? scoringName : (byteState ? "device_on" : "device_off"));
   (*event.mutable_metadata())["byte_state"] = std::to_string(byteState);

   if (src.GetFloatState)
   {
      float floatState = src.GetFloatState(deviceIndex);
      (*event.mutable_metadata())["float_state"] = std::to_string(floatState);
   }

   SetTimestampNow(event.mutable_timestamp());
   return event;
}

static const char* ActionName(VPXAction action)
{
   switch (action)
   {
   case VPXACTION_LeftFlipper:        return "left_flipper";
   case VPXACTION_RightFlipper:       return "right_flipper";
   case VPXACTION_StagedLeftFlipper:  return "staged_left_flipper";
   case VPXACTION_StagedRightFlipper: return "staged_right_flipper";
   case VPXACTION_LeftMagnaSave:      return "left_magna_save";
   case VPXACTION_RightMagnaSave:     return "right_magna_save";
   case VPXACTION_LaunchBall:         return "launch_ball";
   case VPXACTION_LeftNudge:          return "left_nudge";
   case VPXACTION_CenterNudge:        return "center_nudge";
   case VPXACTION_RightNudge:         return "right_nudge";
   case VPXACTION_Tilt:               return "tilt";
   case VPXACTION_AddCredit:          return "add_credit";
   case VPXACTION_AddCredit2:         return "add_credit2";
   case VPXACTION_StartGame:          return "start_game";
   case VPXACTION_Lockbar:            return "lockbar";
   case VPXACTION_Pause:              return "pause";
   case VPXACTION_ExitGame:           return "exit_game";
   default:                           return "unknown";
   }
}

static cade::events::DeviceCategory ActionDeviceCategory(VPXAction action)
{
   switch (action)
   {
   case VPXACTION_LeftFlipper:
   case VPXACTION_RightFlipper:
   case VPXACTION_StagedLeftFlipper:
   case VPXACTION_StagedRightFlipper:
      return cade::events::DEVICE_CATEGORY_FLIPPER;
   default:
      return cade::events::DEVICE_CATEGORY_SWITCH;
   }
}

cade::events::CategorizedEvent MapActionToCade(VPXAction action, int isPressed, const DeviceRegistry* registry)
{
   cade::events::CategorizedEvent event;
   event.set_category(cade::events::EVENT_CATEGORY_USER);

   const char* actionName = ActionName(action);
   cade::events::DeviceCategory devCat = ActionDeviceCategory(action);
   int vpxEvent = isPressed ? VPXDEV_EVENT_PRESSED : VPXDEV_EVENT_RELEASED;

   // Action-appropriate default event type. Applied in BOTH the registered and
   // unregistered cases: start_game / add_credit / launch are VPX input actions,
   // not .cade-config devices, so the negotiated registry often has no explicit
   // mapping for them — without a default, start_game would fall through to a
   // plain switch and never reach cade's start handler (UserStart).
   int defaultType;
   if (devCat == cade::events::DEVICE_CATEGORY_FLIPPER)
      defaultType = isPressed ? CadeEventType::FlipperActivate : CadeEventType::FlipperDeactivate;
   else if (action == VPXACTION_StartGame)
      defaultType = isPressed ? CadeEventType::UserStart : CadeEventType::SwitchDeactivate;
   else if (action == VPXACTION_AddCredit || action == VPXACTION_AddCredit2)
      defaultType = isPressed ? CadeEventType::UserCoin : CadeEventType::SwitchDeactivate;
   else if (action == VPXACTION_LaunchBall)
      defaultType = isPressed ? CadeEventType::UserBallPlunge : CadeEventType::SwitchDeactivate;
   else
      defaultType = isPressed ? CadeEventType::SwitchActivate : CadeEventType::SwitchDeactivate;
   event.set_event_type(defaultType);

   if (registry && registry->IsRegistered())
   {
      int cadeType = registry->GetCadeEventType(actionName, vpxEvent);
      if (cadeType >= 0)
         event.set_event_type(cadeType); // registry mapping overrides the default
      event.set_device_category(registry->GetDeviceCategory(actionName));
   }
   else
   {
      event.set_device_category(devCat);
   }

   // User-control actions have fixed cade semantics and must reach cade's
   // coin/start/plunge handlers even when the negotiated registry has no mapping
   // for them (or, worse, a generic switch mapping that would clobber the default
   // above). These take precedence over any registry mapping.
   if (action == VPXACTION_StartGame)
      event.set_event_type(isPressed ? CadeEventType::UserStart : CadeEventType::SwitchDeactivate);
   else if (action == VPXACTION_AddCredit || action == VPXACTION_AddCredit2)
      event.set_event_type(isPressed ? CadeEventType::UserCoin : CadeEventType::SwitchDeactivate);
   else if (action == VPXACTION_LaunchBall)
      event.set_event_type(isPressed ? CadeEventType::UserBallPlunge : CadeEventType::SwitchDeactivate);

   // Use negotiated scoring trigger name if available
   std::string scoringName;
   if (registry && registry->IsRegistered())
      scoringName = registry->GetCadeEventName(actionName, vpxEvent);
   event.set_event_type_name(!scoringName.empty() ? scoringName : (isPressed ? "pressed" : "released"));
   event.set_device_key(actionName);
   event.set_device_type("vpx_action");
   (*event.mutable_metadata())["action_id"] = std::to_string(static_cast<int>(action));
   SetTimestampNow(event.mutable_timestamp());
   return event;
}

bool MapActionFromCade(const cade::events::CategorizedEvent& event, VPXAction& outAction, int& outPressed)
{
   if (event.device_type() != "vpx_action")
      return false;

   outPressed = (event.event_type() != 0) ? 1 : 0;

   const std::string& key = event.device_key();
   if (key == "left_flipper") outAction = VPXACTION_LeftFlipper;
   else if (key == "right_flipper") outAction = VPXACTION_RightFlipper;
   else if (key == "staged_left_flipper") outAction = VPXACTION_StagedLeftFlipper;
   else if (key == "staged_right_flipper") outAction = VPXACTION_StagedRightFlipper;
   else if (key == "left_magna_save") outAction = VPXACTION_LeftMagnaSave;
   else if (key == "right_magna_save") outAction = VPXACTION_RightMagnaSave;
   else if (key == "launch_ball") outAction = VPXACTION_LaunchBall;
   else if (key == "left_nudge") outAction = VPXACTION_LeftNudge;
   else if (key == "center_nudge") outAction = VPXACTION_CenterNudge;
   else if (key == "right_nudge") outAction = VPXACTION_RightNudge;
   else if (key == "tilt") outAction = VPXACTION_Tilt;
   else if (key == "add_credit") outAction = VPXACTION_AddCredit;
   else if (key == "add_credit2") outAction = VPXACTION_AddCredit2;
   else if (key == "start_game") outAction = VPXACTION_StartGame;
   else if (key == "lockbar") outAction = VPXACTION_Lockbar;
   else if (key == "exit_game") outAction = VPXACTION_ExitGame;
   else return false;

   return true;
}

static const char* GameElementEventName(int eventId)
{
   switch (eventId)
   {
   case VPXELEMENT_EVT_HIT:              return "hit";
   case VPXELEMENT_EVT_UNHIT:            return "unhit";
   case VPXELEMENT_EVT_LIMIT_EOS:        return "end_of_stroke";
   case VPXELEMENT_EVT_LIMIT_BOS:        return "beginning_of_stroke";
   case VPXELEMENT_EVT_SLINGSHOT:        return "slingshot";
   case VPXELEMENT_EVT_FLIPPER_COLLIDE:  return "flipper_collide";
   case VPXELEMENT_EVT_SPIN:             return "spin";
   case VPXELEMENT_EVT_DROPPED:          return "dropped";
   case VPXELEMENT_EVT_RAISED:           return "raised";
   default:                              return "unknown";
   }
}

// Map VPX game element event ID to VPXDeviceEvent for registry lookup
static int GameElementEventToVPXDeviceEvent(int eventId)
{
   switch (eventId)
   {
   case VPXELEMENT_EVT_HIT:              return VPXDEV_EVENT_HIT;
   case VPXELEMENT_EVT_UNHIT:            return VPXDEV_EVENT_UNHIT;
   case VPXELEMENT_EVT_LIMIT_EOS:        return VPXDEV_EVENT_EOS;
   case VPXELEMENT_EVT_LIMIT_BOS:        return VPXDEV_EVENT_BOS;
   case VPXELEMENT_EVT_SLINGSHOT:        return VPXDEV_EVENT_SLINGSHOT;
   case VPXELEMENT_EVT_FLIPPER_COLLIDE:  return VPXDEV_EVENT_FLIPPER_COLLIDE;
   case VPXELEMENT_EVT_SPIN:             return VPXDEV_EVENT_SPIN;
   case VPXELEMENT_EVT_DROPPED:          return VPXDEV_EVENT_DROPPED;
   case VPXELEMENT_EVT_RAISED:           return VPXDEV_EVENT_RAISED;
   default:                              return VPXDEV_EVENT_HIT;
   }
}

// Determine device category from element type (ItemTypeEnum)
static cade::events::DeviceCategory DeviceCategoryForElementType(int elementType)
{
   // ItemTypeEnum values from iselect.h
   switch (elementType)
   {
   case 1:  return cade::events::DEVICE_CATEGORY_FLIPPER;     // eItemFlipper
   case 8:  return cade::events::DEVICE_CATEGORY_BALL_DEVICE; // eItemKicker
   case 7:  return cade::events::DEVICE_CATEGORY_LIGHT;       // eItemLight
   default: return cade::events::DEVICE_CATEGORY_SWITCH;      // bumper, trigger, gate, spinner, target, surface, rubber, etc.
   }
}

cade::events::CategorizedEvent MapGameElementToCade(const char* elementName, int eventId, int elementType, const DeviceRegistry* registry)
{
   cade::events::CategorizedEvent event;
   event.set_category(cade::events::EVENT_CATEGORY_GENERAL);

   std::string deviceName = SanitizeUTF8(elementName);
   int vpxEvent = GameElementEventToVPXDeviceEvent(eventId);

   if (registry && registry->IsRegistered())
   {
      int cadeType = registry->GetCadeEventType(deviceName, vpxEvent);
      if (cadeType >= 0)
         event.set_event_type(cadeType);
      auto regCat = registry->GetDeviceCategory(deviceName);
      // Fall back to element-type-based category if registry doesn't have this device
      event.set_device_category(regCat != cade::events::DEVICE_CATEGORY_GENERAL ? regCat : DeviceCategoryForElementType(elementType));
   }
   else
   {
      event.set_device_category(DeviceCategoryForElementType(elementType));
      // Use default mapping based on VPX device event
      switch (vpxEvent)
      {
      case VPXDEV_EVENT_HIT:       event.set_event_type(CadeEventType::SwitchActivate); break;
      case VPXDEV_EVENT_UNHIT:     event.set_event_type(CadeEventType::SwitchDeactivate); break;
      case VPXDEV_EVENT_EOS:       event.set_event_type(CadeEventType::FlipperActivate); break;
      case VPXDEV_EVENT_BOS:       event.set_event_type(CadeEventType::FlipperDeactivate); break;
      case VPXDEV_EVENT_SLINGSHOT: event.set_event_type(CadeEventType::SwitchActivate); break;
      case VPXDEV_EVENT_SPIN:      event.set_event_type(CadeEventType::SwitchActivate); break;
      case VPXDEV_EVENT_DROPPED:   event.set_event_type(CadeEventType::SwitchActivate); break;
      case VPXDEV_EVENT_RAISED:    event.set_event_type(CadeEventType::SwitchDeactivate); break;
      default:                     event.set_event_type(CadeEventType::SwitchActivate); break;
      }
   }

   // Use negotiated scoring trigger name if available, otherwise fall back to default
   std::string scoringName;
   if (registry && registry->IsRegistered())
      scoringName = registry->GetCadeEventName(deviceName, vpxEvent);
   event.set_event_type_name(!scoringName.empty() ? scoringName : GameElementEventName(eventId));
   event.set_device_key(deviceName);
   event.set_device_type("vpx_element");
   SetTimestampNow(event.mutable_timestamp());
   return event;
}

cade::events::CategorizedEvent MapTableReadyToCade(const char* gameId)
{
   cade::events::CategorizedEvent event;
   event.set_category(cade::events::EVENT_CATEGORY_SYSTEM);
   event.set_device_category(cade::events::DEVICE_CATEGORY_UNSPECIFIED);
   event.set_event_type(CadeEventType::SystemTableReady);
   event.set_event_type_name("table_ready");
   if (gameId)
      (*event.mutable_metadata())["game_id"] = SanitizeUTF8(gameId);
   SetTimestampNow(event.mutable_timestamp());
   return event;
}

cade::events::CategorizedEvent MapTableStoppedToCade()
{
   cade::events::CategorizedEvent event;
   event.set_category(cade::events::EVENT_CATEGORY_SYSTEM);
   event.set_device_category(cade::events::DEVICE_CATEGORY_UNSPECIFIED);
   event.set_event_type(CadeEventType::SystemTableStopped);
   event.set_event_type_name("table_stopped");
   SetTimestampNow(event.mutable_timestamp());
   return event;
}

bool MapFlipperFromCade(const cade::events::CategorizedEvent& event, VPXAction& outAction, int& outPressed)
{
   if (event.device_category() != cade::events::DEVICE_CATEGORY_FLIPPER)
      return false;

   // Determine pressed state from event_type: 1=pressed, 0=released
   outPressed = (event.event_type() != 0) ? 1 : 0;

   // Map device_key to VPX action
   const std::string& key = event.device_key();
   if (key == "left" || key == "left_flipper")
      outAction = VPXACTION_LeftFlipper;
   else if (key == "right" || key == "right_flipper")
      outAction = VPXACTION_RightFlipper;
   else if (key == "staged_left" || key == "staged_left_flipper")
      outAction = VPXACTION_StagedLeftFlipper;
   else if (key == "staged_right" || key == "staged_right_flipper")
      outAction = VPXACTION_StagedRightFlipper;
   else
      return false;

   return true;
}

std::string SanitizeUTF8(const std::string& input)
{
   std::string out;
   out.reserve(input.size());
   const auto* p = reinterpret_cast<const uint8_t*>(input.data());
   const auto* end = p + input.size();

   while (p < end)
   {
      if (*p < 0x80)
      {
         // ASCII
         out.push_back(static_cast<char>(*p++));
      }
      else if ((*p & 0xE0) == 0xC0)
      {
         // 2-byte sequence
         if (p + 1 < end && (p[1] & 0xC0) == 0x80)
         {
            uint32_t cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
            if (cp >= 0x80)
            {
               out.push_back(static_cast<char>(p[0]));
               out.push_back(static_cast<char>(p[1]));
               p += 2;
            }
            else
            {
               // Overlong encoding
               out.append("\xEF\xBF\xBD"); // U+FFFD
               p++;
            }
         }
         else
         {
            out.append("\xEF\xBF\xBD");
            p++;
         }
      }
      else if ((*p & 0xF0) == 0xE0)
      {
         // 3-byte sequence
         if (p + 2 < end && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80)
         {
            uint32_t cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            if (cp >= 0x800 && (cp < 0xD800 || cp > 0xDFFF))
            {
               out.push_back(static_cast<char>(p[0]));
               out.push_back(static_cast<char>(p[1]));
               out.push_back(static_cast<char>(p[2]));
               p += 3;
            }
            else
            {
               out.append("\xEF\xBF\xBD");
               p++;
            }
         }
         else
         {
            out.append("\xEF\xBF\xBD");
            p++;
         }
      }
      else if ((*p & 0xF8) == 0xF0)
      {
         // 4-byte sequence
         if (p + 3 < end && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80)
         {
            uint32_t cp = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
            if (cp >= 0x10000 && cp <= 0x10FFFF)
            {
               out.push_back(static_cast<char>(p[0]));
               out.push_back(static_cast<char>(p[1]));
               out.push_back(static_cast<char>(p[2]));
               out.push_back(static_cast<char>(p[3]));
               p += 4;
            }
            else
            {
               out.append("\xEF\xBF\xBD");
               p++;
            }
         }
         else
         {
            out.append("\xEF\xBF\xBD");
            p++;
         }
      }
      else
      {
         // Invalid leading byte
         out.append("\xEF\xBF\xBD");
         p++;
      }
   }

   return out;
}

std::string SanitizeUTF8(const char* input)
{
   if (!input)
      return "unknown";
   return SanitizeUTF8(std::string(input));
}

} // namespace CadeBridge
