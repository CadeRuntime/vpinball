// license:GPLv3+

#include "DeviceRegistry.h"
#include "EventMapper.h"
#include "plugins/LoggingPlugin.h"
#include <algorithm>
#include <cctype>

namespace CadeBridge {

LPI_USE_CPP();
#define LOGD CadeBridge::LPI_LOGD_CPP
#define LOGI CadeBridge::LPI_LOGI_CPP
#define LOGW CadeBridge::LPI_LOGW_CPP
#define LOGE CadeBridge::LPI_LOGE_CPP

///////////////////////////////////////////////////////////////////////////////
// Element type to manifest profile mapping

// ItemTypeEnum values (from iselect.h) — only the interactive ones
static constexpr int eItemSurface  = 0;  // walls/slingshots — named slingshot surfaces fire SLINGSHOT events
static constexpr int eItemFlipper  = 1;
static constexpr int eItemPlunger = 3;
static constexpr int eItemBumper = 5;
static constexpr int eItemTrigger = 6;
static constexpr int eItemLight = 7;
static constexpr int eItemKicker = 8;
static constexpr int eItemGate = 10;
static constexpr int eItemSpinner = 11;
static constexpr int eItemHitTarget = 22;
static constexpr int eItemRubber = 21;

void DeviceRegistry::ProfileForElementType(int elementType,
   cade::events::DeviceCategory& outCategory,
   std::vector<cade::events::VPXDeviceEvent>& outEvents,
   cade::events::SwitchNormality& outNormality)
{
   using namespace cade::events;
   outNormality = SWITCH_NORMALLY_OPEN;

   switch (elementType)
   {
   case eItemSurface:
      // Named surface elements that fire slingshot events (e.g. LeftSlingShot, RightSlingShot).
      // Plain walls don't generate SLINGSHOT events so they won't produce any cade events.
      outCategory = DEVICE_CATEGORY_SWITCH;
      outEvents = { VPX_EVENT_SLINGSHOT };
      break;
   case eItemBumper:
      outCategory = DEVICE_CATEGORY_SWITCH;
      outEvents = { VPX_EVENT_HIT, VPX_EVENT_UNHIT };
      break;
   case eItemHitTarget:
      outCategory = DEVICE_CATEGORY_SWITCH;
      outEvents = { VPX_EVENT_HIT, VPX_EVENT_UNHIT, VPX_EVENT_DROPPED, VPX_EVENT_RAISED };
      break;
   case eItemTrigger:
      outCategory = DEVICE_CATEGORY_SWITCH;
      outEvents = { VPX_EVENT_HIT, VPX_EVENT_UNHIT };
      break;
   case eItemGate:
      outCategory = DEVICE_CATEGORY_SWITCH;
      outEvents = { VPX_EVENT_HIT, VPX_EVENT_UNHIT };
      break;
   case eItemSpinner:
      outCategory = DEVICE_CATEGORY_SWITCH;
      outEvents = { VPX_EVENT_SPIN };
      break;
   case eItemFlipper:
      outCategory = DEVICE_CATEGORY_FLIPPER;
      outEvents = { VPX_EVENT_HIT, VPX_EVENT_EOS, VPX_EVENT_BOS, VPX_EVENT_FLIPPER_COLLIDE };
      outNormality = SWITCH_NORMALITY_UNSPECIFIED;
      break;
   case eItemKicker:
      outCategory = DEVICE_CATEGORY_BALL_DEVICE;
      outEvents = { VPX_EVENT_HIT, VPX_EVENT_UNHIT };
      outNormality = SWITCH_NORMALITY_UNSPECIFIED;
      break;
   case eItemLight:
      outCategory = DEVICE_CATEGORY_LIGHT;
      outEvents = { VPX_EVENT_ON, VPX_EVENT_OFF, VPX_EVENT_BRIGHTNESS_CHANGE };
      outNormality = SWITCH_NORMALITY_UNSPECIFIED;
      break;
   case eItemPlunger:
      outCategory = DEVICE_CATEGORY_SWITCH;
      outEvents = { VPX_EVENT_HIT };
      break;
   default:
      outCategory = DEVICE_CATEGORY_GENERAL;
      outEvents = { VPX_EVENT_HIT, VPX_EVENT_UNHIT };
      outNormality = SWITCH_NORMALITY_UNSPECIFIED;
      break;
   }
}

///////////////////////////////////////////////////////////////////////////////
// Default mapping tables

DeviceRegistry::DeviceMapping DeviceRegistry::DefaultMappingForCategory(cade::events::DeviceCategory cat)
{
   DeviceMapping m;
   m.category = cat;

   switch (cat)
   {
   case cade::events::DEVICE_CATEGORY_SWITCH:
      m.eventTypeMap = {
         { VPXDEV_EVENT_HIT, CadeEventType::SwitchActivate },
         { VPXDEV_EVENT_UNHIT, CadeEventType::SwitchDeactivate },
         { VPXDEV_EVENT_SLINGSHOT, CadeEventType::SwitchActivate },
         { VPXDEV_EVENT_SPIN, CadeEventType::SwitchActivate },
         { VPXDEV_EVENT_DROPPED, CadeEventType::SwitchActivate },
         { VPXDEV_EVENT_RAISED, CadeEventType::SwitchDeactivate },
         { VPXDEV_EVENT_ON, CadeEventType::SwitchActivate },
         { VPXDEV_EVENT_OFF, CadeEventType::SwitchDeactivate },
         { VPXDEV_EVENT_PRESSED, CadeEventType::SwitchActivate },
         { VPXDEV_EVENT_RELEASED, CadeEventType::SwitchDeactivate },
      };
      break;
   case cade::events::DEVICE_CATEGORY_FLIPPER:
      m.eventTypeMap = {
         { VPXDEV_EVENT_HIT, CadeEventType::FlipperActivate },
         { VPXDEV_EVENT_EOS, CadeEventType::FlipperActivate },
         { VPXDEV_EVENT_BOS, CadeEventType::FlipperDeactivate },
         { VPXDEV_EVENT_FLIPPER_COLLIDE, CadeEventType::FlipperActivate },
         { VPXDEV_EVENT_PRESSED, CadeEventType::FlipperActivate },
         { VPXDEV_EVENT_RELEASED, CadeEventType::FlipperDeactivate },
      };
      break;
   case cade::events::DEVICE_CATEGORY_COIL:
      m.eventTypeMap = {
         { VPXDEV_EVENT_ON, CadeEventType::CoilEnable },
         { VPXDEV_EVENT_OFF, CadeEventType::CoilDisable },
      };
      break;
   case cade::events::DEVICE_CATEGORY_LIGHT:
      m.eventTypeMap = {
         { VPXDEV_EVENT_ON, CadeEventType::LightOn },
         { VPXDEV_EVENT_OFF, CadeEventType::LightOff },
         { VPXDEV_EVENT_BRIGHTNESS_CHANGE, CadeEventType::LightBrightnessChange },
      };
      break;
   case cade::events::DEVICE_CATEGORY_BALL_DEVICE:
      m.eventTypeMap = {
         { VPXDEV_EVENT_HIT, CadeEventType::BallDeviceCapture },
         { VPXDEV_EVENT_UNHIT, CadeEventType::BallDeviceEject },
      };
      break;
   default:
      m.eventTypeMap = {
         { VPXDEV_EVENT_HIT, CadeEventType::SwitchActivate },
         { VPXDEV_EVENT_UNHIT, CadeEventType::SwitchDeactivate },
         { VPXDEV_EVENT_ON, CadeEventType::SwitchActivate },
         { VPXDEV_EVENT_OFF, CadeEventType::SwitchDeactivate },
      };
      break;
   }

   return m;
}

///////////////////////////////////////////////////////////////////////////////
// Manifest building

cade::events::DeviceManifest DeviceRegistry::BuildManifest(
   const std::vector<InputSrcId>& inputs,
   const std::vector<DevSrcId>& devices,
   const MsgPluginAPI* msgApi,
   unsigned int endpointId,
   unsigned int getGameElementsMsgId)
{
   using namespace std::string_literals;
   cade::events::DeviceManifest manifest;

   // 1. Controller inputs (switches from controller/ROM layer)
   for (const auto& src : inputs)
   {
      for (unsigned int i = 0; i < src.nInputs; i++)
      {
         const DeviceDef& def = src.inputDefs[i];
         auto* entry = manifest.add_devices();
         entry->set_name(def.name ? SanitizeUTF8(def.name) : (std::to_string(def.id.groupId) + ":" + std::to_string(def.id.deviceId)));
         entry->set_category(cade::events::DEVICE_CATEGORY_SWITCH);
         entry->add_supported_events(cade::events::VPX_EVENT_ON);
         entry->add_supported_events(cade::events::VPX_EVENT_OFF);
         entry->set_normality(cade::events::SWITCH_NORMALLY_OPEN);
         (*entry->mutable_metadata())["source"] = "controller_input";
         (*entry->mutable_metadata())["group_id"] = std::to_string(def.id.groupId);
         (*entry->mutable_metadata())["device_id"] = std::to_string(def.id.deviceId);
      }
   }

   // 2. Controller devices (coils, lights from controller/ROM layer)
   for (const auto& src : devices)
   {
      for (unsigned int i = 0; i < src.nDevices; i++)
      {
         const DeviceDef& def = src.deviceDefs[i];
         auto* entry = manifest.add_devices();
         entry->set_name(def.name ? SanitizeUTF8(def.name) : (std::to_string(def.id.groupId) + ":" + std::to_string(def.id.deviceId)));

         cade::events::DeviceCategory cat;
         switch (def.id.groupId)
         {
         case 0:
            cat = cade::events::DEVICE_CATEGORY_COIL;
            entry->add_supported_events(cade::events::VPX_EVENT_ON);
            entry->add_supported_events(cade::events::VPX_EVENT_OFF);
            break;
         case 1:
            cat = cade::events::DEVICE_CATEGORY_LIGHT;
            entry->add_supported_events(cade::events::VPX_EVENT_ON);
            entry->add_supported_events(cade::events::VPX_EVENT_OFF);
            entry->add_supported_events(cade::events::VPX_EVENT_BRIGHTNESS_CHANGE);
            break;
         default:
            cat = cade::events::DEVICE_CATEGORY_GENERAL;
            entry->add_supported_events(cade::events::VPX_EVENT_ON);
            entry->add_supported_events(cade::events::VPX_EVENT_OFF);
            break;
         }
         entry->set_category(cat);
         (*entry->mutable_metadata())["source"] = "controller_device";
         (*entry->mutable_metadata())["group_id"] = std::to_string(def.id.groupId);
         (*entry->mutable_metadata())["device_id"] = std::to_string(def.id.deviceId);
      }
   }

   // 3. Table elements (bumpers, flippers, targets, etc.)
   {
      // First pass: count
      VPXGetGameElementsMsg elemMsg {};
      elemMsg.maxEntryCount = 0;
      elemMsg.count = 0;
      elemMsg.entries = nullptr;
      msgApi->BroadcastMsg(endpointId, getGameElementsMsgId, &elemMsg);

      if (elemMsg.count > 0)
      {
         // Second pass: get entries
         unsigned int count = elemMsg.count;
         std::vector<VPXGameElementInfo> entries(count);
         elemMsg.maxEntryCount = count;
         elemMsg.count = 0;
         elemMsg.entries = entries.data();
         msgApi->BroadcastMsg(endpointId, getGameElementsMsgId, &elemMsg);

         unsigned int actual = std::min(elemMsg.count, count);
         for (unsigned int i = 0; i < actual; i++)
         {
            cade::events::DeviceCategory cat;
            std::vector<cade::events::VPXDeviceEvent> events;
            cade::events::SwitchNormality normality;
            ProfileForElementType(entries[i].elementType, cat, events, normality);

            auto* entry = manifest.add_devices();
            entry->set_name(SanitizeUTF8(entries[i].name));
            entry->set_category(cat);
            for (auto evt : events)
               entry->add_supported_events(evt);
            if (normality != cade::events::SWITCH_NORMALITY_UNSPECIFIED)
               entry->set_normality(normality);
            (*entry->mutable_metadata())["source"] = "table_element";
            (*entry->mutable_metadata())["element_type"] = std::to_string(entries[i].elementType);
         }
      }
   }

   // 4. Actions (always present — flippers, nudge, start, coin, etc.)
   struct ActionEntry {
      const char* name;
      cade::events::DeviceCategory category;
   };
   static const ActionEntry actions[] = {
      { "left_flipper", cade::events::DEVICE_CATEGORY_FLIPPER },
      { "right_flipper", cade::events::DEVICE_CATEGORY_FLIPPER },
      { "staged_left_flipper", cade::events::DEVICE_CATEGORY_FLIPPER },
      { "staged_right_flipper", cade::events::DEVICE_CATEGORY_FLIPPER },
      { "left_magna_save", cade::events::DEVICE_CATEGORY_SWITCH },
      { "right_magna_save", cade::events::DEVICE_CATEGORY_SWITCH },
      { "launch_ball", cade::events::DEVICE_CATEGORY_SWITCH },
      { "left_nudge", cade::events::DEVICE_CATEGORY_SWITCH },
      { "center_nudge", cade::events::DEVICE_CATEGORY_SWITCH },
      { "right_nudge", cade::events::DEVICE_CATEGORY_SWITCH },
      { "tilt", cade::events::DEVICE_CATEGORY_SWITCH },
      { "add_credit", cade::events::DEVICE_CATEGORY_SWITCH },
      { "add_credit2", cade::events::DEVICE_CATEGORY_SWITCH },
      { "start_game", cade::events::DEVICE_CATEGORY_SWITCH },
      { "lockbar", cade::events::DEVICE_CATEGORY_SWITCH },
      { "exit_game", cade::events::DEVICE_CATEGORY_SWITCH },
   };
   for (const auto& act : actions)
   {
      auto* entry = manifest.add_devices();
      entry->set_name(act.name);
      entry->set_category(act.category);
      entry->add_supported_events(cade::events::VPX_EVENT_PRESSED);
      entry->add_supported_events(cade::events::VPX_EVENT_RELEASED);
      (*entry->mutable_metadata())["source"] = "vpx_action";
   }

   LOGI("CadeBridge: built device manifest with "s + std::to_string(manifest.devices_size()) + " devices");
   return manifest;
}

///////////////////////////////////////////////////////////////////////////////
// Registration handling

void DeviceRegistry::ApplyRegistration(const cade::events::RegistrationResponse& response)
{
   using namespace std::string_literals;
   m_mappings.clear();

   for (const auto& dm : response.device_mappings())
   {
      DeviceMapping mapping;
      mapping.category = dm.device_category();
      for (const auto& [vpxEvt, cadeType] : dm.event_type_map())
         mapping.eventTypeMap[vpxEvt] = cadeType;
      for (const auto& [vpxEvt, name] : dm.event_name_map())
         mapping.eventNameMap[vpxEvt] = name;
      m_mappings[dm.device_name()] = std::move(mapping);
   }

   m_registered = true;

   // Apply autofire rules if present
   if (response.autofire_rules_size() > 0)
      ApplyAutofireRules(response.autofire_rules());

   // Apply kicker configs if present
   if (response.kicker_configs_size() > 0)
      ApplyKickerConfigs(response.kicker_configs());

   // Log warnings
   for (const auto& warn : response.warnings())
      LOGW("CadeBridge: registration warning for '"s + warn.device_name() + "': " + warn.message());

   LOGI("CadeBridge: registration complete, "s + std::to_string(m_mappings.size()) + " device mappings, "
      + std::to_string(m_autofireRules.size()) + " autofire rules, "
      + std::to_string(m_kickerParams.size()) + " kicker configs cached");
}

void DeviceRegistry::ApplyConfigUpdate(const cade::events::PlatformConfigUpdate& update)
{
   using namespace std::string_literals;
   m_mappings.clear();

   for (const auto& dm : update.device_mappings())
   {
      DeviceMapping mapping;
      mapping.category = dm.device_category();
      for (const auto& [vpxEvt, cadeType] : dm.event_type_map())
         mapping.eventTypeMap[vpxEvt] = cadeType;
      for (const auto& [vpxEvt, name] : dm.event_name_map())
         mapping.eventNameMap[vpxEvt] = name;
      m_mappings[dm.device_name()] = std::move(mapping);
   }

   m_registered = true;

   // Apply autofire rules if present
   if (update.autofire_rules_size() > 0)
      ApplyAutofireRules(update.autofire_rules());

   // Apply kicker configs if present
   if (update.kicker_configs_size() > 0)
      ApplyKickerConfigs(update.kicker_configs());

   // Log warnings
   for (const auto& warn : update.warnings())
      LOGW("CadeBridge: config update warning for '"s + warn.device_name() + "': " + warn.message());

   LOGI("CadeBridge: config update v"s + std::to_string(update.config_version())
      + " applied, " + std::to_string(m_mappings.size()) + " device mappings, "
      + std::to_string(m_autofireRules.size()) + " autofire rules, "
      + std::to_string(m_kickerParams.size()) + " kicker configs");
}

void DeviceRegistry::ApplyDefaults()
{
   using namespace std::string_literals;

   // Apply default mappings for all devices currently tracked
   // This is called when Cade doesn't support registration (timeout/old version)
   // The mappings will be populated on-demand via GetCadeEventType fallback

   m_registered = true;
   LOGI("CadeBridge: using default event type mappings (Cade did not respond to registration)"s);
}

int DeviceRegistry::GetCadeEventType(const std::string& deviceName, int vpxDeviceEvent) const
{
   auto it = m_mappings.find(deviceName);
   if (it != m_mappings.end())
   {
      auto evtIt = it->second.eventTypeMap.find(vpxDeviceEvent);
      if (evtIt != it->second.eventTypeMap.end())
         return evtIt->second;
   }

   // Fallback: use defaults based on the event itself
   // This handles devices not in the registry (unmatched or default mode)
   switch (vpxDeviceEvent)
   {
   case VPXDEV_EVENT_HIT:       return CadeEventType::SwitchActivate;
   case VPXDEV_EVENT_UNHIT:     return CadeEventType::SwitchDeactivate;
   case VPXDEV_EVENT_EOS:       return CadeEventType::FlipperActivate;
   case VPXDEV_EVENT_BOS:       return CadeEventType::FlipperDeactivate;
   case VPXDEV_EVENT_SLINGSHOT: return CadeEventType::SwitchActivate;
   case VPXDEV_EVENT_SPIN:      return CadeEventType::SwitchActivate;
   case VPXDEV_EVENT_DROPPED:   return CadeEventType::SwitchActivate;
   case VPXDEV_EVENT_RAISED:    return CadeEventType::SwitchDeactivate;
   case VPXDEV_EVENT_ON:        return CadeEventType::SwitchActivate;
   case VPXDEV_EVENT_OFF:       return CadeEventType::SwitchDeactivate;
   case VPXDEV_EVENT_PRESSED:   return CadeEventType::SwitchActivate;
   case VPXDEV_EVENT_RELEASED:  return CadeEventType::SwitchDeactivate;
   default: return -1;
   }
}

std::string DeviceRegistry::GetCadeEventName(const std::string& deviceName, int vpxDeviceEvent) const
{
   auto it = m_mappings.find(deviceName);
   if (it != m_mappings.end())
   {
      auto nameIt = it->second.eventNameMap.find(vpxDeviceEvent);
      if (nameIt != it->second.eventNameMap.end())
         return nameIt->second;
   }
   return "";
}

cade::events::DeviceCategory DeviceRegistry::GetDeviceCategory(const std::string& deviceName) const
{
   auto it = m_mappings.find(deviceName);
   if (it != m_mappings.end())
      return it->second.category;
   return cade::events::DEVICE_CATEGORY_GENERAL;
}

void DeviceRegistry::Clear()
{
   m_mappings.clear();
   ClearAutofireRules();
   ClearKickerConfigs();
   m_registered = false;
}

///////////////////////////////////////////////////////////////////////////////
// Autofire rule management

// Convert a snake_case identifier into a PascalCase one. Cade sends device
// names in snake_case (e.g. "left_flipper"), but VPX table elements are
// authored in PascalCase (e.g. "LeftFlipper"); hold-type autofire rules
// resolve their coilName through PinTable::GetElementByName which is an
// exact string match, so we transform once at rule-apply time rather than
// per-press. Already-PascalCase inputs pass through unchanged because they
// contain no underscores and the first character is already uppercase.
static std::string SnakeToPascal(const std::string& in)
{
   std::string out;
   out.reserve(in.size());
   bool atWordStart = true;
   for (char c : in)
   {
      if (c == '_')
      {
         atWordStart = true;
         continue;
      }
      if (atWordStart)
      {
         out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
         atWordStart = false;
      }
      else
      {
         out.push_back(c);
      }
   }
   return out;
}

void DeviceRegistry::ApplyAutofireRules(const google::protobuf::RepeatedPtrField<cade::events::AutofireRuleConfig>& rules)
{
   using namespace std::string_literals;
   ClearAutofireRules();

   for (const auto& rule : rules)
   {
      AutofireRule ar;
      ar.name = rule.name();
      ar.switchName = rule.switch_name();
      ar.coilName = rule.coil_name();
      ar.pulseMs = rule.pulse_ms();
      ar.pulsePower = rule.pulse_power();
      ar.recycleMs = rule.recycle_ms();
      ar.enabled = rule.enabled();
      ar.type = rule.type().empty() ? "pulse" : rule.type();

      // Hold-type rules (flippers) resolve coilName through
      // VPXPluginAPIImpl::SetFlipperState -> PinTable::GetElementByName, which
      // is an exact string match against PascalCase table element names. Cade
      // ships rules keyed on the driver-agnostic device name (snake_case), so
      // transform the stored coilName once here. The switchName is left in
      // snake_case because onActionChanged looks it up against the VPX action
      // device_key, which is already snake_case (e.g. "left_flipper").
      if (ar.type == "hold")
      {
         std::string resolvedCoil = SnakeToPascal(ar.coilName);
         if (resolvedCoil != ar.coilName)
         {
            LOGD("CadeBridge: hold-rule coil '"s + ar.coilName + "' -> VPX element '" + resolvedCoil + "'");
            ar.coilName = std::move(resolvedCoil);
         }
      }

      m_coilToRule[ar.coilName] = ar.name;
      m_switchToRule[ar.switchName].push_back(ar.name);
      m_autofireRules[ar.name] = std::move(ar);
   }

   LOGI("CadeBridge: applied "s + std::to_string(m_autofireRules.size()) + " autofire rules");
}

void DeviceRegistry::EnableAutofireRule(const std::string& name)
{
   using namespace std::string_literals;
   auto it = m_autofireRules.find(name);
   if (it != m_autofireRules.end())
   {
      it->second.enabled = true;
      LOGI("CadeBridge: autofire rule '"s + name + "' enabled");
   }
   else
   {
      LOGW("CadeBridge: unknown autofire rule '"s + name + "' (enable ignored)");
   }
}

void DeviceRegistry::DisableAutofireRule(const std::string& name)
{
   using namespace std::string_literals;
   auto it = m_autofireRules.find(name);
   if (it != m_autofireRules.end())
   {
      it->second.enabled = false;
      LOGI("CadeBridge: autofire rule '"s + name + "' disabled");
   }
   else
   {
      LOGW("CadeBridge: unknown autofire rule '"s + name + "' (disable ignored)");
   }
}

bool DeviceRegistry::IsAutofireCoil(const std::string& coilName) const
{
   auto it = m_coilToRule.find(coilName);
   if (it == m_coilToRule.end())
      return false;
   // Only considered autofire-managed if the rule is currently enabled
   auto ruleIt = m_autofireRules.find(it->second);
   return ruleIt != m_autofireRules.end() && ruleIt->second.enabled;
}

const DeviceRegistry::AutofireRule* DeviceRegistry::GetAutofireRuleForSwitch(const std::string& switchName) const
{
   auto it = m_switchToRule.find(switchName);
   if (it == m_switchToRule.end() || it->second.empty())
      return nullptr;
   // Returns the first rule bound to the switch. Used by the element-event gate
   // (one rule per element switch). Callers that may drive several coils from a
   // single action use GetAutofireRulesForSwitch instead.
   auto ruleIt = m_autofireRules.find(it->second.front());
   if (ruleIt == m_autofireRules.end())
      return nullptr;
   // Return the rule regardless of enabled state so callers can distinguish
   // "no rule" (nullptr) from "rule exists but disabled" (ptr with enabled=false).
   return &ruleIt->second;
}

std::vector<const DeviceRegistry::AutofireRule*> DeviceRegistry::GetAutofireRulesForSwitch(const std::string& switchName) const
{
   std::vector<const AutofireRule*> rules;
   auto it = m_switchToRule.find(switchName);
   if (it == m_switchToRule.end())
      return rules;
   rules.reserve(it->second.size());
   for (const auto& ruleName : it->second)
   {
      auto ruleIt = m_autofireRules.find(ruleName);
      if (ruleIt != m_autofireRules.end())
         rules.push_back(&ruleIt->second);
   }
   return rules;
}

void DeviceRegistry::ClearAutofireRules()
{
   m_autofireRules.clear();
   m_coilToRule.clear();
   m_switchToRule.clear();
}

///////////////////////////////////////////////////////////////////////////////
// Kicker config management

void DeviceRegistry::ApplyKickerConfigs(const google::protobuf::RepeatedPtrField<cade::events::KickerConfig>& configs)
{
   using namespace std::string_literals;
   ClearKickerConfigs();

   for (const auto& cfg : configs)
   {
      KickerParams params;
      params.angle = cfg.kick_angle();
      params.strength = cfg.kick_strength();
      m_kickerParams[cfg.device_name()] = params;
   }

   LOGI("CadeBridge: applied "s + std::to_string(m_kickerParams.size()) + " kicker configs");
}

void DeviceRegistry::ClearKickerConfigs()
{
   m_kickerParams.clear();
}

const DeviceRegistry::KickerParams* DeviceRegistry::GetKickerParams(const std::string& deviceName) const
{
   auto it = m_kickerParams.find(deviceName);
   if (it == m_kickerParams.end())
      return nullptr;
   return &it->second;
}

} // namespace CadeBridge
