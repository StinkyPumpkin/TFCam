#pragma once

// 0.7.8: SexLab (P+) scene tracking for the SLCC camera cycle.
//
// P+ 2.17.1 sslThreadModel.SetupThreadEvent sends every thread hook twice:
//   ModEvent.Create("Hook" + name) + PushInt(tid) + PushBool(HasPlayer) + Send  -> Papyrus registrations only
//   Form.SendModEvent(name, tid as string)                                        -> also SKSE's ModCallbackEvent source
// so a native sink sees the BARE names ("AnimationStart", "AnimationEnding", "AnimationEnd") with the thread id
// in strArg and the thread quest as sender. SLCC's native path reads the same events the same way.
// The player's thread is found from the sender quest's reference aliases at AnimationStart.
//
// Every state change runs on the main thread (the sink only queues a task).
namespace SceneTracker {
    void Register();                // kDataLoaded
    void Reset(const char* a_why);  // kPostLoadGame / kNewGame: thread ids from before the load mean nothing

    bool IsSceneActive();           // any tracked SexLab thread running
    bool PlayerSceneActive();       // a thread with the player (or one we could not inspect) is running
}
