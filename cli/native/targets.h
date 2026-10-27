#pragma once
#include <array>
#include <string>
namespace arkit {
inline const std::array<std::string, 52> target_names = {
    "EyeBlinkLeft",       "EyeLookDownLeft",
    "EyeLookInLeft",      "EyeLookOutLeft",
    "EyeLookUpLeft",      "EyeSquintLeft",
    "EyeWideLeft",        "EyeBlinkRight",
    "EyeLookDownRight",   "EyeLookInRight",
    "EyeLookOutRight",    "EyeLookUpRight",
    "EyeSquintRight",     "EyeWideRight",
    "JawForward",         "JawLeft",
    "JawRight",           "JawOpen",
    "MouthFunnel",        "MouthPucker",
    "MouthLeft",          "MouthRight",
    "MouthSmileLeft",     "MouthSmileRight",
    "MouthFrownLeft",     "MouthFrownRight",
    "MouthDimpleLeft",    "MouthDimpleRight",
    "MouthStretchLeft",   "MouthStretchRight",
    "MouthRollLower",     "MouthRollUpper",
    "MouthShrugLower",    "MouthShrugUpper",
    "MouthPressLeft",     "MouthPressRight",
    "MouthLowerDownLeft", "MouthLowerDownRight",
    "MouthUpperUpLeft",   "MouthUpperUpRight",
    "BrowDownLeft",       "BrowDownRight",
    "BrowInnerUp",        "BrowOuterUpLeft",
    "BrowOuterUpRight",   "CheekPuff",
    "CheekSquintLeft",    "CheekSquintRight",
    "NoseSneerLeft",      "NoseSneerRight",
    "TongueOut",          "MouthClose",
};
}
