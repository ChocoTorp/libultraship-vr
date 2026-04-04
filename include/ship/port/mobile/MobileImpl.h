#pragma once

#include <cstdint>
#include <string>

#include <imgui.h>

namespace Ship {

class Mobile {
  public:
    static void ImGuiProcessEvent(bool wantsTextInput);
    static void SetToggleButtonVisible(bool visible);
#ifdef __ANDROID__
    static bool ConsumeGamepadBackPress();
    static void InjectMenuNavKeys();
#endif
};
}; // namespace Ship
