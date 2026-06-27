// license:GPLv3+

#pragma once

#include "MsgPlugin.h"
#include "ControllerPlugin.h"

///////////////////////////////////////////////////////////////////////////////
// VPX plugins
//
// WARNING WARNING WARNING WARNING WARNING WARNING WARNING WARNING WARNING WARNING
// This interface is part of a work in progress and will evolve likely a lot
// before being considered stable. Do not use it, or if you do, use it knowing
// that you're plugin will be broken by the upcoming updates.
// WARNING WARNING WARNING WARNING WARNING WARNING WARNING WARNING WARNING WARNING
//
// Plugins are a simple way to extend VPX core features. VPinballX application
// will scan for plugins in its 'plugin' folder, searching for subfolders
// containing a 'plugin.cfg' file. When found, the file will be read to find
// the needed metadata to present to the user as well as path for the native
// builds of the plugins for each supported platform. Then, the plugin will be
// available for the end user to enable it from the application settings.
// Another option is to statically build plugins into the application.
//
// This API takes for granted that at any time, only one game can be played. 
// All calls related to a running game may only be called between the 'OnGameStart' 
// and 'OnGameEnd' events.


#define VPXPI_NAMESPACE "VPX" // Namespace used for all VPX message definition

// Core VPX messages
#define VPXPI_MSG_GET_API               "GetAPI"              // Get the main VPX plugin API

// Core VPX events
#define VPXPI_EVT_ON_GAME_START         "OnGameStart"         // Broadcasted during player creation, before script initialization
#define VPXPI_EVT_ON_GAME_END           "OnGameEnd"           // Broadcasted during player shutdown
#define VPXPI_EVT_ON_PREPARE_FRAME      "OnPrepareFrame"      // Broadcasted when player starts preparing a new frame
#define VPXPI_EVT_ON_UPDATE_PHYSICS     "OnUpdatePhysics"     // Broadcasted when player update physics (happens often, so must be used with care)
#define VPXPI_EVT_ON_ACTION_CHANGED     "OnActionChanged"     // Broadcasted when an action state change, event data is an VPXActionEvent whose isPressed field can be modified by plugins
#define VPXPI_EVT_ON_GAME_ELEMENT      "OnGameElement"       // Broadcasted when a table element fires a script event (hit, unhit, spin, etc.), event data is a VPXGameElementEvent
#define VPXPI_MSG_GET_GAME_ELEMENTS    "GetGameElements"     // Broadcasted with a VPXGetGameElementsMsg to enumerate interactive table elements

// Ancillary window rendering
#define VPXPI_MSG_GET_AUX_RENDERER      "GetAuxRenderer"      // Broadcasted with a GetAncillaryRendererMsg to discover ancillary window renderer implemented in plugins
#define VPXPI_EVT_AUX_RENDERER_CHG      "AuxRendererChanged"  // Broadcasted when an ancillary renderer is added or removed

typedef void* VPXTexture;

typedef enum
{
   VPXTEXFMT_BW32F,
   VPXTEXFMT_sRGB8,
   VPXTEXFMT_sRGBA8,
   VPXTEXFMT_sRGB565,
} VPXTextureFormat;

typedef struct VPXTextureInfo
{
   unsigned int width;
   unsigned int height;
   VPXTextureFormat format;
   void* data;
} VPXTextureInfo;

typedef enum
{
   VPXWINDOW_Playfield,
   VPXWINDOW_Backglass,
   VPXWINDOW_ScoreView,
   VPXWINDOW_Topper,
   VPXWINDOW_VRPreview,
} VPXWindowId;

typedef enum
{
   VPXDMDStyle_Legacy,
   VPXDMDStyle_Plasma,
   VPXDMDStyle_RedLED,
   VPXDMDStyle_GreenLED,
   VPXDMDStyle_YellowLED,
   VPXDMDStyle_GenPlasma,
   VPXDMDStyle_GenLED,
   VPXDMDStyle_Pixelated,
   VPXDMDStyle_Smoothed,
   VPXDMDStyle_CRT,
} VPXDisplayRenderStyle;

