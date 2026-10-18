// Replays a BFR eye dump through the eyecam_squint sink's engine path (EyecamSquintSink::upright + eyecam_process)
// and writes eye,eye_index,t,squint rows, to compare with eyecam-rec --replay on the converted session.
#include "frame/frame.h"
#include "sink/eyecam_ffi.h"
#include "sink/eyecam_squint_sink.h"

#include <sched.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace babble;

namespace {

std::vector<std::string> split(const std::string &s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string p;
    while (std::getline(ss, p, ','))
        out.push_back(p);
    return out;
}

// Reads a CSV into rows and returns the column index of each header name.
bool read_csv(const std::string &path, std::map<std::string, size_t> &cols, std::vector<std::vector<std::string>> &rows) {
    std::ifstream f(path);
    std::string line;
    if (!f || !std::getline(f, line))
        return false;
    auto head = split(line);
    for (size_t i = 0; i < head.size(); i++)
        cols[head[i]] = i;
    while (std::getline(f, line))
        if (!line.empty())
            rows.push_back(split(line));
    return true;
}

double num(const std::string &s) { return s.empty() ? NAN : atof(s.c_str()); }

}

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: squint_replay <dump dir> <session dir> <calib.json> <out.csv>\n");
        return 2;
    }
    const std::string dump = argv[1], session = argv[2];

    // Frame times by (eye, eye_index). Session eye L is the BFR right view (converted with swap=1).
    std::map<std::string, size_t> fc, vc;
    std::vector<std::vector<std::string>> frows, vrows;
    if (!read_csv(session + "/frames.csv", fc, frows) || !read_csv(session + "/valve.csv", vc, vrows)) {
        fprintf(stderr, "squint_replay: cannot read frames.csv or valve.csv in %s\n", session.c_str());
        return 1;
    }
    std::vector<double> t_cam[2];
    for (auto &r : frows) {
        const int e = r[fc["eye"]] == "L" ? 0 : 1;
        const size_t k = size_t(atol(r[fc["eye_index"]].c_str()));
        if (t_cam[e].size() <= k)
            t_cam[e].resize(k + 1, NAN);
        t_cam[e][k] = num(r[fc["t_cam"]]);
    }

    // Valve samples, like eyecam's Session::open.
    std::vector<double> vt, pitch[2], open[2];
    const char *gy[2] = {"gaze_l_y", "gaze_r_y"}, *op[2] = {"open_l", "open_r"};
    for (auto &r : vrows) {
        vt.push_back(num(r[vc["sample_time"]]));
        for (int e = 0; e < 2; e++) {
            pitch[e].push_back(std::asin(std::clamp(num(r[vc[gy[e]]]), -1.0, 1.0)) * 180.0 / M_PI);
            open[e].push_back(num(r[vc[op[e]]]));
        }
    }
    auto nearest = [&](double t) -> long {   // Valve::nearest in third_party/eyecam/src/replay.rs
        if (vt.size() < 2)
            return -1;
        long j = std::partition_point(vt.begin(), vt.end(), [&](double v) { return v < t; }) - vt.begin();
        j = std::clamp<long>(j, 1, long(vt.size()) - 1);
        return std::fabs(vt[j - 1] - t) < std::fabs(vt[j] - t) ? j - 1 : j;
    };

    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(4, &set);
    sched_setaffinity(0, sizeof set, &set);

    EyecamSquint *engine = eyecam_new(argv[3], "");
    if (!engine) {
        fprintf(stderr, "squint_replay: eyecam_new failed\n");
        return 1;
    }
    std::ofstream out(argv[4]);
    out << "eye,eye_index,t,squint\n";
    std::vector<uint8_t> px(800 * 400), img[2] = {std::vector<uint8_t>(400 * 400), std::vector<uint8_t>(400 * 400)};
    double total_s = 0;
    size_t n = 0;
    for (size_t k = 0;; k++) {
        char path[512];
        snprintf(path, sizeof path, "%s/eye_%03zu.pgm", dump.c_str(), k);
        std::ifstream f(path, std::ios::binary);
        if (!f)
            break;
        std::string l1, l2, l3, l4;
        std::getline(f, l1);
        std::getline(f, l2);
        std::getline(f, l3);
        std::getline(f, l4);
        f.read(reinterpret_cast<char *>(px.data()), std::streamsize(px.size()));
        if (l1 != "P5" || l3 != "800 400" || size_t(f.gcount()) != px.size()) {
            fprintf(stderr, "squint_replay: bad pgm %s\n", path);
            return 1;
        }
        // Side by side: left view | right view, each 400x400 inside the 800-wide rows.
        Frame view[2];   // 0 = BFR left view, 1 = BFR right view.
        for (int v = 0; v < 2; v++) {
            view[v].fmt = PixelFormat::Gray8;
            view[v].width = view[v].height = 400;
            view[v].stride = 800;
            view[v].data = px.data() + v * 400;
            view[v].size = size_t(800) * 399 + 400;
        }
        // Eye 0 (the wearer's left) is the right view, flipped; eye 1 is the left view, as in EyecamSquintSink::process.
        if (!EyecamSquintSink::upright(view[1], true, img[0].data()) ||
            !EyecamSquintSink::upright(view[0], false, img[1].data()))
            return 1;
        for (int e = 0; e < 2; e++) {
            if (k >= t_cam[e].size()) {
                fprintf(stderr, "squint_replay: no frame time for eye %d index %zu\n", e, k);
                return 1;
            }
            const double t = t_cam[e][k];
            const long j = nearest(t);
            EyecamOut o{};
            const auto t0 = std::chrono::steady_clock::now();
            eyecam_process(engine, e, img[e].data(), j < 0 ? NAN : pitch[e][j], j < 0 ? NAN : open[e][j], t, &o);
            total_s += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            n++;
            char line[128];
            snprintf(line, sizeof line, "%d,%zu,%.9f,%.9g\n", e, k, t, double(o.squint));
            out << line;
        }
    }
    eyecam_free(engine);
    fprintf(stderr, "squint_replay: %zu eye frames, %.3f ms per eye frame\n", n, n ? total_s / double(n) * 1e3 : 0.0);
    return 0;
}
