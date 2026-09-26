// Vendored for TFCam 0.7.7 from github.com/staalo18/FreeCameraFramework include/FCFW_API.h
// at commit 8b4df64 (2026-03-23, repo head; the repo has no tags). MIT licence, (c) staalo18.
// Unmodified below this block.
//
// The installed FCFW (Nexus 174046, MO2 meta 1.1.0, DLL built 2026-09-05, logs itself as v1-0-0-0 /
// plugin version 10000) is newer than that commit, so the vtable was checked against the DLL itself:
// RequestPluginAPI returns the static Messaging::FCFWInterface at .data+0x1011C8, whose ctor stores
// vftable .rdata+0xAFF68. That vftable has 44 interface slots + 1 derived-class destructor (slot 44).
// slot 0 = `mov eax,[rcx+8]; ret` (GetFCFWThreadId), slot 1 = `mov eax,0x2710; ret`
// (GetFCFWPluginVersion = 10000), slot 33 = `call TimelineManager::GetSingleton; mov rax,[rax+0xD8]; ret`
// (GetActiveTimelineID). Same order as the IVFCFW1 declaration below, which also matches the type
// records in the FreeCameraFramework.pdb shipped with the mod (GetActiveTimelineID at vtable offset 264).
// TFCam only calls GetFCFWPluginVersion and GetActiveTimelineID.

#pragma once
#include <functional>
#include <stdint.h>

/*
* For modders: Copy this file into your own project if you wish to use this API
*/
namespace FCFW_API {
	constexpr const auto FCFWPluginName = "FreeCameraFramework";

	// Interpolation mode for camera movement between points
	enum class InterpolationMode : int {
		kNone = 0,          // No interpolation (instant jump)
		kLinear = 1,        // Linear interpolation
		kCubicHermite = 2   // Smooth cubic Hermite interpolation (default)
	};

	// Body part selection for actor reference points
	enum class BodyPart : int {
		kNone = 0,   // Use actor root position
		kHead = 1,   // Head position
		kTorso = 2   // Torso position
	};

	// Playback mode for timeline behavior at completion
	enum class PlaybackMode : int {
		kEnd = 0,   // Stop at end of timeline (default)
		kLoop = 1,  // Restart from beginning when timeline completes
		kWait = 2   // Stay at final position indefinitely (requires manual StopPlayback)
	};

	// SKSE Messaging Interface - Timeline Event Types
	// Consumers can register a listener with SKSE::GetMessagingInterface()->RegisterListener()
	// and receive these messages from FCFW
	enum class FCFWMessage : uint32_t {
		// Dispatched when timeline playback starts
		// Data: FCFWTimelineEventData*
		kPlaybackStart = 0,
		
		// Dispatched when timeline playback stops (manual stop or kEnd mode completion)
		// Data: FCFWTimelineEventData*
		kPlaybackStop = 1,
		
		// Dispatched when timeline reaches end in kWait mode (stays at final position)
		// Data: FCFWTimelineEventData*
		kPlaybackWait = 2
	};

	// Event data structure for timeline events
	// Cast the 'data' parameter in your message handler to this type
	struct FCFWTimelineEventData {
		size_t timelineID;  // ID of the timeline that triggered the event
	};

	// Available FCFW interface versions
	enum class InterfaceVersion : uint8_t {
		V1
	};

	// FCFW's modder interface
	class IVFCFW1 {
	public:
		/// <summary>
		/// Get the thread ID FCFW is running in.
		/// You may compare this with the result of GetCurrentThreadId() to help determine
		/// if you are using the correct thread.
		/// </summary>
		/// <returns>TID</returns>
		[[nodiscard]] virtual unsigned long GetFCFWThreadId() const noexcept = 0;

		/// <summary>
		/// Get the FCFW plugin version as an integer.
		/// Encoded as: major * 10000 + minor * 100 + patch
		/// Example: version 1.2.3 returns 10203
		/// </summary>
		/// <returns>Version encoded as integer</returns>
		[[nodiscard]] virtual int GetFCFWPluginVersion() const noexcept = 0;

		/// <summary>
		/// Register your plugin with FCFW (required before RegisterTimeline).
		/// This function should be called during plugin initialization.
		/// If your plugin was already registered, this will clean up any orphaned timelines
		/// from previous game sessions (eg when loading a savegame while in-game)
		/// and prepare for fresh timeline registration.
		/// </summary>
		/// <param name="a_pluginHandle">Plugin handle of the calling plugin (use SKSE::GetPluginHandle())</param>
		/// <returns>true on success, false on failure</returns>
		[[nodiscard]] virtual bool RegisterPlugin(SKSE::PluginHandle a_pluginHandle) const noexcept = 0;

