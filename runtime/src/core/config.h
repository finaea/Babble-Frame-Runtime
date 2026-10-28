#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace babble {

using json = nlohmann::json;

struct ConfigLoad {
    json config;                           // The defaults with the user file and the settings file merged over them.
    std::filesystem::path user_file;       // The path where the loader looked for the user file.
    bool user_loaded = false;
    std::string error;                     // This is not empty if the user file exists but the loader rejected it.
    std::filesystem::path settings_file;   // The path where the loader looked for the settings file.
    bool settings_loaded = false;
    std::string settings_error;
    // The keys that had a type different from the defaults, which the loader reset to the default value.
    std::vector<std::string> type_fixes;
    // The leaf keys that settings.json sets over config.json.
    std::vector<std::string> settings_overrides;
    // The old single "output" choice that the loader changed into the "outputs" switches, or "" if none.
    std::string legacy_output;
};

// Merges config.json and then settings.json over the defaults with RFC 7386, and reports and ignores a broken file.
ConfigLoad load_config(const std::optional<std::filesystem::path> &user_file_override,
                       const std::optional<std::filesystem::path> &settings_file_override = std::nullopt);

// Returns $XDG_CONFIG_HOME/babble-frame-runtime/settings.json.
std::filesystem::path default_settings_file();

// Sets one value in the settings file and replaces the file atomically.
bool save_setting(const std::filesystem::path &settings_file, const json::json_pointer &where, const json &value,
                  std::string *error);
// Replaces the settings file with doc atomically.
bool write_settings(const std::filesystem::path &settings_file, const json &doc, std::string *error);

// The merge that load_config uses, which is public for the tests.
json merge_config(json defaults, const json &user);

// Changes the old single "output" choice of the newest layer that has one into the "outputs" switches, where a switch
// that any layer sets stays as it is. The layers are oldest first. It removes "output" from config and returns the old
// choice, or "" if no layer has one.
std::string migrate_output_choice(json &config, const std::vector<const json *> &layers);
// Moves a saved "pipelines.face.mirror" (the old global Mirror Image switch) to "sources.face.tracker_flip_h", the
// default horizontal flip of the Babble tracker, unless a layer already sets that. Returns true if it moved a value.
bool migrate_face_mirror(json &config, const std::vector<const json *> &layers);
// Changes the fork's old gaze smoothing ("smoothing" as a switch and "smoothing_strength" 0-3) into the "smoothing"
// level, where off is 0. In each layer a numeric "smoothing" is newer and wins. The layers are oldest first. Returns
// true if it changed a value.
bool migrate_gaze_smoothing(json &config, const std::vector<const json *> &layers);

// Moves the fork's mouth camera settings into the entry of the selected USB camera ("sources.face.cameras.<id>"),
// where V1.2 keeps them, and changes settings (the settings file layer) the same way so that it can be saved:
// - The old "sources.face.format", "width", "height" and "fps" become the mode of the camera, unless it has a saved
//   format; "width", "height" and "fps" go back to the defaults, and the old "format" goes.
// - In USB mode, the model-side "pipelines.face.rotate" (counter-clockwise) and "flip" become the clockwise turn and
//   flips of the camera, unless it has a saved turn or flip. The crop box turns with the frame, or becomes the whole
//   frame if it does not fit the saved size, and the model-side turn becomes 0 and "none".
// Returns true if it changed anything.
bool migrate_face_camera(json &config, json &settings, const json &defaults);

// Resets each key of cfg that has a JSON kind different from defaults, and returns the keys that it reset.
std::vector<std::string> fix_types(json &cfg, const json &defaults);

// Returns a config section, or an empty object if the section is missing, and never inserts like operator[].
const json &section(const json &cfg, const char *key);

// Parses JSON that can contain comments, and returns nullopt on an error.
std::optional<json> parse_json(const std::string &text, std::string *error);

}
