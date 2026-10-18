//! A small C API over eyecam's live engine for both eyes (runtime/src/sink/eyecam_ffi.h), for the eyecam_squint sink.
//! It drives the engine the way eyecam-rec does: `livesvc.rs` for frames, new wears and the calibrations, and
//! `replay.rs` (`write_live`) for the parameters a calibration file gives. One handle is used from one thread.

use eyecam::cues::Player;
use eyecam::live::{self, CalibFile, EyeEngine, Label, Params, Sample, UserParams};
use std::ffi::{CStr, c_char};
use std::path::{Path, PathBuf};

/// One frame's result for one eye.
#[repr(C)]
pub struct EyecamOut {
    pub squint: f32,
    pub valid: u8,
    pub closed: u8,
}

/// The result of `eyecam_calib_finish`.
#[repr(C)]
pub struct EyecamCalibResult {
    pub ok: i32,
    /// 0 none, 1 not enough usable frames, 2 squint not deep enough, 3 look-down check failed, 4 other.
    pub fail: i32,
    /// The fitted squint depth per eye (0 = left), NaN unless a user calibration went through.
    pub f_sq: [f32; 2],
    /// eyecam's own message (UTF-8, NUL-terminated).
    pub message: [c_char; 512],
}

#[derive(Clone, Copy, PartialEq)]
enum Kind {
    Wear,
    User,
}

pub struct Squint {
    path: PathBuf,
    calib: CalibFile,
    engines: [EyeEngine; 2],
    params: [Params; 2],
    collect: Option<Kind>,
    steps: Vec<(Label, f64, f64)>,
    samples: [Vec<Sample>; 2],
    player: Player,
}

fn stamp() -> String {
    let t = unsafe { libc::time(std::ptr::null_mut()) };
    let mut tm: libc::tm = unsafe { std::mem::zeroed() };
    unsafe { libc::localtime_r(&t, &mut tm) };
    format!("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec)
}

/// Reads the calibration file as eyecam-rec's `load_calib` does; a file from before the eyes were anatomical is
/// converted and saved again (this is our own copy, so no backup is kept).
fn load_calib(path: &Path) -> CalibFile {
    let Ok(text) = std::fs::read_to_string(path) else { return CalibFile::default() };
    match CalibFile::parse_converting(&text) {
        Ok((file, converted)) => {
            if converted && let Err(e) = save_calib(path, &file) {
                eprintln!("eyecam_ffi: {e}");
            }
            file
        }
        Err(e) => {
            eprintln!("eyecam_ffi: ignoring {}: {e}", path.display());
            CalibFile::default()
        }
    }
}

fn save_calib(path: &Path, file: &CalibFile) -> Result<(), String> {
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    }
    let tmp = path.with_extension("json.tmp");
    std::fs::write(&tmp, file.to_json()).map_err(|e| format!("{}: {e}", tmp.display()))?;
    std::fs::rename(&tmp, path).map_err(|e| format!("{}: {e}", path.display()))
}

/// The contract's fail reason for one of eyecam's calibration messages.
fn fail_code(message: &str) -> i32 {
    let has = |m: &str| message.contains(m);
    if has(live::MSG_PUPIL) || has(live::MSG_LID_LINE) || has(live::MSG_LIDS) || has(live::MSG_NO_GAZE) {
        1
    } else if has(live::MSG_SQUINT_MISSING) || has(live::MSG_SQUINT_SHALLOW) {
        2
    } else if has(live::MSG_LOOK_DOWN) || has(live::MSG_LOWER_UP) {
        3
    } else {
        4
    }
}

impl Squint {
    fn apply_calib(&mut self) {
        let hist = self.calib.history_params();
        for (e, p) in self.params.iter_mut().enumerate() {
            p.user = self.calib.user.map(|u| u[e]);
            p.hist = hist.map(|h| h[e]);
            p.pupil_measured = self.calib.pupil_measured;
        }
    }

    fn reset_wear(&mut self) {
        for e in &mut self.engines {
            e.reset();
        }
        for p in &mut self.params {
            p.wear = None;
        }
        self.cancel();
    }