		/// <summary>
		/// Register a new timeline and get its unique ID.
		/// Each plugin can register multiple timelines for independent camera paths.
		/// 
		/// IMPORTANT: You must call RegisterPlugin() first before calling this function.
		/// IMPORTANT: Timeline IDs are permanent once registered. To update a timeline's
		/// content, use ClearTimeline() followed by Add...Point() calls. Only unregister
		/// when you no longer need the timeline at all (e.g., plugin shutdown).
		/// </summary>
		/// <param name="a_pluginHandle">Plugin handle of the calling plugin (use SKSE::GetPluginHandle())</param>
		/// <returns>New timeline ID (>0) on success, or 0 on failure</returns>
		[[nodiscard]] virtual size_t RegisterTimeline(SKSE::PluginHandle a_pluginHandle) const noexcept = 0;

		/// <summary>
		/// Unregister a timeline and free its resources.
		/// This will stop any active playback/recording on the timeline before removing it.
		/// </summary>
		/// <param name="a_pluginHandle">Plugin handle for ownership validation</param>
		/// <param name="a_timelineID">Timeline ID to unregister</param>
		/// <returns>true if successfully unregistered, false on failure</returns>
		[[nodiscard]] virtual bool UnregisterTimeline(SKSE::PluginHandle a_pluginHandle, size_t a_timelineID) const noexcept = 0;

		/// <summary>
		/// Add a translation point at a specified position.
		/// </summary>
		/// <param name="a_pluginHandle">Plugin handle of the calling plugin (use SKSE::GetPluginHandle())</param>
		/// <param name="a_timelineID">Timeline ID to add the point to</param>
		/// <param name="a_time">Time in seconds when this point occurs</param>
		/// <param name="a_position">3D position coordinates (RE::NiPoint3)</param>
		/// <param name="a_easeIn">Apply ease-in at the start of interpolation (default: false)</param>
		/// <param name="a_easeOut">Apply ease-out at the end of interpolation (default: false)</param>
		/// <param name="a_interpolationMode">Interpolation mode (default: kCubicHermite)</param>
		/// <returns>Index of the added point, or -1 on failure</returns>
		/// <remarks>NOTE: Both easeIn and easeOut control the INCOMING segment (previous→current point), not the outgoing segment.
		/// For smooth transition through a point, set easeOut=false for the current point AND easeIn=false for the next point.</remarks>
		[[nodiscard]] virtual int AddTranslationPoint(SKSE::PluginHandle a_pluginHandle, size_t a_timelineID, float a_time, const RE::NiPoint3& a_position, bool a_easeIn = false, bool a_easeOut = false, InterpolationMode a_interpolationMode = InterpolationMode::kCubicHermite) const noexcept = 0;

		/// <summary>
		/// Add a translation point to the camera timeline relative to a reference object.
		/// The point will track the reference's position plus the offset.
		/// If a_isOffsetRelative is true, the offset is relative to the reference's heading, else world space.
		/// </summary>
		/// <param name="a_pluginHandle">Plugin handle of the calling plugin (use SKSE::GetPluginHandle())</param>
		/// <param name="a_timelineID">Timeline ID to add the point to</param>
		/// <param name="a_time">Time in seconds when this point occurs</param>
		/// <param name="a_reference">The object reference to track</param>
		/// <param name="a_offset">3D offset from reference position (RE::NiPoint3, default: (0,0,0))</param>
		/// <param name="a_isOffsetRelative">If true, offset is relative to reference's heading (local space), otherwise world space (default: false)</param>
		/// <param name="a_easeIn">Apply ease-in at the start of interpolation (default: false)</param>
		/// <param name="a_easeOut">Apply ease-out at the end of interpolation (default: false)</param>
		/// <param name="a_interpolationMode">Interpolation mode: 0=None, 1=Linear, 2=CubicHermite (default)</param>
		/// <returns>Index of the added point, or -1 on failure</returns>
		/// <remarks>NOTE: Both easeIn and easeOut control the INCOMING segment (previous→current point), not the outgoing segment.
		/// For smooth transition through a point, set easeOut=false for the current point AND easeIn=false for the next point.</remarks>
		/// <param name="a_bodyPart">Body part to track for actors (kNone=root, kHead=head, kTorso=torso). Non-actors always use root position. Each part has fallback logic if unavailable for the actor's race.</param>
		[[nodiscard]] virtual int AddTranslationPointAtRef(SKSE::PluginHandle a_pluginHandle, size_t a_timelineID, float a_time, RE::TESObjectREFR* a_reference, BodyPart a_bodyPart = BodyPart::kNone, const RE::NiPoint3& a_offset = RE::NiPoint3(), bool a_isOffsetRelative = false, bool a_easeIn = false, bool a_easeOut = false, InterpolationMode a_interpolationMode = InterpolationMode::kCubicHermite) const noexcept = 0;

