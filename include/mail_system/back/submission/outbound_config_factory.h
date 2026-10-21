#pragma once
#include "mail_system/back/server_config.h"
namespace mail_system::submission {
inline outbound::OutboundConfig outbound_config(const ServerConfig &cfg) {
    outbound::OutboundConfig out;
    out.helo_domain = cfg.outbound_helo_domain;
    out.mail_from_domain = cfg.outbound_mail_from_domain;
    out.rewrite_header_from = cfg.outbound_rewrite_header_from;
    out.dkim_enabled = cfg.outbound_dkim_enabled;
    out.dkim_selector = cfg.outbound_dkim_selector;
    out.dkim_domain = cfg.outbound_dkim_domain;
    out.dkim_private_key_file = cfg.outbound_dkim_private_key_file;
    out.ports = cfg.outbound_ports;
    out.max_attempts = cfg.outbound_max_attempts;
    out.static_routes = cfg.outbound_static_routes;
    out.default_route = cfg.outbound_default_route;
    return out;
}
} // namespace mail_system::submission