    fn cancel(&mut self) {
        self.collect = None;
        self.steps.clear();
        self.samples = [Vec::new(), Vec::new()];
    }

    fn process(&mut self, eye: usize, img: &[u8], pitch: f64, open: f64, t: f64) -> live::Out {
        let o = self.engines[eye].process_open(img, pitch, open, t, &self.params[eye]);
        if self.collect.is_some() {
            let label = self.steps.iter().rev().find(|(_, t0, sec)| t >= t0 + live::STEP_DELAY && t < t0 + sec).map(|s| s.0);
            if let Some(l) = label {
                let smp = self.engines[eye].calib_sample(l, &o, t);
                self.samples[eye].push(smp);
            }
        }
        o
    }

    /// As livesvc's `fit`: a wear calibration seeds the engines and adds to the history, a user calibration sets the
    /// pitch curve and squint depth; both save the file.
    fn finish(&mut self) -> Result<(String, Option<[UserParams; 2]>), String> {
        let kind = self.collect.take().ok_or("not calibrating")?;
        let samples = std::mem::take(&mut self.samples);
        self.steps.clear();
        match kind {
            Kind::Wear => {
                for l in live::wear_report(&samples).1 {
                    eprintln!("{l}");
                }
                let hist = self.calib.history_params();
                let fallback = [0, 1].map(|e| hist.map_or(live::DEFAULT_WIDEN_STEP, |h| h[e].step));
                let out = live::fit_wear_settled(&samples, fallback, self.calib.measured_wear(), hist)?;
                let w = out.wear;
                for ((p, wp), eng) in self.params.iter_mut().zip(w).zip(&mut self.engines) {
                    p.wear = Some(wp);
                    eng.seed(&wp);
                }
                self.calib.wear = Some(w);
                self.calib.wear_time = stamp();
                if out.for_history() {
                    self.calib.push_history(&self.calib.wear_time.clone(), &w);
                }
                self.calib.wear_widen_measured = Some(out.widen_measured);
                self.calib.wear_failed_eye = out.failed_eye().to_string();
                self.calib.setup_done = true;
                self.apply_calib();
                save_calib(&self.path, &self.calib)?;
                Ok((out.message, None))
            }
            Kind::User => {
                let wear = match (self.params[0].wear, self.params[1].wear) {
                    (Some(l), Some(r)) => [l, r],
                    _ => return Err("no wear calibration this wear".into()),
                };
                for l in live::user_report(&samples, &wear).1 {
                    eprintln!("{l}");
                }
                let (u, warnings) = live::fit_user(&samples, &wear, self.calib.user, true)?;
                self.calib.user = Some(u);
                self.calib.user_time = stamp();
                self.calib.user_warnings = warnings.clone();
                save_calib(&self.path, &self.calib)?;
                self.apply_calib();
                let message = if warnings.is_empty() { String::new() } else { warnings.join(", ") };
                Ok((message, Some(u)))
            }
        }
    }
}

unsafe fn path_arg(p: *const c_char) -> PathBuf {
    if p.is_null() { PathBuf::new() } else { PathBuf::from(unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned()) }
}

fn write_message(out: &mut [c_char; 512], text: &str) {
    let mut n = text.len().min(out.len() - 1);
    while !text.is_char_boundary(n) {
        n -= 1;
    }
    for (d, s) in out.iter_mut().zip(&text.as_bytes()[..n]) {
        *d = *s as c_char;
    }
    out[n] = 0;
}

/// A new handle that reads (or later writes) the calibration file at `calib_path`; beeps are written to `beep_dir`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eyecam_new(calib_path: *const c_char, beep_dir: *const c_char) -> *mut Squint {
    let path = unsafe { path_arg(calib_path) };
    let beeps = unsafe { path_arg(beep_dir) };
    let mut s = Squint {
        calib: load_calib(&path),
        path,
        engines: [EyeEngine::default(), EyeEngine::default()],
        params: [Params::default(); 2],
        collect: None,
        steps: Vec::new(),
        samples: [Vec::new(), Vec::new()],
        player: Player::new(&beeps, 0.3, !beeps.as_os_str().is_empty()),
    };
    s.apply_calib();
    Box::into_raw(Box::new(s))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn eyecam_free(h: *mut Squint) {
    if !h.is_null() {
        drop(unsafe { Box::from_raw(h) });
    }
}