typedef enum
{
   VPXSegStyle_Plasma,
   VPXSegStyle_BlueVFD,
   VPXSegStyle_GreenVFD,
   VPXSegStyle_RedLED,
   VPXSegStyle_GreenLED,
   VPXSegStyle_YellowLED,
   VPXSegStyle_GenPlasma,
   VPXSegStyle_GenLED,
} VPXSegDisplayRenderStyle;

typedef enum
{
   Generic,
   Gottlieb,
   Williams,
   Bally,
   Atari,
} VPXSegDisplayHint;

typedef struct VPXRenderContext2D
{
   VPXWindowId window;        // Target window
   float srcWidth;            // Source surface width, used in DrawImage call, default to target surface width
   float srcHeight;           // Source surface height, used in DrawImage call, default to target surface height
   int is2D;                  // If true, the rendering is done in 2D mode, otherwise in 3D mode
   float outWidth;            // Target surface width for 2D render, otherwise hint to be used for aspect ratio computation, LOD and layout
   float outHeight;           // Target surface height for 2D render, otherwise hint to be used for apsect ratio computation, LOD and layout
   void(MSGPIAPI* DrawImage)(VPXRenderContext2D* ctx, VPXTexture texture,
      const float tintR, const float tintG, const float tintB, const float alpha, // tint color and alpha (0..1)
      const float texX, const float texY, const float texW, const float texH,  // coordinates in texture surface (0..tex.width, 0..tex.height)
      const float pivotX, const float pivotY, const float rotation, // pivot point in texture surface & rotation in degrees
      const float srcX, const float srcY, const float srcW, const float srcH); // coordinates in source surface (0..srcWidth, 0..srcHeight)
   void(MSGPIAPI* DrawDisplay)(VPXRenderContext2D* ctx, VPXDisplayRenderStyle style,
      // First layer is an optional tinted glass
      VPXTexture glassTex, const float glassTintR, const float glassTintG, const float glassTintB, const float glassRoughness,
      const float glassAreaX, const float glassAreaY, const float glassAreaW, const float glassAreaH,
      const float glassAmbientR, const float glassAmbientG, const float glassAmbientB,
      // Second layer is the emitter
      VPXTexture dispTex, const float dispTintR, const float dispTintG, const float dispTintB, const float brightness, const float alpha, // Emitter texture, tint, brightness and alpha
      const float dispPadL, const float dispPadT, const float dispPadR, const float dispPadB, // Display padding from glass bounds
      //
      const float srcX, const float srcY, const float srcW, const float srcH); // coordinates in source surface (0..srcWidth, 0..srcHeight)
   void(MSGPIAPI* DrawSegDisplay)(VPXRenderContext2D* ctx, VPXSegDisplayRenderStyle style, VPXSegDisplayHint shapeHint,
      // First layer is an optional tinted glass
      VPXTexture glassTex, const float glassTintR, const float glassTintG, const float glassTintB, const float glassRoughness, const float glassAreaX, const float glassAreaY,
      const float glassAreaW, const float glassAreaH, const float glassAmbientR, const float glassAmbientG, const float glassAmbientB,
      // Second layer is the emitter
      SegElementType type, const float* state, const float dispTintR, const float dispTintG, const float dispTintB, const float brightness, const float alpha, // Emitter, tint, brightness and alpha
      const float dispPadL, const float dispPadT, const float dispPadR, const float dispPadB, // Display padding from glass bounds
      //
      const float srcX, const float srcY, const float srcW, const float srcH); // coordinates in source surface (0..srcWidth, 0..srcHeight)
   void* rendererData; // Custom renderer data pointer to be used by the rendering functions
} VPXRenderContext2D;

