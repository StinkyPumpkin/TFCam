#pragma once

#include <cstdint>
#include <string>

// 0.10.0: saved Scene Camera shots, one per SexLab scene stage (+ the player's position slot) or per pose.
//
// File: Data/SKSE/Plugins/TFCam/SceneShots.tsv. TFCam creates it on the first save (MO2 puts it in overwrite);
// it is never shipped. One shot per line, tab-separated:
//   key  x  y  z  yawRel  pitch  roll  fov  savedAt  label
// key = "stage:<sceneId>|<stageId>|<playerSlot>" or "pose:<animEvent>" (lower case). x / y / z = the camera
// position in the anchor's frame (x right, y forward, z up; anchor = the player for a stage shot, the posed actor
// for a pose shot), yawRel = camera yaw minus the anchor's heading; angles in degrees. Lines starting with '#' are
// comments; lines TFCam cannot read (or with a value that is not a finite number) are kept as they are when it
// rewrites the file, and never applied.
//
// Main thread only. Loaded on first use; every Put / Erase rewrites the file atomically (.tmp + MoveFileExW). The
// in-memory shots change only once that write succeeded, so a failed write changes nothing.
namespace SceneShots {

    struct Shot {
        float       x      = 0.0f;
        float       y      = 0.0f;
        float       z      = 0.0f;
        float       yawRel = 0.0f;  // degrees
        float       pitch  = 0.0f;  // degrees, absolute
        float       roll   = 0.0f;  // degrees
        float       fov    = 0.0f;
        std::string savedAt;        // local time, "YYYY-MM-DD HH:MM:SS"
        std::string label;          // scene name / pose event, for people reading the file
    };

    const Shot* Find(const std::string& a_key);
    bool        Has(const std::string& a_key);
    bool        Put(const std::string& a_key, const Shot& a_shot);  // false = the file could not be written
    bool        Erase(const std::string& a_key);                    // false = no such shot, or the write failed (kept)
    int         EraseWithPrefix(const std::string& a_prefix);       // shots removed; -1 = the write failed (all kept)
    int         CountWithPrefix(const std::string& a_prefix);
    std::uint32_t Version();      // bumps on every change (status caches key off it)
    std::string   FilePath();     // full path, for the log and the settings page
}
