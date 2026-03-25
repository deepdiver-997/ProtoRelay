#include "mail_system/back/cli/help_text.h"

#include "build_info.h"

#include <sstream>

namespace mail_system {
namespace cli {

namespace {

const char* kProgramName = "ProtoRelay";

const char* kFeatureSsl =
#if PROTORELAY_FEATURE_SSL
    "+ssl";
#else
    "-ssl";
#endif

const char* kFeatureDbDedup =
#if PROTORELAY_FEATURE_INBOUND_DEDUP
    "+inbound-dedup";
#else
    "-inbound-dedup";
#endif

const char* kFeatureObjectOnly =
#if PROTORELAY_FEATURE_OBJECT_ONLY
    "+object-only";
#else
    "-object-only";
#endif

const char* kFeatureHdfsWeb =
#if PROTORELAY_FEATURE_HDFS_WEB
    "+hdfs-web";
#else
    "-hdfs-web";
#endif

} // namespace

std::string render_help_text(const std::string& program_name) {
    std::ostringstream oss;
    oss << kProgramName << " - SMTP relay core\n"
        << "\n"
        << "Usage:\n"
        << "  " << program_name << " [options] [config_path]\n"
        << "\n"
        << "Options:\n"
        << "  -h, --help                 Show this help and exit\n"
        << "  -V, --version              Show version/build information and exit\n"
        << "  -c, --config <path>        Config file path (default: config/smtpsConfig.json)\n"
        << "\n"
        << "Examples:\n"
        << "  " << program_name << "\n"
        << "  " << program_name << " -c config/smtpsConfig_hdfs_web.json\n"
        << "  " << program_name << " --version\n"
        << "\n"
        << "Current scope:\n"
        << "  - SMTP state machine\n"
        << "  - SMTP protocol parsing\n"
        << "  - SMTP delivery pipeline\n"
        << "\n"
        << "More docs: docs/PROJECT_STYLE.md\n";
    return oss.str();
}

std::string render_version_text() {
    std::ostringstream oss;
    oss << kProgramName << " " << PROTORELAY_VERSION << "\n"
        << "Commit: " << PROTORELAY_GIT_COMMIT << "\n"
        << "Build-Time: " << PROTORELAY_BUILD_UTC << "\n"
        << "Target: " << PROTORELAY_BUILD_TARGET << "\n"
        << "Compiler: " << PROTORELAY_COMPILER << "\n"
        << "Features: smtp smtps " << kFeatureSsl << " " << kFeatureDbDedup << " " << kFeatureObjectOnly << " " << kFeatureHdfsWeb << "\n";
    return oss.str();
}

} // namespace cli
} // namespace mail_system