typedef struct AncillaryRendererDef
{
   const char* id;            // Unique id of the renderer (only alphanumeric characters, dots and underscore. Can be used for storing settings)
   const char* name;          // Human readable name of the renderer
   const char* description;   // Human readable description of the renderer
   void* context;             // Custom context to be passed when requesting rendering
   int(MSGPIAPI* Render)(VPXRenderContext2D* renderCtx, void* context);
} AncillaryRendererDef;

typedef struct GetAncillaryRendererMsg
{
   // Request
   VPXWindowId window; // Target window
   unsigned int maxEntryCount; // see below
   // Response
   unsigned int count; // Number of entries, also position to put next entry
   AncillaryRendererDef* entries; // Pointer to an array of maxEntryCount entries to be filled
} GetAncillaryRendererMsg;


// Helper defines

// Conversions to/from VP units (50 VPU = 1.0625 inches which is 1"1/16, the default size of a ball, 1 inch is 2.54cm)
// These value are very slightly off from original values which used a VPU to MM of 0.540425 instead of 0.53975 (result of the following formula)
// So it used to be 0.125% larger which is not noticeable but makes it difficult to have perfect matches when playing between apps
#ifndef MMTOVPU
#define MMTOVPU(x) ((x) * (float)(50. / (25.4 * 1.0625)))
#define CMTOVPU(x) ((x) * (float)(50. / (2.54 * 1.0625)))
#define VPUTOMM(x) ((x) * (float)(25.4 * 1.0625 / 50.))
#define VPUTOCM(x) ((x) * (float)(2.54 * 1.0625 / 50.))
#define INCHESTOVPU(x) ((x) * (float)(50. / 1.0625))
#define VPUTOINCHES(x) ((x) * (float)(1.0625 / 50.))
#endif

typedef struct VPXInfo
{
   const char* path;              // [R_]
   const char* prefPath;          // [R_]
} VPXInfo;

typedef struct VPXTableInfo
{
   const char* path;              // [R_]
   float tableWidth, tableHeight; // [R_]
} VPXTableInfo;

typedef struct VPXViewSetupDef
{
   // See ViewSetup class for member description
   int viewMode;                                       // [R_]
   float sceneScaleX, sceneScaleY, sceneScaleZ;        // [R_]
   float viewX, viewY, viewZ;                          // [RW]
   float lookAt;                                       // [R_]
   float viewportRotation;                             // [R_]
   float FOV;                                          // [R_]
   float layback;                                      // [R_]
   float viewHOfs, viewVOfs;                           // [R_]
   float windowTopZOfs, windowBottomZOfs;              // [R_]
   // Following fields are defined in user settings
   float screenWidth, screenHeight, screenInclination; // [R_]
   float realToVirtualScale;                           // [R_]
   float interpupillaryDistance;                       // [R_] TODO upgrade to RW to allow head tracking to measure and adjust accordingly
} VPXViewSetupDef;

typedef enum
{
   VPXACTION_LeftFlipper,
   VPXACTION_RightFlipper,
   VPXACTION_StagedLeftFlipper,
   VPXACTION_StagedRightFlipper,
   VPXACTION_LeftMagnaSave,
   VPXACTION_RightMagnaSave,
   VPXACTION_LaunchBall,
   VPXACTION_LeftNudge,
   VPXACTION_CenterNudge,
   VPXACTION_RightNudge,
   VPXACTION_Tilt,
   VPXACTION_AddCredit,
   VPXACTION_AddCredit2,
   VPXACTION_StartGame,
   VPXACTION_Lockbar,
   VPXACTION_Pause,
   VPXACTION_PerfOverlay,
   VPXACTION_OpenInGameUI,
   VPXACTION_ExitGame,
   VPXACTION_InGameUI,
   VPXACTION_VolumeDown,
   VPXACTION_VolumeUp,
   VPXACTION_VRRecenter,
   VPXACTION_VRUp,
   VPXACTION_VRDown,
} VPXAction;