		/// <summary>
		/// Add a translation point that captures camera position at the start of playback.
		/// This point can be used to start playback smoothly from the last camera position, and return to it later.
		/// </summary>
		/// <param name="a_pluginHandle">Plugin handle of the calling plugin (use SKSE::GetPluginHandle())</param>
		/// <param name="a_timelineID">Timeline ID to add the point to</param>
		/// <param name="a_time">Time in seconds when this point occurs</param>
		/// <param name="a_easeIn">Apply ease-in at the start of interpolation (default: false)</param>
		/// <param name="a_easeOut">Apply ease-out at the end of interpolation (default: false)</param>
		/// <param name="a_interpolationMode">Interpolation mode (default: kCubicHermite)</param>
		/// <returns> Index of the added point, or -1 on failure</returns>
		/// <remarks>NOTE: Both easeIn and easeOut control the INCOMING segment (previous→current point), not the outgoing segment.
		/// For smooth transition through a point, set easeOut=false for the current point AND easeIn=false for the next point.</remarks>
		[[nodiscard]] virtual int AddTranslationPointAtCamera(SKSE::PluginHandle a_pluginHandle, size_t a_timelineID, float a_time, bool a_easeIn = false, bool a_easeOut = false, InterpolationMode a_interpolationMode = InterpolationMode::kCubicHermite) const noexcept = 0;

        /// <summary>
        /// Add a rotation point to the camera timeline with specified pitch and yaw.
        /// </summary>
        /// <param name="a_pluginHandle">Plugin handle of the calling plugin (use SKSE::GetPluginHandle())</param>
        /// <param name="a_timelineID">Timeline ID to add the point to</param>
        /// <param name="a_time">Time in seconds when this point occurs</param>
		/// <param name="a_rotation">Rotation in radians as RE::NiPoint3 (x=pitch, y=roll, z=yaw).
		/// <param name="a_easeIn">Apply ease-in at the start of interpolation (default: false)</param>
        /// <param name="a_easeOut">Apply ease-out at the end of interpolation (default: false)</param>
        /// <param name="a_interpolationMode">Interpolation mode (default: kCubicHermite)</param>
        /// <returns>Index of the added point, or -1 on failure</returns>
        /// <remarks>NOTE: Both easeIn and easeOut control the INCOMING segment (previous→current point), not the outgoing segment.
        /// For smooth transition through a point, set easeOut=false for the current point AND easeIn=false for the next point.</remarks>
        [[nodiscard]] virtual int AddRotationPoint(SKSE::PluginHandle a_pluginHandle, size_t a_timelineID, float a_time, const RE::NiPoint3& a_rotation, bool a_easeIn = false, bool a_easeOut = false, InterpolationMode a_interpolationMode = InterpolationMode::kCubicHermite) const noexcept = 0;
		
		/// <summary>
		/// Add a rotation point that sets the rotation relative to camera-to-reference direction, or alternatively the ref's heading
		/// </summary>
		/// <param name="a_pluginHandle">Plugin handle of the calling plugin (use SKSE::GetPluginHandle())</param>
		/// <param name="a_timelineID">Timeline ID to add the point to</param>
		/// <param name="a_time">Time in seconds when this point occurs</param>
		/// <param name="a_reference">The object reference to track</param>
		/// <param name="a_offset">Rotation offset as RE::NiPoint3 (x=pitch, y=roll, z=yaw in radians). Meaning depends on a_isOffsetRelative:
		///            a_isOffsetRelative == false - offset from camera-to-reference direction (0,0,0 means looking directly at reference)
		///            a_isOffsetRelative == true - offset from reference's heading (0,0,0 means looking in direction ref is facing)</param>
		/// <param name="a_bodyPart">Body part to track rotation from (default: kNone = root rotation)</param>
		/// <param name="a_isOffsetRelative">If true, offset is relative to reference's heading instead of camera-to-reference direction (default: false)</param>
		/// <param name="a_easeIn">Ease in at the start of interpolation (default: false)</param>
		/// <param name="a_easeOut">Ease out at the end of interpolation (default: false)</param>
		/// <param name="a_interpolationMode">Interpolation mode (default: kCubicHermite)</param>
		/// <returns> Index of the added point on success, -1 on failure</returns>
		/// <remarks>NOTE: Both easeIn and easeOut control the INCOMING segment (previous→current point), not the outgoing segment.
		/// For smooth transition through a point, set easeOut=false for the current point AND easeIn=false for the next point.</remarks>
		[[nodiscard]] virtual int AddRotationPointAtRef(SKSE::PluginHandle a_pluginHandle, size_t a_timelineID, float a_time, RE::TESObjectREFR* a_reference, BodyPart a_bodyPart = BodyPart::kNone, const RE::NiPoint3& a_offset = RE::NiPoint3(), bool a_isOffsetRelative = false, bool a_easeIn = false, bool a_easeOut = false, InterpolationMode a_interpolationMode = InterpolationMode::kCubicHermite) const noexcept = 0;        /// <summary>

