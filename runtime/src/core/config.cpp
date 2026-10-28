#include "core/config.h"

#include "core/log.h"
#include "core/paths.h"
#include "defaults_json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <utility>

namespace {
const babble::log::Logger LOG("config");
}

namespace babble {

std::optional<json> parse_json(const std::string &text, std::string *error) {
    try {
        return json::parse(text, nullptr, true, true);
    } catch (const json::exception &e) {
        if (error)
            *error = e.what();
        return std::nullopt;
    }
}

json merge_config(json defaults, const json &user) {
    defaults.merge_patch(user);
    return defaults;
}

namespace {

const char *kind(const json &j) {
    if (j.is_object()) return "object";
    if (j.is_array()) return "array";
    if (j.is_string()) return "string";
    if (j.is_boolean()) return "boolean";
    if (j.is_number()) return "number";
    return "null";
}

// These options can have more than one type, for example "ipd" can be "auto" or a value in metres.
bool flexible(const std::string &key) { return key == "ipd" || key == "one_euro" || key == "pupil_one_euro_mm"; }

void fix_types_at(json &cfg, const json &defaults, const std::string &path, std::vector<std::string> &fixed) {
    for (auto it = defaults.begin(); it != defaults.end(); ++it) {
        if (!cfg.contains(it.key()) || it.value().is_null() || flexible(it.key()))
            continue;
        json &v = cfg[it.key()];
        const std::string p = path + "/" + it.key();
        if (it.value().is_object() && v.is_object()) {
            fix_types_at(v, it.value(), p, fixed);
        } else if (std::string(kind(v)) != kind(it.value())) {
            fixed.push_back(p + ": " + kind(v) + ", expected " + kind(it.value()));
            v = it.value();
        }
    }
}

// Collects the JSON pointer of each leaf value in a JSON object.
void leaves(const json &j, const std::string &path, std::vector<std::string> &out) {
    for (auto it = j.begin(); it != j.end(); ++it) {
        const std::string p = path + "/" + it.key();
        if (it.value().is_object() && !it.value().empty())
            leaves(it.value(), p, out);
        else
            out.push_back(p);
    }
}

}

std::vector<std::string> fix_types(json &cfg, const json &defaults) {
    std::vector<std::string> fixed;
    if (cfg.is_object() && defaults.is_object())
        fix_types_at(cfg, defaults, "", fixed);
    return fixed;
}

const json &section(const json &cfg, const char *key) {
    static const json empty = json::object();
    if (!cfg.is_object())
        return empty;
    auto it = cfg.find(key);
    return it != cfg.end() && it->is_object() ? *it : empty;
}

namespace {

enum class Layer { Missing, Loaded, Broken };

// Reads one user layer, merges it into config and puts the layer in *layer_out.
Layer merge_layer(const std::filesystem::path &file, json &config, std::string *error, json *layer_out = nullptr) {
    std::ifstream in(file);
    if (!in)
        return Layer::Missing;
    std::stringstream ss;
    ss << in.rdbuf();
    auto user = parse_json(ss.str(), error);
    if (!user)
        return Layer::Broken;
    if (!user->is_object()) {
        *error = "top level is not an object";
        return Layer::Broken;
    }
    config = merge_config(std::move(config), *user);
    if (layer_out)
        *layer_out = std::move(*user);
    return Layer::Loaded;
}

}

std::string migrate_output_choice(json &config, const std::vector<const json *> &layers) {
    std::string old;
    for (const json *layer : layers)
        if (layer->is_object() && layer->contains("output") && (*layer)["output"].is_string())
            old = (*layer)["output"].get<std::string>();
    if (old.empty())
        return "";
    if (config.contains("outputs") && config["outputs"].is_object())
        for (auto &[id, on] : config["outputs"].items())
            if (std::none_of(layers.begin(), layers.end(), [&](const json *l) { return section(*l, "outputs").contains(id); }))
                on = id == old;
    config.erase("output");
    return old;
}

bool migrate_face_mirror(json &config, const std::vector<const json *> &layers) {
    std::optional<bool> mirror;
    bool flip_set = false;
    for (const json *layer : layers) {
        const json &face = section(section(*layer, "pipelines"), "face");
        if (face.contains("mirror") && face["mirror"].is_boolean())
            mirror = face["mirror"].get<bool>();
        flip_set |= section(section(*layer, "sources"), "face").contains("tracker_flip_h");
    }
    if (config.contains("pipelines") && config["pipelines"].contains("face") && config["pipelines"]["face"].is_object())
        config["pipelines"]["face"].erase("mirror");
    if (!mirror || flip_set)
        return false;
    config["sources"]["face"]["tracker_flip_h"] = *mirror;
    return true;
}

bool migrate_gaze_smoothing(json &config, const std::vector<const json *> &layers) {
    std::optional<int> level;
    for (const json *layer : layers) {
        const json &gaze = section(section(*layer, "pipelines"), "gaze");
        const json on = gaze.value("smoothing", json()), strength = gaze.value("smoothing_strength", json());
        if (on.is_number())
            level = on.get<int>();
        else if (on.is_boolean() && !on.get<bool>())
            level = 0;
        else if (strength.is_number())
            level = strength.get<int>();
        else if (on.is_boolean())
            level = 1;   // The fork's default strength.
    }
    if (!config.contains("pipelines") || !config["pipelines"].contains("gaze") || !config["pipelines"]["gaze"].is_object())
        return false;
    json &gaze = config["pipelines"]["gaze"];
    const bool old = gaze.contains("smoothing_strength") || gaze.value("smoothing", json()).is_boolean();
    gaze.erase("smoothing_strength");
    if (!old || !level)
        return false;
    gaze["smoothing"] = std::clamp(*level, 0, 3);
    return true;
}

namespace {

// Returns the number at ptr rounded to an int, or fallback if it is missing or not a number.
int int_at(const json &j, const char *ptr, int fallback) {
    const json::json_pointer p(ptr);
    return j.contains(p) && j[p].is_number() ? int(std::lround(j[p].get<double>())) : fallback;
}

// Returns "303a:8000" for a vendor or product id given as a hex string or a number, as usb::device_id writes them.
std::string usb_pair(const json &face) {
    auto id = [&](const char *key, unsigned fallback) {
        const json v = face.value(key, json());
        return v.is_number_unsigned() ? v.get<unsigned>()
               : v.is_string()        ? unsigned(std::strtoul(v.get<std::string>().c_str(), nullptr, 16))
                                      : fallback;
    };
    char buf[16];
    snprintf(buf, sizeof(buf), "%04x:%04x", id("vendor", 0x303a), id("product", 0x8000));
    return buf;
}

// Returns the box [x, y, w, h] of a fw x fh frame after the USB source flips the frame and turns it clockwise.
json turn_box(const json &roi, int fw, int fh, int rotate, bool flip_h, bool flip_v) {
    auto at = [&](double x, double y) -> std::pair<double, double> {
        if (flip_h)
            x = fw - x;
        if (flip_v)
            y = fh - y;
        switch (rotate) {
        case 90: return {fh - y, x};
        case 180: return {fw - x, fh - y};
        case 270: return {y, fw - x};
        default: return {x, y};
        }
    };
    const double x = roi[0].get<double>(), y = roi[1].get<double>(), w = roi[2].get<double>(), h = roi[3].get<double>();
    const auto [ax, ay] = at(x, y);
    const auto [bx, by] = at(x + w, y + h);
    return json::array({std::lround(std::min(ax, bx)), std::lround(std::min(ay, by)), std::lround(std::fabs(bx - ax)),
                        std::lround(std::fabs(by - ay))});
}

}

bool migrate_face_camera(json &config, json &settings, const json &defaults) {
    const json &face = section(section(config, "sources"), "face");
    const json cam_id = face.value("camera", json());
    if (!cam_id.is_string() || cam_id.get<std::string>().empty() ||
        (face.contains("cameras") && !face["cameras"].is_object()))
        return false;
    const std::string id = cam_id.get<std::string>();
    std::string token;   // The id as a JSON pointer token, as the dashboard escapes it.
    for (char ch : id)
        token += ch == '~' ? "~0" : ch == '/' ? "~1" : std::string(1, ch);
    const json::json_pointer cam_ptr("/sources/face/cameras/" + token);
    json cam = config.contains(cam_ptr) && config[cam_ptr].is_object() ? config[cam_ptr] : json::object();
    if (config.contains(cam_ptr) && !config[cam_ptr].is_object())
        return false;
    json changes = json::object();   // The new values, by JSON pointer.
    std::vector<std::string> erase;   // The keys of sources.face that go.

    // The fork saved one mode for every camera, with "yuyv" for upstream's "yuy2".
    int fw = int_at(config, "/sources/face/width", 240), fh = int_at(config, "/sources/face/height", 240);
    const json old_format = face.value("format", json());
    if (old_format.is_string()) {
        const std::string f = old_format == "yuyv" ? "yuy2" : old_format.get<std::string>();
        if ((f == "mjpeg" || f == "yuy2") && !cam.contains("format")) {
            cam["format"] = f;
            cam["width"] = fw;
            cam["height"] = fh;
            cam["fps"] = face.contains("fps") && face["fps"].is_number() ? face["fps"] : json(30);
            for (const char *key : {"width", "height", "fps"}) {
                const json &d = section(section(defaults, "sources"), "face");
                if (d.contains(key))
                    changes["/sources/face/" + std::string(key)] = d[key];
            }
        }
        erase.push_back("format");
    } else if (cam.contains("width") && cam.contains("height")) {
        fw = int_at(cam, "/width", fw);
        fh = int_at(cam, "/height", fh);
    }

    // The model-side turn was counter-clockwise, and the camera turns clockwise; flips come before the turn in both.
    const json mode = face.value("mode", json("usb"));
    const int rot = int_at(config, "/pipelines/face/rotate", 0);
    const json::json_pointer flip_ptr("/pipelines/face/flip");
    const std::string flip = config.contains(flip_ptr) && config[flip_ptr].is_string() ? config[flip_ptr].get<std::string>()
                                                                                       : "none";
    const bool mh = flip.find('h') != std::string::npos, mv = flip.find('v') != std::string::npos;
    if (mode == "usb" && !cam.contains("rotate") && !cam.contains("flip_h") && !cam.contains("flip_v") &&
        (rot == 90 || rot == 180 || rot == 270 || mh || mv) && rot % 90 == 0 && rot >= 0 && rot < 360) {
        const int cw = (360 - rot) % 360;
        // The Babble tracker's default mirror (tracker_flip_h) stays, with the model-side flip on top of it.
        const bool tracker = id.starts_with(usb_pair(face)) && id.size() > 9 && (id[9] == ':' || id[9] == '@') &&
                             face.value("tracker_flip_h", json(true)) == true;
        cam["rotate"] = cw;
        cam["flip_h"] = tracker != mh;
        cam["flip_v"] = mv;
        changes["/pipelines/face/rotate"] = 0;
        changes["/pipelines/face/flip"] = "none";
        const json::json_pointer roi_ptr("/pipelines/face/roi");
        const json roi = config.contains(roi_ptr) ? config[roi_ptr] : json::array();
        if (roi.is_array() && roi.size() == 4 && std::all_of(roi.begin(), roi.end(), [](const json &v) {
                return v.is_number() && v.get<double>() >= 0;
            })) {
            const bool fits = roi[2].get<double>() > 0 && roi[3].get<double>() > 0 &&
                              roi[0].get<double>() + roi[2].get<double>() <= fw &&
                              roi[1].get<double>() + roi[3].get<double>() <= fh;
            changes["/pipelines/face/roi"] = fits ? turn_box(roi, fw, fh, cw, mh, mv) : json::array();
        }
    }
    if (changes.empty() && erase.empty())
        return false;

    if (cam != (config.contains(cam_ptr) ? config[cam_ptr] : json()))
        changes[cam_ptr.to_string()] = cam;
    for (auto &[ptr, value] : changes.items()) {
        config[json::json_pointer(ptr)] = value;
        settings[json::json_pointer(ptr)] = value;
    }
    for (const std::string &key : erase) {
        config["sources"]["face"].erase(key);
        if (settings.contains("sources") && settings["sources"].is_object() && settings["sources"].contains("face") &&
            settings["sources"]["face"].is_object())
            settings["sources"]["face"].erase(key);
    }
    return true;
}

std::filesystem::path default_settings_file() { return paths::config_dir() / "settings.json"; }

bool save_setting(const std::filesystem::path &settings_file, const json::json_pointer &where, const json &value,
                  std::string *error) {
    json doc = json::object();
    {
        std::ifstream in(settings_file);
        if (in) {
            std::stringstream ss;
            ss << in.rdbuf();
            std::string parse_error;
            auto parsed = parse_json(ss.str(), &parse_error);
            if (parsed && parsed->is_object()) {
                doc = std::move(*parsed);
            } else {
                // Keep the broken file for inspection so that the saved settings are not lost.
                std::filesystem::path bad = settings_file;
                bad += ".bad";
                std::error_code ec;
                std::filesystem::rename(settings_file, bad, ec);
                LOG.warn("config: %s was unreadable (%s); moved it to %s and started a new one", settings_file.c_str(),
                         parsed ? "top level is not an object" : parse_error.c_str(), bad.c_str());
            }
        }
    }
    doc[where] = value;
    return write_settings(settings_file, doc, error);
}

bool write_settings(const std::filesystem::path &settings_file, const json &doc, std::string *error) {
    std::filesystem::path tmp = settings_file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << doc.dump(2) << "\n";
        if (!out) {
            if (error)
                *error = "cannot write " + tmp.string();
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, settings_file, ec);
    if (ec && error)
        *error = ec.message();
    return !ec;
}

ConfigLoad load_config(const std::optional<std::filesystem::path> &user_file_override,
                       const std::optional<std::filesystem::path> &settings_file_override) {
    ConfigLoad out;
    std::string err;
    auto defaults = parse_json(kDefaultsJson, &err);
    if (!defaults) {
        // A parse failure of the compiled-in defaults is a build bug.
        LOG.error("built-in defaults.json is invalid: %s", err.c_str());
        out.config = json::object();
    } else {
        out.config = *defaults;   // A copy, because the migrations and fix_types below read the defaults.
    }

    out.user_file = user_file_override ? *user_file_override : paths::config_dir() / "config.json";
    json user_layer;
    out.user_loaded = merge_layer(out.user_file, out.config, &out.error, &user_layer) == Layer::Loaded;
    out.settings_file = settings_file_override ? *settings_file_override : default_settings_file();
    json settings_layer = json::object();
    const Layer settings_state = merge_layer(out.settings_file, out.config, &out.settings_error, &settings_layer);
    out.settings_loaded = settings_state == Layer::Loaded;
    out.legacy_output = migrate_output_choice(out.config, {&user_layer, &settings_layer});
    migrate_face_mirror(out.config, {&user_layer, &settings_layer});
    migrate_gaze_smoothing(out.config, {&user_layer, &settings_layer});
    // The mouth camera move is saved once, so that a later camera turn is not added to the old model-side one. A
    // broken settings file stays as it is for inspection.
    if (settings_state != Layer::Broken && migrate_face_camera(out.config, settings_layer, defaults ? *defaults : json())) {
        std::string err;
        if (write_settings(out.settings_file, settings_layer, &err)) {
            LOG.info("config: moved the mouth camera settings into %s (saved to %s)",
                     out.config["sources"]["face"].value("camera", "").c_str(), out.settings_file.c_str());
            out.settings_loaded = true;
        } else {
            LOG.warn("config: moving the mouth camera settings: %s", err.c_str());
        }
    }
    if (out.settings_loaded)
        leaves(settings_layer, "", out.settings_overrides);
    if (defaults)
        out.type_fixes = fix_types(out.config, *defaults);
    return out;
}

}