typedef struct VPXActionEvent
{
   VPXAction action;
   int isPressed;
   int enableVPXProcessing;
} VPXActionEvent;

// Table element event IDs (matching COM dispatch IDs)
typedef enum
{
   VPXELEMENT_EVT_HIT         = 1400, // Ball hit the element
   VPXELEMENT_EVT_UNHIT       = 1401, // Ball left the element
   VPXELEMENT_EVT_LIMIT_EOS   = 1402, // End of stroke (flipper)
   VPXELEMENT_EVT_LIMIT_BOS   = 1403, // Beginning of stroke (flipper)
   VPXELEMENT_EVT_ANIMATE     = 1404, // Animation frame
   VPXELEMENT_EVT_SLINGSHOT   = 1101, // Slingshot fired
   VPXELEMENT_EVT_FLIPPER_COLLIDE = 1200, // Flipper collision
   VPXELEMENT_EVT_SPIN        = 1301, // Spinner rotation
   VPXELEMENT_EVT_DROPPED     = 1302, // Drop target fell
   VPXELEMENT_EVT_RAISED      = 1303, // Drop target raised
   VPXELEMENT_EVT_TIMER       = 1300, // Timer event (high frequency, usually filtered)
} VPXGameElementEventId;

typedef struct VPXGameElementEvent
{
   const char* elementName;          // Element name (e.g. "Bumper001", "LeftFlipper")
   VPXGameElementEventId eventId;    // Event type
   int elementType;                  // ItemTypeEnum value identifying the element kind
} VPXGameElementEvent;

// Element info for table element enumeration
typedef struct VPXGameElementInfo
{
   const char* name;                 // Element name string (e.g. "Bumper001")
   int elementType;                  // ItemTypeEnum value (eItemBumper, eItemFlipper, etc.)
} VPXGameElementInfo;

// Message struct for VPXPI_MSG_GET_GAME_ELEMENTS broadcast
typedef struct VPXGetGameElementsMsg
{
   unsigned int maxEntryCount;       // Capacity of entries array (0 = count query only)
   unsigned int count;               // OUT: number of interactive elements
   VPXGameElementInfo* entries;      // OUT: filled by VPX (caller allocates)
} VPXGetGameElementsMsg;

typedef struct VPXInputState
{
   uint64_t actionMask;
   uint64_t actionState;
   uint16_t stateMask; // Bit 0 for plunger position, bit 1 for plunger velocity, bit 2 for nudge (both acceleration & displacement)
   float plungerPosition;
   float plungerVelocity;
   float nudgeAccelerationX;
   float nudgeAccelerationY;
   float nudgeDisplacementX;
   float nudgeDisplacementY;
} VPXInputState;