		/// Add a rotation point that captures camera rotation at the start of playback.
        /// This point can be used to start playback smoothly from the last camera rotation, and return to it later.
        /// </summary>
        /// <param name="a_pluginHandle">Plugin handle of the calling plugin (use SKSE::GetPluginHandle())</param>
        /// <param name="a_timelineID">Timeline ID to add the point to</param>
        /// <param name="a_time">Time in seconds when this point occurs</param>
        /// <param name="a_easeIn">Apply ease-in at the start of interpolation (default: false)</param>
        /// <param name="a_easeOut">Apply ease-out at the end of interpolation (default: false)</param>
        /// <param name="a_interpolationMode">Interpolation mode (default: kCubicHermite)</param>
		/// <returns>Index of the added point, or -1 on failure</returns>
		/// <remarks>NOTE: Both easeIn and easeOut control the INCOMING segment (previous→current point), not the outgoing segment.
		/// For smooth transition through a point, set easeOut=false for the current point AND easeIn=false for the next point.</remarks>
		[[nodiscard]] virtual int AddRotationPointAtCamera(SKSE::PluginHandle a_pluginHandle, size_t a_timelineID, float a_time, bool a_easeIn = false, bool a_easeOut = false, InterpolationMode a_interpolationMode = InterpolationMode::kCubicHermite) const noexcept = 0;

		/// <summary>
		/// Add a FOV (Field of View) point to the timeline.
		/// FOV controls camera zoom level in degrees (1-160, default 80).
		/// </summary>
		/// <param name="a_pluginHandle">Plugin handle of the calling plugin (use SKSE::GetPluginHandle())</param>
		/// <param name="a_timelineID">Timeline ID to add the point to</param>
		/// <param name="a_time">Time in seconds when this point occurs</param>
		/// <param name="a_fov">Field of view in degrees (1-160)</param>
		/// <param name="a_easeIn">Apply ease-in at the start of interpolation (default: false)</param>
		/// <param name="a_easeOut">Apply ease-out at the end of interpolation (default: false)</param>
		/// <param name="a_interpolationMode">Interpolation mode (default: kCubicHermite)</param>
		/// <returns>Index of the added point, or -1 on failure</returns>
		/// <remarks>NOTE: Both easeIn and easeOut control the INCOMING segment (previous→current point), not the outgoing segment.
		/// For smooth transition through a point, set easeOut=false for the current point AND easeIn=false for the next point.</remarks>
		[[nodiscard]] virtual int AddFOVPoint(SKSE::PluginHandle a_pluginHandle, size_t a_timelineID, float a_time, float a_fov, bool a_easeIn = false, bool a_easeOut = false, InterpolationMode a_interpolationMode = InterpolationMode::kCubicHermite) const noexcept = 0;

		/// <summary>
		/// Remove a translation point from a timeline
		/// </summary>
		/// <param name="a_pluginHandle">Plugin handle of the calling plugin (use SKSE::GetPluginHandle())</param>
		/// <param name="a_timelineID">Timeline ID to remove the point from</param>
		/// <param name="a_index">Index of the point to remove</param>
		/// <returns>true if the point was removed, false on failure</returns>
		[[nodiscard]] virtual bool RemoveTranslationPoint(SKSE::PluginHandle a_pluginHandle, size_t a_timelineID, size_t a_index) const noexcept = 0;

		/// <summary>
		/// Remove a rotation point from a timeline
		/// </summary>
		/// <param name="a_pluginHandle">Plugin handle of the calling plugin (use SKSE::GetPluginHandle())</param>
		/// <param name="a_timelineID">Timeline ID to remove the point from</param>
		/// <param name="a_index">Index of the point to remove</param>
		/// <returns>true if the point was removed, false on failure</returns>
		[[nodiscard]] virtual bool RemoveRotationPoint(SKSE::PluginHandle a_pluginHandle, size_t a_timelineID, size_t a_index) const noexcept = 0;

		/// <summary>
		/// Remove a FOV point from a timeline
		/// </summary>
		/// <param name="a_pluginHandle">Plugin handle of the calling plugin (use SKSE::GetPluginHandle())</param>
		/// <param name="a_timelineID">Timeline ID to remove the point from</param>
		/// <param name="a_index">Index of the point to remove</param>
		/// <returns>true if the point was removed, false on failure</returns>
		[[nodiscard]] virtual bool RemoveFOVPoint(SKSE::PluginHandle a_pluginHandle, size_t a_timelineID, size_t a_index) const noexcept = 0;

