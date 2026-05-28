#pragma once

namespace CameraLight {
    void  Toggle();
    bool  IsActive();

    // Shift+Scroll: increase/decrease light
    void  ScrollUp();    // turn on or increase
    void  ScrollDown();  // decrease or turn off

    void  SetIntensity(float intensity);
    float GetIntensity();

    void  SetRadius(float radius);
    float GetRadius();

    void  SetFade(float fade);
    float GetFade();

    void  SetColor(float r, float g, float b);
    void  GetColor(float& r, float& g, float& b);

    // Menu checkboxes: what does scroll affect?
    void  SetScrollBrightness(bool enabled);
    bool  GetScrollBrightness();
    void  SetScrollRadius(bool enabled);
    bool  GetScrollRadius();

    void  Update();
    void  Cleanup();
}