/// Runs one upright 400x400 grey frame of eye `eye` (0 = left) through the engine.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eyecam_process(
    h: *mut Squint,
    eye: i32,
    img: *const u8,
    pitch_deg: f64,
    valve_open: f64,
    t: f64,
    out: *mut EyecamOut,
) {
    let s = unsafe { &mut *h };
    let img = unsafe { std::slice::from_raw_parts(img, 400 * 400) };
    let o = s.process((eye != 0) as usize, img, pitch_deg, valve_open, t);
    if let Some(out) = unsafe { out.as_mut() } {
        *out = EyecamOut { squint: o.eye_squint as f32, valid: o.valid as u8, closed: o.closed as u8 };
    }
}

/// 1 when both eyes' wear baselines are ready.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eyecam_ready(h: *const Squint) -> i32 {
    unsafe { &*h }.engines.iter().all(EyeEngine::baseline_ready) as i32
}

/// 1 when user parameters are loaded.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eyecam_calibrated(h: *const Squint) -> i32 {
    unsafe { &*h }.calib.user.is_some() as i32
}

/// 1 when this wear has a wear calibration on both eyes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eyecam_wear_calibrated(h: *const Squint) -> i32 {
    unsafe { &*h }.params.iter().all(|p| p.wear.is_some()) as i32
}

/// The loaded squint depth of eye `eye` (0 = left), or NaN.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eyecam_fsq(h: *const Squint, eye: i32) -> f64 {
    unsafe { &*h }.calib.user.map_or(f64::NAN, |u| u[(eye != 0) as usize].f_sq)
}

/// A new wear (the headset was off): the engines learn again and a running calibration is dropped.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eyecam_reset_wear(h: *mut Squint) {
    unsafe { &mut *h }.reset_wear();
}

/// Starts collecting a calibration: kind 0 wear, 1 user. Returns 0 for a user calibration without this wear's.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eyecam_calib_begin(h: *mut Squint, kind: i32) -> i32 {
    let s = unsafe { &mut *h };
    let kind = if kind == 0 { Kind::Wear } else { Kind::User };
    if kind == Kind::User && s.params.iter().any(|p| p.wear.is_none()) {
        return 0;
    }
    s.cancel();
    s.collect = Some(kind);
    1
}

/// A step of the running calibration: frames from `t0` + STEP_DELAY to `t0` + `seconds` are samples of `label`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eyecam_calib_step(h: *mut Squint, label: *const c_char, t0: f64, seconds: f64) {
    let s = unsafe { &mut *h };
    let label = unsafe { CStr::from_ptr(label) }.to_string_lossy();
    s.steps.push((Label::parse(&label), t0, seconds));
}

/// Fits the running calibration, uses it at once and saves the file. Returns `out.ok`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eyecam_calib_finish(h: *mut Squint, out: *mut EyecamCalibResult) -> i32 {
    let s = unsafe { &mut *h };
    let out = unsafe { &mut *out };
    out.f_sq = [f32::NAN; 2];
    match s.finish() {
        Ok((message, user)) => {
            if let Some(u) = user {
                out.f_sq = [u[0].f_sq as f32, u[1].f_sq as f32];
            }
            (out.ok, out.fail) = (1, 0);
            write_message(&mut out.message, &message);
        }
        Err(message) => {
            (out.ok, out.fail) = (0, fail_code(&message));
            write_message(&mut out.message, &message);
        }
    }
    out.ok
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn eyecam_calib_cancel(h: *mut Squint) {
    unsafe { &mut *h }.cancel();
}

/// Plays eyecam's beep for a step label ("end" for the end) when pw-play, paplay or aplay is there.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eyecam_beep(h: *mut Squint, label: *const c_char) {
    let s = unsafe { &mut *h };
    let label = unsafe { CStr::from_ptr(label) }.to_string_lossy();
    s.player.play(&label);
}