        /// <summary>
        /// Start recording camera movement to a timeline.
        /// </summary>
        /// <param name="a_pluginHandle">Plugin handle of the calling plugin (use SKSE::GetPluginHandle())</param>
        /// <param name="a_timelineID">Timeline ID to record to</param>
        /// <param name="a_recordingInterval">Time between samples in seconds. 0.0 = capture every frame (default: 1.0)</param>
        /// <param name="a_append">If true, append to existing timeline; if false, clear timeline first (default: false)</param>
        /// <param name="a_timeOffset">Time offset in seconds added after the last existing point when appending (default: 0.0)</param>
        /// <returns>True on success, false on failure</returns>
        /// <remarks>When a_recordingInterval = 0.0 or negative: captures every frame (frame-rate dependent).
        /// When a_append=true: First recorded point placed at (lastPointTime + a_timeOffset). Uses easeIn=false.
        /// If timeline is empty when appending, starts at a_timeOffset with easeIn=false.
        /// When a_append=false: Clears timeline and starts at time 0.0 with easeIn=true.</remarks>
        [[nodiscard]] virtual bool StartRecording(SKSE::PluginHandle a_pluginHandle, size_t a_timelineID, float a_recordingInterval = 1.0f, bool a_append = false, float a_timeOffset = 0.0f) const noexcept = 0;

		/// <summary>
		/// Stop recording camera movements on a timeline.
		/// Adds a final point with ease-out at the current camera position.
		/// </summary>
		/// <param name="a_pluginHandle">Plugin handle of the calling plugin (use SKSE::GetPluginHandle())</param>
		/// <param name="a_timelineID">Timeline ID to stop recording on</param>
		/// <returns>true on success, false on failure</returns>
		[[nodiscard]] virtual bool StopRecording(SKSE::PluginHandle a_pluginHandle, size_t a_timelineID) const noexcept = 0;

		/// <summary>
		/// Clear the entire timeline.
		/// </summary>
		/// <param name="a_pluginHandle">Plugin handle of the calling plugin (use SKSE::GetPluginHandle())</param>
		/// <param name="a_timelineID">Timeline ID to clear</param>
		/// <returns>true if cleared successfully, false on failure</returns>
		[[nodiscard]] virtual bool ClearTimeline(SKSE::PluginHandle a_pluginHandle, size_t a_timelineID) const noexcept = 0;

        /// <summary>
        /// Get the number of translation points in the timeline.
        /// </summary>
        /// <param name="a_pluginHandle">Plugin handle for ownership validation</param>
        /// <param name="a_timelineID">Timeline ID to query</param>
        /// <returns>Number of translation points, or -1 if timeline not found or not owned</returns>
        [[nodiscard]] virtual int GetTranslationPointCount(SKSE::PluginHandle a_pluginHandle, size_t a_timelineID) const noexcept = 0;

		/// <summary>
		/// Get the number of rotation points in the timeline.
		/// </summary>
		/// <param name="a_pluginHandle">Plugin handle for ownership validation</param>
		/// <param name="a_timelineID">Timeline ID to query</param>
		/// <returns>Number of rotation points, or -1 if timeline not found or not owned</returns>
		[[nodiscard]] virtual int GetRotationPointCount(SKSE::PluginHandle a_pluginHandle, size_t a_timelineID) const noexcept = 0;

		/// <summary>
		/// Get the position of a translation point by index.
		/// Returns the baked world position of the point (kWorld/kCamera points return absolute position,
		/// kReference points are not evaluated - use during playback to get dynamic positions).
		/// </summary>
		/// <param name="a_pluginHandle">Plugin handle for ownership validation</param>
		/// <param name="a_timelineID">Timeline ID to query</param>
		/// <param name="a_index">Index of the translation point (0-based)</param>
		/// <returns>RE::NiPoint3 with position coordinates, or (0,0,0) if timeline not found/owned or index out of range</returns>
		[[nodiscard]] virtual RE::NiPoint3 GetTranslationPoint(SKSE::PluginHandle a_pluginHandle, size_t a_timelineID, size_t a_index) const noexcept = 0;

		/// <summary>
		/// Get the rotation of a rotation point by index.
		/// Returns the rotation as pitch (x), roll (y), and yaw (z) in radians.
		/// </summary>
		/// <param name="a_pluginHandle">Plugin handle for ownership validation</param>
		/// <param name="a_timelineID">Timeline ID to query</param>
		/// <param name="a_index">Index of the rotation point (0-based)</param>
		/// <returns>RE::NiPoint3 with rotation (x=pitch, y=roll, z=yaw in radians), or (0,0,0) if timeline not found/owned or index out of range</returns>
		[[nodiscard]] virtual RE::NiPoint3 GetRotationPoint(SKSE::PluginHandle a_pluginHandle, size_t a_timelineID, size_t a_index) const noexcept = 0;

