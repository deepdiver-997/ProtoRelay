#include "mail_system/back/mailServer/fsm/imaps/imaps_fsm.h"
#include <unordered_map>

namespace mail_system {

std::string ImapsFsm::get_state_name(ImapsState state) {
    static const std::unordered_map<ImapsState, std::string> state_names = {
        {ImapsState::INIT, "INIT"},
        {ImapsState::GREETING, "GREETING"},
        {ImapsState::WAIT_LOGIN, "WAIT_LOGIN"},
        {ImapsState::WAIT_SELECT, "WAIT_SELECT"},
        {ImapsState::WAIT_FETCH, "WAIT_FETCH"},
        {ImapsState::WAIT_STORE, "WAIT_STORE"},
        {ImapsState::WAIT_EXPUNGE, "WAIT_EXPUNGE"},
        {ImapsState::WAIT_CLOSE, "WAIT_CLOSE"},
        {ImapsState::WAIT_LOGOUT, "WAIT_LOGOUT"},
        {ImapsState::CLOSED, "CLOSED"}
    };

    auto it = state_names.find(state);
    if (it != state_names.end()) {
        return it->second;
    }
    return "UNKNOWN_STATE";
}

std::string ImapsFsm::get_event_name(ImapsEvent event) {
    static const std::unordered_map<ImapsEvent, std::string> event_names = {
        {ImapsEvent::CONNECT, "CONNECT"},
        {ImapsEvent::LOGIN, "LOGIN"},
        {ImapsEvent::SELECT, "SELECT"},
        {ImapsEvent::FETCH, "FETCH"},
        {ImapsEvent::STORE, "STORE"},
        {ImapsEvent::EXPUNGE, "EXPUNGE"},
        {ImapsEvent::CLOSE, "CLOSE"},
        {ImapsEvent::LOGOUT, "LOGOUT"},
        {ImapsEvent::ERROR, "ERROR"},
        {ImapsEvent::TIMEOUT, "TIMEOUT"}
    };

    auto it = event_names.find(event);
    if (it != event_names.end()) {
        return it->second;
    }
    return "UNKNOWN_EVENT";
}

} // namespace mail_system