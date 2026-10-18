#pragma once

// The C API of runtime/eyecam_ffi, a small Rust crate over eyecam's live engine (third_party/eyecam) for both eyes.
// Eye 0 is the left eye of the wearer. One handle must only be used from one thread.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct EyecamSquint EyecamSquint;

typedef struct {
    float squint;
    uint8_t valid, closed;
} EyecamOut;

typedef struct {
    int32_t ok;
    int32_t fail;       // 0 none, 1 not enough usable frames, 2 squint not deep enough, 3 look-down check failed, 4 other.
    float f_sq[2];      // The fitted squint depth per eye, or NaN.
    char message[512];  // eyecam's own message in UTF-8.
} EyecamCalibResult;

// Reads the calibration file in eyecam's calib.json format, and writes beep WAVs to beep_dir ("" turns beeps off).
EyecamSquint *eyecam_new(const char *calib_path, const char *beep_dir);
void eyecam_free(EyecamSquint *h);

// Runs one upright 400x400 Gray8 frame with Valve's pitch in degrees and openness (NaN without them) at t seconds.
void eyecam_process(EyecamSquint *h, int32_t eye, const uint8_t *img, double pitch_deg, double valve_open, double t,
                    EyecamOut *out);

int32_t eyecam_ready(const EyecamSquint *h);            // Both wear baselines are ready.
int32_t eyecam_calibrated(const EyecamSquint *h);       // User parameters are loaded.
int32_t eyecam_wear_calibrated(const EyecamSquint *h);  // This wear has a wear calibration.
double eyecam_fsq(const EyecamSquint *h, int32_t eye);  // The loaded squint depth, or NaN.

// Starts a new wear, which also drops a running calibration.
void eyecam_reset_wear(EyecamSquint *h);

// A calibration: begin (kind 0 wear, 1 user; returns 0 when a user calibration has no wear calibration), the steps
// as they start on the frame clock, then finish, which fits, saves the file and uses the result at once.
int32_t eyecam_calib_begin(EyecamSquint *h, int32_t kind);
void eyecam_calib_step(EyecamSquint *h, const char *label, double t0, double seconds);
int32_t eyecam_calib_finish(EyecamSquint *h, EyecamCalibResult *out);
void eyecam_calib_cancel(EyecamSquint *h);

// Plays eyecam's beep for a step label, or "end".
void eyecam_beep(EyecamSquint *h, const char *label);

#ifdef __cplusplus
}
#endif