		/// <summary>
		/// Get the number of FOV points in the timeline.
		/// </summary>
		/// <param name="a_pluginHandle">Plugin handle for ownership validation</param>
		/// <param name="a_timelineID">Timeline ID to query</param>
		/// <returns>Number of FOV points, or -1 if timeline not found or not owned</returns>
		[[nodiscard]] virtual int GetFOVPointCount(SKSE::PluginHandle a_pluginHandle, size_t a_timelineID) const noexcept = 0;

		/// <summary>
		/// Get the FOV value of a FOV point by index.
		/// </summary>
		/// <param name="a_pluginHandle">Plugin handle for ownership validation</param>
		/// <param name="a_timelineID">Timeline ID to query</param>
		/// <param name="a_index">Index of the FOV point (0-based)</param>
		/// <returns>FOV value in degrees, or 80.0 if timeline not found/owned or index out of range</returns>
		[[nodiscard]] virtual float GetFOVPoint(SKSE::PluginHandle a_pluginHandle, size_t a_timelineID, size_t a_index) const noexcept = 0;

		/// <summary>
		/// Start playing the camera timeline.
		/// If a_useDuration is true: 
        ///     plays complete timeline with total time a_duration seconds
		/// If a_useDuration is false:
        ///     plays timeline with a_speed as speed multiplier.
        /// Global easing (a_globalEaseIn/Out) applies to overall playback in both modes.
		/// NOTE: To control ground following, menu visibility, and user rotation, use the separate SetFollowGround(),
		///       SetMenuVisibility(), and AllowUserRotation() functions before or during playback.
		/// </summary>
		/// <param name="a_pluginHandle">Plugin handle for ownership validation</param>
		/// <param name="a_timelineID">Timeline ID to start playback on</param>
		/// <param name="a_speed">Playback speed multiplier, only used if a_useDuration is false (default: 1.0)</param>
		/// <param name="a_globalEaseIn">Apply ease-in at the start of entire playback (default: false)</param>
		/// <param name="a_globalEaseOut">Apply ease-out at the end of entire playback (default: false)</param>
		/// <param name="a_useDuration">If true, plays complete timeline with total time a_duration seconds; if false, uses a_speed multiplier (default: false)</param>
		/// <param name="a_duration">Total duration in seconds for entire timeline, only used if a_useDuration is true (default: 0.0)</param>
		/// <param name="a_startTime">Start playback at this time in seconds (default: 0.0 = start from beginning). Useful for resuming playback after save/load.</param>
		/// <returns>true on success, false on failure</returns>
		[[nodiscard]] virtual bool StartPlayback(SKSE::PluginHandle a_pluginHandle, size_t a_timelineID, float a_speed = 1.0f, bool a_globalEaseIn = false, bool a_globalEaseOut = false, bool a_useDuration = false, float a_duration = 0.0f, float a_startTime = 0.0f) const noexcept = 0;

		/// <summary>
		/// Stop playback of a camera timeline.
		/// </summary>
		/// <param name="a_pluginHandle">Plugin handle for ownership validation</param>
		/// <param name="a_timelineID">Timeline ID to stop</param>
		/// <returns>true on success, false on failure</returns>
		[[nodiscard]] virtual bool StopPlayback(SKSE::PluginHandle a_pluginHandle, size_t a_timelineID) const noexcept = 0;

		/// <summary>
		/// Switch playback from one timeline to another without exiting free camera mode.
		/// If a_fromTimelineID is 0, switches from any timeline owned by the calling plugin.
		/// Otherwise, only switches if the specified source timeline is actively playing.
		/// </summary>
		/// <param name="a_pluginHandle">Plugin handle for ownership validation</param>
		/// <param name="a_fromTimelineID">Source timeline ID (0 = any owned timeline currently playing)</param>
		/// <param name="a_toTimelineID">Target timeline ID to switch to</param>
		/// <returns>true on successful switch, false on failure</returns>
		[[nodiscard]] virtual bool SwitchPlayback(SKSE::PluginHandle a_pluginHandle, size_t a_fromTimelineID, size_t a_toTimelineID) const noexcept = 0;

		/// <summary>
		/// Pause playback of a camera path timeline.
		/// </summary>
		/// <param name="a_pluginHandle">Plugin handle for ownership validation</param>
		/// <param name="a_timelineID">Timeline ID to pause</param>
		/// <returns>true on success, false on failure</returns>
		[[nodiscard]] virtual bool PausePlayback(SKSE::PluginHandle a_pluginHandle, size_t a_timelineID) const noexcept = 0;

		/// <summary>
		/// Resume playback of a camera path timeline.
		/// </summary>
		/// <param name="a_pluginHandle">Plugin handle for ownership validation</param>
		/// <param name="a_timelineID">Timeline ID to resume</param>
		/// <returns>true on success, false on failure</returns>
		[[nodiscard]] virtual bool ResumePlayback(SKSE::PluginHandle a_pluginHandle, size_t a_timelineID) const noexcept = 0;

