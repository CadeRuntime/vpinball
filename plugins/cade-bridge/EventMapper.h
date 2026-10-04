// license:GPLv3+

#pragma once

#include <string>
#include <vector>
#include "plugins/ControllerPlugin.h"
#include "plugins/VPXPlugin.h"
#include "events.pb.h"

namespace CadeBridge {

class DeviceRegistry;

// Read a numeric controller state (any CTLPI_STATE_FORMAT_xxx but STRING) as a double.
// Returns false if the state has no getter or a non numeric format.
bool ReadControllerState(const StateDef& def, double& value);

// Default cade device category of a controller state, derived from its semantic
// type and the name of its state block (e.g. "Solenoids" -> COIL)
cade::events::DeviceCategory ClassifyControllerState(const StateSrcId& src, const StateDef& def);

// VPX -> Cade: Build a CategorizedEvent from a controller state (switch, solenoid, lamp, ...) change
cade::events::CategorizedEvent MapStateToCade(const StateSrcId& src, unsigned int stateIndex, double value, const DeviceRegistry* registry = nullptr);

// VPX -> Cade: Build a CategorizedEvent from a VPX action state change (flipper, start, coin, etc.)
cade::events::CategorizedEvent MapActionToCade(VPXAction action, int isPressed, const DeviceRegistry* registry = nullptr);

// VPX -> Cade: Build a CategorizedEvent from a VPX table element event (bumper hit, target drop, etc.)
cade::events::CategorizedEvent MapGameElementToCade(const char* elementName, int eventId, int elementType, const DeviceRegistry* registry = nullptr);

// Cade -> VPX: Map an inbound action event to a VPXAction. Returns true if matched.
bool MapActionFromCade(const cade::events::CategorizedEvent& event, VPXAction& outAction, int& outPressed);

// VPX -> Cade: Build a platform readiness signal (table simulation loaded)
cade::events::CategorizedEvent MapTableReadyToCade(const char* gameId);

// VPX -> Cade: Build a platform readiness signal (table simulation ended)
cade::events::CategorizedEvent MapTableStoppedToCade();

// Cade -> VPX: Determine the VPX action for an inbound flipper event
// Returns true if a matching action was found, fills outAction and outPressed
bool MapFlipperFromCade(const cade::events::CategorizedEvent& event, VPXAction& outAction, int& outPressed);

// Build a device key for a controller state: its name, or "<state block id>:<mapping id>" when unnamed
std::string MakeDeviceKey(const StateSrcId& src, const StateDef& def);

// Build a device key string from a groupId and deviceId
std::string MakeDeviceKey(uint32_t groupId, uint32_t deviceId);

// Sanitize a string to valid UTF-8 for protobuf compatibility.
// Replaces invalid byte sequences with U+FFFD (replacement character).
std::string SanitizeUTF8(const std::string& input);

// Sanitize a C string to valid UTF-8, returning "unknown" for null pointers.
std::string SanitizeUTF8(const char* input);

} // namespace CadeBridge
