#pragma once

#include <cstdint>
#include <string>
#include <vector>

// 0.7.8: SexLab (P+) scene tracking for the SLCC camera cycle.
//
// P+ 2.17.1 sslThreadModel.SetupThreadEvent sends every thread hook twice:
//   ModEvent.Create("Hook" + name) + PushInt(tid) + PushBool(HasPlayer) + Send  -> Papyrus registrations only
//   Form.SendModEvent(name, tid as string)                                        -> also SKSE's ModCallbackEvent source
// so a native sink sees the BARE names ("AnimationStart", "AnimationEnding", "AnimationEnd") with the thread id
// in strArg and the thread quest as sender. SLCC's native path reads the same events the same way.
// The player's thread is found from the sender quest's reference aliases at AnimationStart.
//
// 0.10.0 (Scene Camera): for the player's thread TFCam also knows the scene id, the stage id, the player's
// position slot and the actors, read from the thread quest's sslThreadController through the Papyrus VM
// (GetActiveScene / GetActiveStage / GetPositions, async; the callbacks only fill a mailbox). StageStart is sent
// BEFORE P+ advances the stage, so StageStart / StageEnd / ActorsRelocated / PositionChange / AnimationChange open
// a 1.5 s window of re-reads every 150 ms, and a running scene is re-read every 2 s anyway (the P+ HUD and SLUI
// change scenes and positions without any event). A new key counts once two reads in a row agree.
//
// Every state change runs on the main thread (the sink only queues a task).
namespace SceneTracker {
    void Register();                // kDataLoaded
    void Reset(const char* a_why);  // kPreLoadGame / kPostLoadGame / kNewGame: thread ids from before the load mean nothing

    bool IsSceneActive();           // any tracked SexLab thread running
    bool PlayerSceneActive();       // a thread with the player (or one we could not inspect) is running

    // 0.10.0: the player's scene. Main thread only.
    struct PlayerScene {
        bool                         inScene    = false;  // == PlayerSceneActive()
        bool                         stageKnown = false;  // sceneId + stageId read and confirmed
        int                          tid        = -1;
        std::string                  sceneId;             // SLSB ids, lower case, "" unknown
        std::string                  stageId;
        std::string                  sceneName;           // SexLabRegistry.GetSceneName, "" until it arrives
        int                          playerSlot = -1;     // the player's index in GetPositions(), -1 unknown
        std::vector<RE::ActorHandle> actors;              // GetPositions() order
        std::vector<RE::FormID>      actorIDs;
        std::uint32_t                keySerial  = 0;      // bumps whenever the stage key or the scene state changes
    };
    const PlayerScene& GetPlayerScene();

    void Tick();       // main thread, from SceneCam's tick: sends the VM reads, applies their answers
    bool NeedsTick();  // the player's scene runs (reads are due)
}