		/// <summary>
		/// Check if a timeline is currently playing back.
		/// </summary>
		/// <param name="a_pluginHandle">Plugin handle for ownership validation</param>
		/// <param name="a_timelineID">Timeline ID to check</param>
		/// <returns>true if playing, false otherwise or if not owned</returns>
		[[nodiscard]] virtual bool IsPlaybackRunning(SKSE::PluginHandle a_pluginHandle, size_t a_timelineID) const noexcept = 0;

		/// <summary>
		/// Check if a timeline is currently recording.
		/// </summary>
		/// <param name="a_pluginHandle">Plugin handle for ownership validation</param>
		/// <param name="a_timelineID">Timeline ID to check</param>
		/// <returns>true if recording, false otherwise or if not owned</returns>
		[[nodiscard]] virtual bool IsRecording(SKSE::PluginHandle a_pluginHandle, size_t a_timelineID) const noexcept = 0;

		/// <summary>
		/// Check if timeline playback is currently paused.
		/// </summary>
		/// <param name="a_pluginHandle">Plugin handle for ownership validation</param>
		/// <param name="a_timelineID">Timeline ID to check</param>
		/// <returns>True if paused, false otherwise or if not owned</returns>
		[[nodiscard]] virtual bool IsPlaybackPaused(SKSE::PluginHandle a_pluginHandle, size_t a_timelineID) const noexcept = 0;

		/// <summary>
		/// Get the current playback time for a timeline.
		/// Useful for saving playback state to resume after loading a save.
		/// </summary>
		/// <param name="a_pluginHandle">Plugin handle for ownership validation</param>
		/// <param name="a_timelineID">Timeline ID to query</param>
		/// <returns>Current playback time in seconds, or -1.0f if timeline not found or not owned</returns>
		[[nodiscard]] virtual float GetPlaybackTime(SKSE::PluginHandle a_pluginHandle, size_t a_timelineID) const noexcept = 0;

		/// <summary>
		/// Get the ID of the currently active timeline (recording or playing).
		/// </summary>
		/// <returns>Timeline ID if active (>0), or 0 if no timeline is active</returns>
		[[nodiscard]] virtual size_t GetActiveTimelineID() const noexcept = 0;

		/// <summary>
		/// Enable or disable user rotation control during playback for a specific timeline.
		/// Can be called before or during playback to change behavior dynamically.
		/// </summary>
		/// <param name="a_pluginHandle">Plugin handle for ownership validation</param>
		/// <param name="a_timelineID">Timeline ID to configure</param>
		/// <param name="a_allow">True to allow user to control rotation, false to disable</param>
		virtual void AllowUserRotation(SKSE::PluginHandle a_pluginHandle, size_t a_timelineID, bool a_allow) const noexcept = 0;

		/// <summary>
		/// Check if user rotation is currently allowed for a specific timeline.
		/// </summary>
		/// <param name="a_pluginHandle">Plugin handle for ownership validation</param>
		/// <param name="a_timelineID">Timeline ID to check</param>
		/// <returns>True if user can control rotation, false otherwise or if not owned</returns>
		[[nodiscard]] virtual bool IsUserRotationAllowed(SKSE::PluginHandle a_pluginHandle, size_t a_timelineID) const noexcept = 0;

		/// <summary>
		/// Enable or disable ground following and set minimum height above ground.
		/// Can be called before or during playback to change behavior dynamically.
		/// When enabled, camera will stay above ground/water level during playback.
		/// </summary>
		/// <param name="a_pluginHandle">Plugin handle for ownership validation</param>
		/// <param name="a_timelineID">Timeline ID to configure</param>
		/// <param name="a_follow">True to enable ground following, false to disable</param>
		/// <param name="a_minHeight">Minimum height above ground when following ground (default: 0.0)</param>
		/// <returns>True if successfully set, false on failure</returns>
		[[nodiscard]] virtual bool SetFollowGround(SKSE::PluginHandle a_pluginHandle, size_t a_timelineID, bool a_follow, float a_minHeight = 0.0f) const noexcept = 0;

		/// <summary>
		/// Check if ground following is currently enabled for a specific timeline.
		/// </summary>
		/// <param name="a_pluginHandle">Plugin handle for ownership validation</param>
		/// <param name="a_timelineID">Timeline ID to check</param>
		/// <returns>True if ground following is enabled, false otherwise or if not owned</returns>
		[[nodiscard]] virtual bool IsGroundFollowingEnabled(SKSE::PluginHandle a_pluginHandle, size_t a_timelineID) const noexcept = 0;

