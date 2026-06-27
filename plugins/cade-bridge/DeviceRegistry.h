// license:GPLv3+

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "events.pb.h"
#include "plugins/VPXPlugin.h"
#include "plugins/ControllerPlugin.h"

namespace CadeBridge {

// Maps VPXDeviceEvent enum values for use in the event_type_map
// These must match the VPXDeviceEvent proto enum values
enum VPXDeviceEventId : int
{
   VPXDEV_EVENT_HIT = 1,
   VPXDEV_EVENT_UNHIT = 2,
   VPXDEV_EVENT_EOS = 3,
   VPXDEV_EVENT_BOS = 4,
   VPXDEV_EVENT_SLINGSHOT = 5,
   VPXDEV_EVENT_SPIN = 6,
   VPXDEV_EVENT_DROPPED = 7,
   VPXDEV_EVENT_RAISED = 8,
   VPXDEV_EVENT_ON = 9,
   VPXDEV_EVENT_OFF = 10,
   VPXDEV_EVENT_PRESSED = 11,
   VPXDEV_EVENT_RELEASED = 12,
   VPXDEV_EVENT_FLIPPER_COLLIDE = 13,
   VPXDEV_EVENT_BRIGHTNESS_CHANGE = 14,
};

// Default Cade EventType values (from pkg/cade/system/event_types.go iota)
namespace CadeEventType {
   constexpr int SystemReset = 0;       // Legacy: cade-internal game init
   constexpr int SystemInitGame = 2;    // Legacy: cade-internal game init
   constexpr int SystemTableReady = 50; // Platform signal: table simulation loaded
   constexpr int SystemTableStopped = 51; // Platform signal: table simulation ended
   constexpr int UserCoin = 3;
   constexpr int UserStart = 4;
   constexpr int UserBallPlunge = 5;
   constexpr int SwitchActivate = 9;
   constexpr int SwitchDeactivate = 10;
   constexpr int CoilPulse = 13;
   constexpr int CoilEnable = 14;
   constexpr int CoilDisable = 15;
   constexpr int LightOn = 19;
   constexpr int LightOff = 20;
   constexpr int LightBrightnessChange = 21;
   constexpr int FlipperActivate = 28;
   constexpr int FlipperDeactivate = 29;
   constexpr int BallDeviceEject = 33;
   constexpr int BallDeviceCapture = 34;
}

class DeviceRegistry
{
public:
   // Build manifest from all discovered sources
   cade::events::DeviceManifest BuildManifest(
      const std::vector<InputSrcId>& inputs,
      const std::vector<DevSrcId>& devices,
      const MsgPluginAPI* msgApi,
      unsigned int endpointId,
      unsigned int getGameElementsMsgId);

   // Store mappings from Cade's response (legacy: used when VPX is the client)
   void ApplyRegistration(const cade::events::RegistrationResponse& response);

   // Apply a PlatformConfigUpdate from cade (new: used when cade is the client).
   // Contains the same data as RegistrationResponse but delivered via the
   // PlatformEventFlow stream after cade loads .cade config.
   void ApplyConfigUpdate(const cade::events::PlatformConfigUpdate& update);

   // Apply default mappings (when Cade doesn't support registration)
   void ApplyDefaults();

   // Lookup: given device name + VPX device event, get Cade event_type
   // Returns -1 if no mapping found
   int GetCadeEventType(const std::string& deviceName, int vpxDeviceEvent) const;

   // Lookup: given device name + VPX device event, get Cade scoring trigger name.
   // Returns empty string if no mapping found.
   std::string GetCadeEventName(const std::string& deviceName, int vpxDeviceEvent) const;

   // Lookup: get device category for a device
   cade::events::DeviceCategory GetDeviceCategory(const std::string& deviceName) const;

   // Autofire rule definition (public so callers can inspect rule properties).
   struct AutofireRule {
      std::string name;
      std::string switchName;
      std::string coilName;
      std::string type;         // "hold" (flippers) or "pulse" (bumpers/slings)
      uint32_t pulseMs;
      uint32_t pulsePower;
      uint32_t recycleMs;
      bool enabled;
   };

   // Autofire rule management
   void ApplyAutofireRules(const google::protobuf::RepeatedPtrField<cade::events::AutofireRuleConfig>& rules);
   void EnableAutofireRule(const std::string& name);
   void DisableAutofireRule(const std::string& name);
   bool IsAutofireCoil(const std::string& coilName) const;
   void ClearAutofireRules();
   const AutofireRule* GetAutofireRuleForSwitch(const std::string& switchName) const;
   // Returns every rule bound to a switch. One input action can drive multiple
   // coils (e.g. two right flippers sharing the right_flipper button), so the
   // action handler fires each enabled rule.
   std::vector<const AutofireRule*> GetAutofireRulesForSwitch(const std::string& switchName) const;

   // Kicker kick parameters (pushed from cade via KickerConfig in
   // PlatformConfigUpdate / RegistrationResponse). The bridge stores them
   // locally so runtime KickBallCommand only needs to carry the device name.
   struct KickerParams {
      float angle;    // Kick yaw angle in degrees
      float strength; // Kick speed/force (driver-interpreted)
   };
   void ApplyKickerConfigs(const google::protobuf::RepeatedPtrField<cade::events::KickerConfig>& configs);
   void ClearKickerConfigs();
   const KickerParams* GetKickerParams(const std::string& deviceName) const;

   bool IsRegistered() const { return m_registered; }
   void Clear();

private:
   struct DeviceMapping {
      cade::events::DeviceCategory category;
      std::unordered_map<int, int> eventTypeMap; // VPXDeviceEvent → Cade EventType
      std::unordered_map<int, std::string> eventNameMap; // VPXDeviceEvent → Cade scoring trigger name
   };

   // Build default event type map for a device category
   static DeviceMapping DefaultMappingForCategory(cade::events::DeviceCategory cat);

   // Determine category and supported events from VPX element type
   static void ProfileForElementType(int elementType,
      cade::events::DeviceCategory& outCategory,
      std::vector<cade::events::VPXDeviceEvent>& outEvents,
      cade::events::SwitchNormality& outNormality);

   std::unordered_map<std::string, DeviceMapping> m_mappings;
   std::unordered_map<std::string, AutofireRule> m_autofireRules;  // rule name → rule
   std::unordered_map<std::string, std::string> m_coilToRule;      // coil name → rule name
   std::unordered_map<std::string, std::vector<std::string>> m_switchToRule; // switch name → rule names (one action may drive many coils)
   std::unordered_map<std::string, KickerParams> m_kickerParams;   // kicker name → kick params
   bool m_registered = false;
};

} // namespace CadeBridge
