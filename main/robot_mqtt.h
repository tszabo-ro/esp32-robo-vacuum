#pragma once

#include <string_view>

namespace robot_mqtt {

// Home Assistant's command vocabulary, mapped to the controller allowlist.
// Raw UART strings and unsupported features are never accepted over MQTT.
inline const char* command_action(std::string_view payload)
{
    if (payload == "start") return "start";
    if (payload == "stop") return "stop";
    if (payload == "return_to_base") return "dock";
    if (payload == "clean_spot") return "spot";
    return nullptr;
}

// Unknown states return nullptr, published as JSON null (HA's unknown state).
inline const char* activity(std::string_view ui, std::string_view robot, bool docked, bool fault)
{
    if (fault || robot == "ST_K_Critical" || robot == "ST_L_Safety"
        || robot == "ST_F7_CleaningError" || robot == "ST_G7_CleaningError") return "error";
    if (ui == "UIMGR_STATE_HOUSECLEANINGPAUSED" || ui == "UIMGR_STATE_SPOTCLEANINGPAUSED"
        || ui == "UIMGR_STATE_DOCKINGPAUSED") return "paused";
    if (ui == "UIMGR_STATE_DOCKINGRUNNING") return "returning";
    if (ui == "UIMGR_STATE_HOUSECLEANINGRUNNING" || ui == "UIMGR_STATE_SPOTCLEANINGRUNNING"
        || ui == "UIMGR_STATE_STARTHOUSECLEANING" || ui == "UIMGR_STATE_STARTSPOTCLEANING") return "cleaning";
    if (docked && (ui == "UIMGR_STATE_IDLE" || ui == "UIMGR_STATE_STANDBY"
        || ui == "UIMGR_STATE_CLEANINGSUSPENDED" || ui == "UIMGR_STATE_CLEANINGSUSPENDEDMENU")) return "docked";
    if (ui == "UIMGR_STATE_CLEANINGSUSPENDED" || ui == "UIMGR_STATE_CLEANINGSUSPENDEDMENU") return "paused";
    if (ui == "UIMGR_STATE_IDLE" || ui == "UIMGR_STATE_STANDBY") return "idle";
    return nullptr;
}

} // namespace robot_mqtt