typedef struct VPXPluginAPI
{
   // General information API
   void (MSGPIAPI *GetVpxInfo)(VPXInfo* info);
   void (MSGPIAPI *GetTableInfo)(VPXTableInfo* info);

   // User Interface
   unsigned int (MSGPIAPI *PushNotification)(const char* msg, const int lengthMs);
   void (MSGPIAPI *UpdateNotification)(const unsigned int handle, const char* msg, const int lengthMs);

   // View management
   void (MSGPIAPI *DisableStaticPrerendering)(const int /* bool */ disable);
   void (MSGPIAPI *GetActiveViewSetup)(VPXViewSetupDef* view);
   void (MSGPIAPI *SetActiveViewSetup)(VPXViewSetupDef* view);

   // Input management
   void(MSGPIAPI* GetInputState)(VPXInputState* state);
   void(MSGPIAPI* SetInputState)(VPXInputState* state);

   // Game state
   double(MSGPIAPI* GetGameTime)(); // Game time in seconds

   // Rendering
   
   // Create a texture from encoded data (Webp, Exr, ...).
   // Texture must be destroyed by the caller using DeleteTexture.
   // Thread safe
   VPXTexture(MSGPIAPI* CreateTexture)(uint8_t* rawData, int size);
   // Create or update a texture from 'flat' data in the specified format
   // - if texture object already exists, it is either updated or destroyed/recreated. texture pointer is 
   //   updated with the new/updated texture object, this object should be provided for later updates
   //   to allow reuse of texture objects.
   // Texture must be destroyed by the caller using DeleteTexture.
   // NOT Thread safe
   void(MSGPIAPI* UpdateTexture)(VPXTexture* texture, int width, int height, VPXTextureFormat format, const void* image);
   // Give access to texture informations.
   // NOT Thread safe
   VPXTextureInfo*(MSGPIAPI* GetTextureInfo)(VPXTexture texture);
   // Destroy a texture created through this API.
   // Thread safe
   void(MSGPIAPI* DeleteTexture)(VPXTexture texture);

   // Ball management
   // Create a ball at a kicker device and kick it with the given parameters.
   // deviceName: name of a Kicker element on the table (e.g., "BallRelease")
   // angle: kick angle in degrees, speed: kick speed, inclination: kick inclination
   // Returns 0 on success, -1 if no game is active, -2 if device not found, -3 if device is not a kicker.
   // NOT Thread safe
   int(MSGPIAPI* EjectBall)(const char* deviceName, const float angle, const float speed, const float inclination);

   // Destroy the ball currently held by a kicker device.
   // deviceName: name of a Kicker element on the table (e.g., "Drain")
   // Returns the number of balls destroyed (0 or 1), -1 if no game is active, -2 if device not found, -3 if device is not a kicker.
   // NOT Thread safe
   int(MSGPIAPI* DestroyBall)(const char* deviceName);

   // Light control
   // Set the state of a Light element by name.
   // deviceName: name of a Light element on the table (e.g., "B1L1")
   // state: 0.0 = off, 1.0 = fully on, values in between for dimming
   // Returns 0 on success, -1 if no game is active, -2 if element not found, -3 if element is not a light.
   // NOT Thread safe
   int(MSGPIAPI* SetLightState)(const char* deviceName, const float state);

   // Drop target control
   // Set the dropped state of a HitTarget element by name.
   // deviceName: name of a HitTarget element on the table (e.g., "sw1")
   // isDropped: 0 = raised (reset), 1 = dropped
   // Returns 0 on success, -1 if no game is active, -2 if element not found, -3 if element is not a hit target.
   // NOT Thread safe
   int(MSGPIAPI* SetDropTargetState)(const char* deviceName, const int isDropped);

   // Flipper control
   // Set the state of a Flipper element by name.
   // deviceName: name of a Flipper element on the table (e.g., "LeftFlipper")
   // isActive: 1 = RotateToEnd (power stroke), 0 = RotateToStart (return to park)
   // Returns 0 on success, -1 if no game is active, -2 if element not found, -3 if element is not a flipper.
   // NOT Thread safe
   int(MSGPIAPI* SetFlipperState)(const char* deviceName, const int isActive);

   // Kick an existing ball held by a kicker device (kick-only, does NOT create a new ball).
   // Counterpart to EjectBall: EjectBall creates + kicks (trough/launcher use case), while
   // KickBall only kicks an already-held ball (gate->kicker flow). Calling KickBall on an
   // empty kicker returns -4 instead of silently no-op'ing.
   // deviceName: name of a Kicker element on the table (e.g., "Kicker1")
   // angle: kick angle in degrees, speed: kick speed, inclination: kick inclination
   // Returns 0 on success, -1 if no game is active, -2 if device not found, -3 if device is not a kicker, -4 if the kicker has no ball held.
   // NOT Thread safe
   int(MSGPIAPI* KickBall)(const char* deviceName, const float angle, const float speed, const float inclination);

} VPXPluginAPI;
