#ifndef PROTORELAY_CLI_HELP_TEXT_H
#define PROTORELAY_CLI_HELP_TEXT_H

#include <string>

namespace mail_system {
namespace cli {

std::string render_help_text(const std::string& program_name);
std::string render_version_text();

} // namespace cli
} // namespace mail_system

#endif // PROTORELAY_CLI_HELP_TEXT_H
