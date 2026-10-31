#include "CamFocus.h"

#include <format>

namespace CamFocus {

    namespace {
        // Built on first use (main thread, in game): BSFixedString needs the game's string cache.
        struct Names {
            RE::BSFixedString pelvis{ "NPC Pelvis [Pelv]" };
            RE::BSFixedString spine2{ "NPC Spine2 [Spn2]" };
            RE::BSFixedString spine1{ "NPC Spine1 [Spn1]" };
            RE::BSFixedString head{ "NPC Head [Head]" };
        };

        const Names& N() {
            static const Names names;
            return names;
        }

        RE::NiAVObject* Node(RE::NiAVObject* a_root, const RE::BSFixedString& a_name) {
            return a_root ? a_root->GetObjectByName(a_name) : nullptr;
        }

        bool Finite(const RE::NiPoint3& a_p) { return std::isfinite(a_p.x) && std::isfinite(a_p.y) && std::isfinite(a_p.z); }
    }

    RE::NiPointer<RE::TESObjectREFR> ConsoleRef() {
        auto ref = RE::Console::GetSelectedRef();
        if (!ref || ref->IsDeleted() || ref->IsDisabled() || !ref->Get3D()) return nullptr;
        return ref;
    }

    bool RefPoint(RE::TESObjectREFR* a_ref, RE::NiPoint3& a_out, float* a_floor) {
        if (!a_ref) return false;
        auto* root = a_ref->Get3D();
        if (auto* actor = a_ref->As<RE::Actor>(); actor && root) {
            auto* pelvis = Node(root, N().pelvis);
            auto* spine2 = Node(root, N().spine2);
            if (pelvis && spine2) {
                const auto& a = pelvis->world.translate;
                const auto& b = spine2->world.translate;
                a_out         = { (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f };
            } else if (auto* spine1 = Node(root, N().spine1)) {
                a_out = spine1->world.translate;
            } else {
                a_out = a_ref->GetPosition();
                a_out.z += 60.0f;
            }
        } else if (root && root->worldBound.radius > 0.0f && Finite(root->worldBound.center)) {
            a_out = root->worldBound.center;
        } else {
            a_out = a_ref->GetPosition();
        }
        if (a_floor) *a_floor = a_ref->GetPosition().z;
        return Finite(a_out);
    }

    bool HeadPoint(RE::Actor* a_actor, RE::NiPoint3& a_out) {
        auto* head = a_actor ? Node(a_actor->Get3D(), N().head) : nullptr;
        if (!head) return false;
        a_out = head->world.translate;
        return Finite(a_out);
    }

    std::string Describe(RE::TESObjectREFR* a_ref) {
        if (!a_ref) return "nothing";
        const char* name = a_ref->GetDisplayFullName();
        return std::format("{} ({:08X})", name && *name ? name : "unnamed", a_ref->GetFormID());
    }
}
