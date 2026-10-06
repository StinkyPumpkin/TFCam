#include "SceneShots.h"

#include <Windows.h>

#include <charconv>
#include <cmath>
#include <cstdio>
#include <format>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace SceneShots {

    namespace {
        // Main thread only.
        bool                        s_loaded = false;
        std::map<std::string, Shot> s_shots;
        std::vector<std::string>    s_unknownLines;  // kept verbatim on rewrite
        std::uint32_t               s_version = 1;

        std::wstring FullPath(const wchar_t* a_rel) {
            wchar_t buf[MAX_PATH] = {};
            const DWORD len = ::GetFullPathNameW(a_rel, MAX_PATH, buf, nullptr);
            return len > 0 && len < MAX_PATH ? std::wstring(buf, len) : std::wstring(a_rel);
        }

        const std::wstring& DirW() {
            static const std::wstring dir = FullPath(L"Data\\SKSE\\Plugins\\TFCam");
            return dir;
        }

        const std::wstring& FileW() {
            static const std::wstring file = FullPath(L"Data\\SKSE\\Plugins\\TFCam\\SceneShots.tsv");
            return file;
        }

        std::string Narrow(const std::wstring& a_w) {
            if (a_w.empty()) return {};
            const int n = ::WideCharToMultiByte(CP_UTF8, 0, a_w.c_str(), static_cast<int>(a_w.size()), nullptr, 0, nullptr, nullptr);
            std::string out(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
            if (n > 0) {
                ::WideCharToMultiByte(CP_UTF8, 0, a_w.c_str(), static_cast<int>(a_w.size()), out.data(), n, nullptr, nullptr);
            }
            return out;
        }

        // Tabs and line breaks would break the line format.
        std::string Clean(std::string_view a_s) {
            std::string out;
            out.reserve(a_s.size());
            for (const char c : a_s) {
                out.push_back(c == '\t' || c == '\r' || c == '\n' ? ' ' : c);
            }
            return out;
        }

        bool ParseFloat(std::string_view a_s, float& a_out) {
            while (!a_s.empty() && a_s.front() == ' ') a_s.remove_prefix(1);
            while (!a_s.empty() && a_s.back() == ' ') a_s.remove_suffix(1);
            if (a_s.empty()) return false;
            const auto res = std::from_chars(a_s.data(), a_s.data() + a_s.size(), a_out);
            // from_chars also reads "nan" / "inf": such a line is kept as an unreadable one, never applied.
            return res.ec == std::errc{} && res.ptr == a_s.data() + a_s.size() && std::isfinite(a_out);
        }

        bool ParseLine(const std::string& a_line, std::string& a_key, Shot& a_shot) {
            std::vector<std::string_view> f;
            std::string_view              rest(a_line);
            while (true) {
                const auto tab = rest.find('\t');
                f.push_back(rest.substr(0, tab));
                if (tab == std::string_view::npos) break;
                rest.remove_prefix(tab + 1);
            }
            if (f.size() < 9) return false;
            const std::string_view key = f[0];
            if (!(key.starts_with("stage:") || key.starts_with("pose:")) || key.size() < 7) return false;
            Shot s;
            if (!ParseFloat(f[1], s.x) || !ParseFloat(f[2], s.y) || !ParseFloat(f[3], s.z) || !ParseFloat(f[4], s.yawRel) ||
                !ParseFloat(f[5], s.pitch) || !ParseFloat(f[6], s.roll) || !ParseFloat(f[7], s.fov)) {
                return false;
            }
            s.savedAt = std::string(f[8]);
            if (f.size() > 9) s.label = std::string(f[9]);
            a_key  = std::string(key);
            a_shot = std::move(s);
            return true;
        }

        void Load() {
            if (s_loaded) return;
            s_loaded = true;
            std::ifstream in(FileW(), std::ios::in | std::ios::binary);
            if (!in) {
                SKSE::log::info("SceneShots: no {} yet - it is created on the first saved shot", Narrow(FileW()));
                return;
            }
            std::string line;
            int         shots = 0;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty() || line.front() == '#') continue;  // our header / comments are rewritten
                std::string key;
                Shot        shot;
                if (ParseLine(line, key, shot)) {
                    s_shots[key] = std::move(shot);
                    ++shots;
                } else {
                    s_unknownLines.push_back(line);
                }
            }
            SKSE::log::info("SceneShots: loaded {} shot(s) from {}{}", shots, Narrow(FileW()),
                s_unknownLines.empty() ? "" : std::format(" ({} unreadable line(s) kept as they are)", s_unknownLines.size()));
        }

        std::string Fmt(float a_v) { return std::format("{:.3f}", a_v); }

        // Writes a_shots (the map as it will be once this succeeds); callers swap it in only on success, so memory never
        // holds a change the file does not.
        bool Save(const std::map<std::string, Shot>& a_shots) {
            if (!::CreateDirectoryW(DirW().c_str(), nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS) {
                SKSE::log::error("SceneShots: could not create {} (Windows error {})", Narrow(DirW()), ::GetLastError());
                return false;
            }
            std::string out;
            out += "# TFCam Scene Camera shots. TFCam writes this file; it is never part of a download.\n";
            out += "# key\tx\ty\tz\tyawRel\tpitch\troll\tfov\tsavedAt\tlabel\n";
            out += "# x/y/z = camera position in the anchor's frame (x right, y forward, z up). Anchor: the player for\n";
            out += "# stage:<scene>|<stage>|<player slot>, the posed actor for pose:<animation event>. Angles in degrees.\n";
            for (const auto& [key, s] : a_shots) {
                out += std::format("{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", Clean(key), Fmt(s.x), Fmt(s.y), Fmt(s.z),
                    Fmt(s.yawRel), Fmt(s.pitch), Fmt(s.roll), Fmt(s.fov), Clean(s.savedAt), Clean(s.label));
            }
            for (const auto& line : s_unknownLines) {
                out += line;
                out += '\n';
            }

            const std::wstring tmp = FileW() + L".tmp";
            {
                std::ofstream f(tmp, std::ios::out | std::ios::binary | std::ios::trunc);
                if (!f) {
                    SKSE::log::error("SceneShots: could not open {} for writing", Narrow(tmp));
                    return false;
                }
                f.write(out.data(), static_cast<std::streamsize>(out.size()));
                f.flush();
                if (!f) {
                    SKSE::log::error("SceneShots: writing {} failed part-way", Narrow(tmp));
                    return false;
                }
            }
            if (!::MoveFileExW(tmp.c_str(), FileW().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                SKSE::log::error("SceneShots: replacing {} failed (Windows error {})", Narrow(FileW()), ::GetLastError());
                ::DeleteFileW(tmp.c_str());
                return false;
            }
            SKSE::log::info("SceneShots: saved {} shot(s) to {}", a_shots.size(), Narrow(FileW()));
            return true;
        }

        // Commits a changed copy of the map: memory takes it only once the file holds it.
        bool Commit(std::map<std::string, Shot>&& a_next) {
            if (!Save(a_next)) return false;
            s_shots = std::move(a_next);
            ++s_version;
            return true;
        }
    }

    const Shot* Find(const std::string& a_key) {
        Load();
        const auto it = s_shots.find(a_key);
        return it == s_shots.end() ? nullptr : &it->second;
    }

    bool Has(const std::string& a_key) { return Find(a_key) != nullptr; }

    bool Put(const std::string& a_key, const Shot& a_shot) {
        Load();
        auto next          = s_shots;
        next[Clean(a_key)] = a_shot;
        return Commit(std::move(next));
    }

    bool Erase(const std::string& a_key) {
        Load();
        if (!s_shots.contains(a_key)) return false;
        auto next = s_shots;
        next.erase(a_key);
        return Commit(std::move(next));
    }

    int EraseWithPrefix(const std::string& a_prefix) {
        Load();
        auto next = s_shots;
        int  n    = 0;
        for (auto it = next.begin(); it != next.end();) {
            if (it->first.starts_with(a_prefix)) {
                it = next.erase(it);
                ++n;
            } else {
                ++it;
            }
        }
        if (n > 0 && !Commit(std::move(next))) return -1;
        return n;
    }

    int CountWithPrefix(const std::string& a_prefix) {
        Load();
        int n = 0;
        for (auto it = s_shots.lower_bound(a_prefix); it != s_shots.end() && it->first.starts_with(a_prefix); ++it) ++n;
        return n;
    }

    std::uint32_t Version() { return s_version; }

    std::string FilePath() { return Narrow(FileW()); }
}
