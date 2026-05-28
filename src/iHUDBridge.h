#pragma once

namespace iHUDBridge {
    enum Message : uint32_t {
        kHideAll    = 1,
        kRestoreAll = 2,
    };

    void Register();
    bool IsHiddenByExternal();
}