		/// <summary>
		/// Get the minimum height above ground for a specific timeline.
		/// </summary>
		/// <param name="a_pluginHandle">Plugin handle for ownership validation</param>
		/// <param name="a_timelineID">Timeline ID to query</param>
		/// <returns>Minimum height in game units, or -1.0f if timeline not found or not owned</returns>
		[[nodiscard]] virtual float GetMinHeightAboveGround(SKSE::PluginHandle a_pluginHandle, size_t a_timelineID) const noexcept = 0;

		/// <summary>
		/// Set menu visibility during playback for a specific timeline.
		/// Can be called before or during playback to change behavior dynamically.
		/// </summary>
		/// <param name="a_pluginHandle">Plugin handle for ownership validation</param>
		/// <param name="a_timelineID">Timeline ID to configure</param>
		/// <param name="a_show">True to show menus during playback, false to hide</param>
		/// <returns>True if successfully set, false on failure</returns>
		[[nodiscard]] virtual bool SetMenuVisibility(SKSE::PluginHandle a_pluginHandle, size_t a_timelineID, bool a_show) const noexcept = 0;

		/// <summary>
		/// Check if menus are currently visible for a specific timeline.
		/// </summary>
		/// <param name="a_pluginHandle">Plugin handle for ownership validation</param>
		/// <param name="a_timelineID">Timeline ID to check</param>
		/// <returns>True if menus are visible, false otherwise or if not owned</returns>
		[[nodiscard]] virtual bool AreMenusVisible(SKSE::PluginHandle a_pluginHandle, size_t a_timelineID) const noexcept = 0;

		// ===== TIMELINE PROPERTIES (set during timeline definition) =====

		/// <summary>
		/// Set the playback mode and loop time offset for a timeline.
		/// This defines the timeline's intrinsic end-of-playback behavior.
		/// Should be called during timeline definition, before StartPlayback().
		/// </summary>
		/// <param name="a_pluginHandle">Plugin handle for ownership validation</param>
		/// <param name="a_timelineID">Timeline ID to configure</param>
		/// <param name="a_playbackMode">Playback mode (PlaybackMode::kEnd, kLoop, or kWait)</param>
		/// <param name="a_loopTimeOffset">Time offset in seconds when looping back (only used in kLoop mode, default: 0.0)</param>
		/// <returns>True if successfully set, false on failure</returns>
		[[nodiscard]] virtual bool SetPlaybackMode(SKSE::PluginHandle a_pluginHandle, size_t a_timelineID, FCFW_API::PlaybackMode a_playbackMode, float a_loopTimeOffset = 0.0f) const noexcept = 0;

		/// <summary>
		/// Adds camera timeline imported from a_filePath at time a_timeOffset to the specified timeline.
		/// </summary>
		/// <param name="a_pluginHandle">Plugin handle for ownership validation</param>
		/// <param name="a_timelineID">ID of the timeline to add points to</param>
		/// <param name="a_filePath">Relative path from Data folder (e.g., "SKSE/Plugins/MyTimeline.dat")</param>
		/// <param name="a_timeOffset">Time offset in seconds to add to all imported point times (default: 0.0)</param>
		/// <returns> True if successful, false otherwise</returns>
		[[nodiscard]] virtual bool AddTimelineFromFile(SKSE::PluginHandle a_pluginHandle, size_t a_timelineID, const char* a_filePath, float a_timeOffset = 0.0f) const noexcept = 0;

		/// <summary>
		/// Export camera timeline to a file.
		/// </summary>
		/// <param name="a_pluginHandle">Plugin handle for ownership validation</param>
		/// <param name="a_timelineID">ID of the timeline to export</param>
		/// <param name="a_filePath">Relative path from Data folder (e.g., "SKSE/Plugins/MyTimeline.dat")</param>
		/// <returns> True if successful, false otherwise</returns>
		[[nodiscard]] virtual bool ExportTimeline(SKSE::PluginHandle a_pluginHandle, size_t a_timelineID, const char* a_filePath) const noexcept = 0;
	};

	typedef void* (*_RequestPluginAPI)(const InterfaceVersion interfaceVersion);

	/// <summary>
	/// Request the FCFW API interface.
	/// Recommended: Send your request during or after SKSEMessagingInterface::kMessage_PostLoad to make sure the dll has already been loaded
	/// </summary>
	/// <param name="a_interfaceVersion">The interface version to request</param>
	/// <returns>The pointer to the API singleton, or nullptr if request failed</returns>
	[[nodiscard]] inline void* RequestPluginAPI(const InterfaceVersion a_interfaceVersion = InterfaceVersion::V1) {
		auto pluginHandle = GetModuleHandleA("FreeCameraFramework.dll");
		_RequestPluginAPI requestAPIFunction = (_RequestPluginAPI)GetProcAddress(pluginHandle, "RequestPluginAPI");
		if (requestAPIFunction) {
			return requestAPIFunction(a_interfaceVersion);
		}
		return nullptr;
	}
}
