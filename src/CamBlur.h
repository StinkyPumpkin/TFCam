#pragma once

// 0.17.0: background blur for TFCam's own cameras. While orbit / preset / eye view, a camera path, the idle camera or
// the conversation camera moves TFCam's free camera (and, optionally, in plain free-fly), ENB's depth of field is
// focused on the subject and its far blur made stronger; the moment that ends, ENB gets its own values back.
//
// Through the official ENB SDK (ENBGetParameter / ENBSetParameter, only inside ENB's own callback - the render thread),
// on the shader variables of Marty McFly's ADOF in enbseries\enbdepthoffield.fx:
//   "DOF: Enable Autofocus" off + "DOF: Manual focus depth" = the subject's depth in the shader's own units (the game's
//   depth buffer value at the subject, run through the shader's LinearizeDepth), "DOF: Far blur curve" lower (stronger)
//   and "DOF: Bokeh shape max size" larger with the strength, and enbseries.ini [EFFECT] EnableDepthOfField on.
// Every changed value is put back when the blur ends, before the ENB editor saves (ENBCallback_PreSave) and is
// forgotten when ENB reloads its config (PostLoad). Without ENB (or another DoF shader) the box says so and does nothing.
//
// 10-31: a second style, the default now - TFCam's own image pass (FrameFx) blurs the picture more and more toward the
// edges and leaves the middle sharp (a focus vignette), the sharp middle following the subject on screen. No ENB
// needed, as strong as you like. The ENB depth of field stays as the other style.
namespace CamBlur {

    enum Style : int { kStyleEdges = 0, kStyleEnb = 1 };

    struct Settings {
        bool  enabled   = false;  // the box
        float strength  = 0.6f;   // 0..1: 0 = ENB's own blur, 1 = strongest
        bool  inFreeFly = false;  // also in plain free-fly (focus = what the middle of the screen sees)
        int   style     = kStyleEdges;  // 10-31: edges (TFCam's own pass) or ENB's depth of field
        float sharp     = 0.4f;         // edges: how far out from the middle stays sharp (0..1, 1 = the screen's edge)
    };

    // 10-31: what the image pass should blur this frame in the Edges style (FrameFx asks every frame). Main thread.
    struct Edge {
        bool         on          = false;
        float        strength    = 0.0f;
        bool         haveSubject = false;
        RE::NiPoint3 subject{};  // the sharp middle sits on it (on screen); none = the middle of the screen
    };
    Edge EdgeWant();

    // 10-31: the archer camera (FovFx) while the string is drawn - its own box, the Background blur strength and sharp
    // middle; a_subject = where the arrow will land. Main thread, every frame it is on; a_on false ends it.
    void Archer(bool a_on, const RE::NiPoint3& a_subject);

    Settings& Get();
    void      Clamp();

    void Install();  // kPostLoad (again at kDataLoaded if ENB was not there yet): link the SDK, register the callback

    // Main thread, the free camera's Update hook, after every camera wrote its pose (walls included).
    void Update(const RE::FreeCameraState* a_fcs, bool a_driving);
    void Off(const char* a_why);  // free camera ended / game load: ENB gets its values back next frame

    // TFCamAPI v5 (game thread): another mod's camera keeps a_subject sharp - every frame while it shows it; a_on false
    // or half a second without a call ends it. a_strength < 0 = the Background blur strength setting.
    void External(bool a_on, const RE::NiPoint3& a_subject, float a_strength);
    bool Available();  // ENB SDK linked and the ADOF variables not known to be missing

    struct Status {
        bool  linked     = false;  // ENB SDK found
        bool  shader     = false;  // the ADOF variables exist (known after the first frame with the box on)
        bool  active     = false;
        float distance   = 0.0f;   // camera to the subject, game units (0 = screen-centre autofocus)
        char  what[96]   = {};
        char  error[160] = {};
    };
    Status GetStatus();  // any thread
}
