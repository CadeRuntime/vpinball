// license:GPLv3+

#pragma once

class IEditable;

// Broadcast a game element event (bumper hit, target drop, spinner spin, etc.)
// to plugins via the VPX plugin API. Per-frame timer/animate events are filtered
// out before any allocation. Implemented in VPXPluginAPIImpl.cpp.
//
// This declaration lives in a cade-owned file so the only change to upstream
// ieditable.h is a single-line call inside the FireGroupEvent macro, keeping the
// rebase conflict surface minimal.
void VPXNotifyGameElementEvent(const IEditable& editable, int dispid);
