#include "iHUDBridge.h"

#include <atomic>

namespace iHUDBridge {

    static std::atomic<bool> s_hidden{false};

    bool IsHiddenByExternal() {
        return s_hidden.load(std::memory_order_relaxed);
    }

    static void __cdecl Callback(SKSE::MessagingInterface::Message* msg) {
        if (!msg) return;
        switch (msg->type) {
        case kHideAll:
            s_hidden.store(true, std::memory_order_relaxed);
            SKSE::log::info("iHUDBridge: HideAll received");
            break;
        case kRestoreAll:
            s_hidden.store(false, std::memory_order_relaxed);
            SKSE::log::info("iHUDBridge: RestoreAll received");
            break;
        default:
            break;
        }
    }

    void Register() {
        auto* mi = SKSE::GetMessagingInterface();
        if (!mi) {
            SKSE::log::error("iHUDBridge: no messaging interface");
            return;
        }
        if (!mi->RegisterListener("iHUDClaude", Callback)) {
            SKSE::log::info("iHUDBridge: RegisterListener('iHUDClaude') failed (iHUDClaude may not be installed)");
            return;
        }
        SKSE::log::info("iHUDBridge: listening for iHUDClaude messages");
    }
}
