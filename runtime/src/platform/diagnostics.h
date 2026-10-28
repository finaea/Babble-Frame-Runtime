#pragma once

// These functions collect the data for the startup log and for a diagnostics file, for problem reports.

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace babble::diag {

// Returns a copy of the config with the values of secret keys, such as upload credentials, as "(not logged)".
nlohmann::json redacted(const nlohmann::json &cfg);

// Returns the value at the config path as JSON text for the log, or "(not logged)" for a secret key.
std::string log_value(const std::string &path, const nlohmann::json &value);

// Returns the session header with the version, the system, the config changes, the model files and the USB devices.
std::vector<std::string> session_info(const nlohmann::json &cfg, const std::filesystem::path &models_dir);

// Returns the name, state, wait channel and CPUs of each thread of this process.
std::vector<std::string> thread_report();

// Writes text to <dir>/diag-YYYYMMDD-HHMMSS.txt and returns the path, or an empty path on failure.
std::filesystem::path write_file(const std::filesystem::path &dir, const std::string &text, int keep = 5);

}
