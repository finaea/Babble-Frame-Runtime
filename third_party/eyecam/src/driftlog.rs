//! The drift log: while it is on (`driftlog on` on ctl.sock, or `eyecam-rec --replay … --driftlog FILE`), one CSV row
//! per eye per second with what the live engine already worked out, to see over a long wear which of its references
//! drift: the pupil search window (fixed about 20 s into a wear), the open eye's box brightness the closed rule
//! compares with, the auto baseline, where the pupil is, and Valve's openness and pitch beside them. Numbers only, no
//! pictures; it reads what the engine computes and changes none of it.
//!
//! A row is the frames of one eye within one second of camera time (a new wear or the log closing ends one early):
//! shares are over its frames, positions and levels are medians over its frames, references are their values on its
//! last frame. The file is written through a buffer and closed when the log is turned off or eyecam-rec exits; past
//! CAP_BYTES it stops writing (`full` in status.json). Live, a thread of its own takes the frames and writes the file
//! (LiveLog), so the live worker never waits on the disk.

use crate::live::{self, EyeEngine, Out, Params, WearState};
use crate::vision;
use std::fs::{self, File};
use std::io::{BufWriter, Write};
use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, mpsc};
use std::thread::JoinHandle;

/// A file is not written past this many bytes (about a day at both eyes' rows a second).
pub const CAP_BYTES: u64 = 50 << 20;
/// The log's folder under the data folder (~/eyecam/drift).
pub const DIR_NAME: &str = "drift";

pub const HEADER: &str = "wall_unix,t_raw,eye,wear,wear_s,frames,\
x_min,x_max,y_min,y_max,x_min_src,x_min_set_s,x_min_frames,search_x_min,\
pupil_share,pupil_cx,pupil_cy,pupil_r,iris_r,edge_held_share,\
closed_share,box_f,box_open,box_open_n,box_ratio,box_ratio_max,\
skin_px,ap_px,base_skin_px,base_ap_px,widen_step_px,ready,\
lid,wide,squint,valve_open,valve_pitch";

/// One frame of one eye, as the drift log takes it.
#[derive(Clone, Copy, Debug)]
pub struct Probe {
    /// Frame time (camera clock, CLOCK_MONOTONIC_RAW s).
    pub t: f64,
    pub state: WearState,
    pub ok_pupil: bool,
    /// Pupil centre and semi-axis (px, upright frame; NaN without a pupil), and the iris radius in use (px).
    pub cx: f64,
    pub cy: f64,
    pub pupil_r: f64,
    pub iris_r: f64,
    pub edge_held: bool,
    pub closed: bool,
    /// The iris box's brightness (5-frame median).
    pub box_f: f64,
    /// Skin line above the pupil and aperture (px, 5-frame medians), and the widen step in use (px).
    pub skin_px: f64,
    pub ap_px: f64,
    pub step_px: f64,
    pub lid: f64,
    pub wide: f64,
    pub squint: f64,
    /// Valve's openness and gaze pitch (degrees) for this eye at this frame (NaN without them).
    pub valve_open: f64,
    pub valve_pitch: f64,
}

impl Probe {
    /// The frame `o` the engine just put out at `t` with `params`; `valve_open`: Valve's openness of that eye.
    pub fn of(engine: &EyeEngine, o: &Out, params: &Params, t: f64, valve_open: f64) -> Self {
        let (step_r, _, _) = live::levels(params);
        Self {
            t,
            state: engine.wear_state(),
            ok_pupil: o.f.ok_pupil,
            cx: o.f.pupil_cx,
            cy: o.f.pupil_cy,
            pupil_r: o.f.pupil_a,
            iris_r: o.r,
            edge_held: o.edge_held,
            closed: o.closed,
            box_f: o.box_f,
            skin_px: o.skin_up * o.r,
            ap_px: o.aperture * o.r,
            step_px: step_r * o.r,
            lid: o.eye_lid,
            wide: o.eye_wide,
            squint: o.eye_squint,
            valve_open,
            valve_pitch: o.pitch,
        }
    }

    /// Seconds since this wear's first frame.
    fn wear_s(&self) -> f64 {
        self.t - self.state.t_first
    }
}

/// The frames of one eye in the second being collected.
#[derive(Default)]
struct Second {
    t0: f64,
    last: Option<Probe>,
    n: u32,
    pupil: u32,
    edge: u32,
    closed: u32,
    cx: Vec<f64>,
    cy: Vec<f64>,
    pupil_r: Vec<f64>,
    iris_r: Vec<f64>,
    box_f: Vec<f64>,
    ratio: Vec<f64>,
    skin: Vec<f64>,
    ap: Vec<f64>,
    lid: Vec<f64>,
    wide: Vec<f64>,
    squint: Vec<f64>,
    valve_open: Vec<f64>,
    valve_pitch: Vec<f64>,
}

/// The median of the finite values (NaN without any).
fn med(v: &[f64]) -> f64 {
    let mut f: Vec<f64> = v.iter().copied().filter(|x| x.is_finite()).collect();
    vision::median(&mut f)
}

fn fmt(v: f64, places: usize) -> String {
    if v.is_finite() { format!("{v:.places$}") } else { String::new() }
}

impl Second {
    fn push(&mut self, p: &Probe) {
        if self.n == 0 {
            self.t0 = p.t;
        }
        self.n += 1;
        self.pupil += p.ok_pupil as u32;
        self.edge += p.edge_held as u32;
        self.closed += p.closed as u32;
        if p.ok_pupil {
            self.cx.push(p.cx);
            self.cy.push(p.cy);
            self.pupil_r.push(p.pupil_r);
        }
        self.iris_r.push(p.iris_r);
        self.box_f.push(p.box_f);
        if p.state.box_open > 0.0 {
            self.ratio.push(p.box_f / p.state.box_open);
        }
        self.skin.push(p.skin_px);
        self.ap.push(p.ap_px);
        self.lid.push(p.lid);
        self.wide.push(p.wide);
        self.squint.push(p.squint);
        self.valve_open.push(p.valve_open);
        self.valve_pitch.push(p.valve_pitch);
        self.last = Some(*p);
    }

    /// Whether `p` starts a new row: a second after this one's first frame, time going back, or another wear.
    fn ends_before(&self, p: &Probe) -> bool {
        self.last.is_some_and(|l| {
            p.t - self.t0 >= 1.0 || p.t < l.t || p.state.t_first.to_bits() != l.state.t_first.to_bits()
        })
    }

    /// This second's row (eye 0 = L, 1 = R; `wear`: the wear's number in this log), and start over.
    fn take_row(&mut self, eye: usize, wear: u32, wall_unix: f64) -> Option<String> {
        let l = self.last?;
        let n = self.n as f64;
        let s = &l.state;
        let ratio_max = self.ratio.iter().copied().filter(|x| x.is_finite()).fold(f64::NAN, f64::max);
        let row = [
            fmt(wall_unix, 3),
            fmt(l.t, 3),
            ["L", "R"][eye].to_string(),
            wear.to_string(),
            fmt(l.wear_s(), 1),
            self.n.to_string(),
            fmt(s.x_min, 0),
            fmt(s.xmax, 0),
            crate::feat::SEARCH_Y0.to_string(),
            crate::feat::SEARCH_Y1.to_string(),
            s.x_min_from.name().to_string(),
            fmt(s.x_min_at - s.t_first, 1),
            s.x_min_frames.to_string(),
            fmt(s.search_x_min, 0),
            fmt(self.pupil as f64 / n, 3),
            fmt(med(&self.cx), 1),
            fmt(med(&self.cy), 1),
            fmt(med(&self.pupil_r), 1),
            fmt(med(&self.iris_r), 1),
            fmt(self.edge as f64 / n, 3),
            fmt(self.closed as f64 / n, 3),
            fmt(med(&self.box_f), 1),
            fmt(s.box_open, 1),
            s.box_open_n.to_string(),
            fmt(med(&self.ratio), 3),
            fmt(ratio_max, 3),
            fmt(med(&self.skin), 1),
            fmt(med(&self.ap), 1),
            fmt(s.base_skin_px, 1),
            fmt(s.base_ap_px, 1),
            fmt(l.step_px, 1),
            (s.ready as u8).to_string(),
            fmt(med(&self.lid), 3),
            fmt(med(&self.wide), 3),
            fmt(med(&self.squint), 3),
            fmt(med(&self.valve_open), 3),
            fmt(med(&self.valve_pitch), 1),
        ];
        *self = Second::default();
        Some(row.join(","))
    }
}

/// What status.json says about the drift log (`driftlog`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct DriftStatus {
    pub on: bool,
    /// The file being written, or the last one (after `driftlog off`); "" before the first.
    pub path: String,
    pub rows: u64,
    pub bytes: u64,
    /// The file reached CAP_BYTES: nothing more is written to it (the log stays on until `driftlog off`).
    pub full: bool,
    /// Why writing stopped, if a write failed ("" otherwise).
    pub error: String,
}

impl DriftStatus {
    pub fn to_json(&self) -> String {
        format!(
            "{{\"on\":{},\"path\":{},\"rows\":{},\"bytes\":{},\"full\":{},\"error\":{}}}",
            self.on,
            crate::json::string(&self.path),
            self.rows,
            self.bytes,
            self.full,
            crate::json::string(&self.error)
        )
    }
}

/// One drift log file.
pub struct DriftLog<W: Write = BufWriter<File>> {
    path: PathBuf,
    out: Option<W>,
    cap: u64,
    bytes: u64,
    rows: u64,
    full: bool,
    error: String,
    eyes: [Second; 2],
    /// Each eye's wear number in this log (from 0; one more each time the engine starts a new wear).
    wear: [u32; 2],
}

impl DriftLog {
    /// A new file `drift_<stamp>.csv` in `dir` (made if missing, 0700; `_2`, `_3`, ... if that name is taken), its
    /// header written.
    pub fn create(dir: &Path, stamp: &str) -> Result<Self, String> {
        Self::create_as(dir, &format!("drift_{stamp}"), ".csv")
    }

    /// A new file for the panel's eye log `stem` (`eyes_YYYY-MM-DD_HH-MM-SS`, see is_eye_log_stem): `<stem>.eyecam.csv`
    /// in `dir`, made as `create` makes its files (`<stem>_2.eyecam.csv`, ... if that name is taken).
    pub fn create_named(dir: &Path, stem: &str) -> Result<Self, String> {
        Self::create_as(dir, stem, ".eyecam.csv")
    }

    /// A new file `<name><ext>` (or `<name>_<k><ext>`) in `dir`, mode 0600 (`dir` 0700, also one that was there before
    /// with a looser mode).
    fn create_as(dir: &Path, name: &str, ext: &str) -> Result<Self, String> {
        let made = !dir.exists();
        let at = |e: std::io::Error| format!("{}: {e}", dir.display());
        fs::create_dir_all(dir).map_err(at)?;
        let mode = fs::metadata(dir).map_err(at)?.permissions().mode() & 0o777;
        if made || mode & 0o077 != 0 {
            // Its mode set outright: the process umask is briefly 0177 while eyecam-rec binds a socket (bind_private)
            fs::set_permissions(dir, fs::Permissions::from_mode(0o700)).map_err(at)?;
        }
        for k in 1..100 {
            let file = if k == 1 { format!("{name}{ext}") } else { format!("{name}_{k}{ext}") };
            let path = dir.join(file);
            match File::options().write(true).create_new(true).mode(0o600).open(&path) {
                Ok(f) => {
                    // (and set outright too, whatever the umask)
                    f.set_permissions(fs::Permissions::from_mode(0o600)).map_err(|e| format!("{}: {e}", path.display()))?;
                    return Ok(Self::new(path, BufWriter::new(f), CAP_BYTES));
                }
                Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(e) => return Err(format!("{}: {e}", path.display())),
            }
        }
        Err(format!("{}: too many {name}*{ext}", dir.display()))
    }
}

/// Whether `stem` is a panel eye log's name as `driftlog on <stem>` takes it: `eyes_YYYY-MM-DD_HH-MM-SS`, digits where
/// the pattern has them and nothing else, so it can never name a path.
pub fn is_eye_log_stem(stem: &str) -> bool {
    const PATTERN: &[u8] = b"eyes_9999-99-99_99-99-99";
    stem.len() == PATTERN.len()
        && stem.bytes().zip(PATTERN).all(|(c, &p)| if p == b'9' { c.is_ascii_digit() } else { c == p })
}

impl<W: Write> DriftLog<W> {
    /// A log writing to `out` (the file at `path`), at most `cap` bytes; the header goes first.
    pub fn new(path: PathBuf, out: W, cap: u64) -> Self {
        let mut log =
            Self { path, out: Some(out), cap, bytes: 0, rows: 0, full: false, error: String::new(), eyes: Default::default(), wear: [0; 2] };
        log.write_line(HEADER);
        log
    }

    pub fn path(&self) -> &Path {
        &self.path
    }

    /// Take one frame of `eye` (0 = L, 1 = R); `wall_unix`: the wall clock now (NaN when not known, as in a replay).
    /// The second before it becomes a row once it is over.
    pub fn push(&mut self, eye: usize, p: &Probe, wall_unix: f64) {
        if self.eyes[eye].ends_before(p) {
            let new_wear = self.eyes[eye].last.is_some_and(|l| l.state.t_first.to_bits() != p.state.t_first.to_bits());
            self.end_row(eye, wall_unix);
            if new_wear {
                self.wear[eye] += 1;
            }
        }
        self.eyes[eye].push(p);
    }

    fn end_row(&mut self, eye: usize, wall_unix: f64) {
        if let Some(row) = self.eyes[eye].take_row(eye, self.wear[eye], wall_unix)
            && self.write_line(&row)
        {
            self.rows += 1;
        }
    }

    /// Write one line: false once the file is closed (full, failed or finished).
    fn write_line(&mut self, line: &str) -> bool {
        let Some(out) = self.out.as_mut() else { return false };
        let len = line.len() as u64 + 1;
        if self.bytes + len > self.cap {
            self.full = true;
            eprintln!("driftlog: {} reached {} MB, writing no more to it", self.path.display(), self.cap >> 20);
            self.close();
            return false;
        }
        match writeln!(out, "{line}") {
            Ok(()) => {
                self.bytes += len;
                true
            }
            Err(e) => {
                self.error = e.to_string();
                eprintln!("driftlog: {}: {e}", self.path.display());
                self.out = None;
                false
            }
        }
    }

    fn close(&mut self) {
        if let Some(mut out) = self.out.take()
            && let Err(e) = out.flush()
        {
            self.error = e.to_string();
            eprintln!("driftlog: {}: {e}", self.path.display());
        }
    }

    /// The seconds still being collected as (shorter) rows, then flush and close the file.
    pub fn finish(&mut self, wall_unix: f64) {
        for eye in 0..2 {
            self.end_row(eye, wall_unix);
        }
        self.close();
    }

    pub fn status(&self, on: bool) -> DriftStatus {
        DriftStatus {
            on,
            path: self.path.display().to_string(),
            rows: self.rows,
            bytes: self.bytes,
            full: self.full,
            error: self.error.clone(),
        }
    }
}

/// What the live worker hands a LiveLog's thread.
enum Msg {
    Frame(usize, Probe, f64),
    /// Turned off at this wall clock: the last part written and the file closed.
    Finish(f64),
    /// Answered once everything sent before it is done (tests).
    #[cfg(test)]
    Sync(mpsc::Sender<()>),
}

/// A drift log written by a thread of its own: the live worker only hands it each frame over a channel (under
/// Shared::driftlog's lock), and the thread does the rest, the file's writes and its closing among them, so a slow disk
/// never holds the worker up (held up, it drops camera frames once its queue of 16 is full).
pub struct LiveLog {
    path: PathBuf,
    tx: mpsc::Sender<Msg>,
    /// Its status, kept up by the thread.
    status: Arc<Mutex<DriftStatus>>,
    thread: JoinHandle<()>,
}

impl DriftLog {
    /// This log written by a thread of its own (see LiveLog).
    pub fn start(mut self) -> Result<LiveLog, String> {
        let path = self.path.clone();
        let status = Arc::new(Mutex::new(self.status(true)));
        let shared = status.clone();
        let (tx, rx) = mpsc::channel();
        let thread = std::thread::Builder::new()
            .name("eyecam-driftlog".into())
            .spawn(move || {
                // (what the status was last set from: rows, bytes, full, and whether it has an error)
                let mut seen = (self.rows, self.bytes, self.full, self.error.is_empty());
                let mut wall_end = f64::NAN;
                for msg in rx {
                    match msg {
                        Msg::Frame(eye, p, wall_unix) => self.push(eye, &p, wall_unix),
                        Msg::Finish(wall_unix) => {
                            wall_end = wall_unix;
                            break;
                        }
                        #[cfg(test)]
                        Msg::Sync(done) => {
                            let _ = done.send(());
                        }
                    }
                    let now = (self.rows, self.bytes, self.full, self.error.is_empty());
                    if now != seen {
                        seen = now;
                        *shared.lock().unwrap() = self.status(true);
                    }
                }
                // Turned off, or dropped without that (eyecam-rec ending): the last part written either way
                self.finish(wall_end);
                eprintln!("driftlog: closed {} ({} rows, {} bytes)", self.path.display(), self.rows, self.bytes);
                *shared.lock().unwrap() = self.status(false);
            })
            .map_err(|e| format!("{}: {e}", path.display()))?;
        Ok(LiveLog { path, tx, status, thread })
    }
}

/// A live log being closed by its thread: `wait` for it once Shared::driftlog's lock is let go (this waits on the
/// disk).
#[must_use = "wait for the file to be closed, after letting go of the lock"]
pub struct Closing(Option<JoinHandle<()>>);

impl Closing {
    pub fn wait(self) {
        if let Some(thread) = self.0 {
            let _ = thread.join();
        }
    }
}

/// The drift log of `eyecam-rec --serve`: turned on and off from ctl.sock, fed by the live worker. Nothing here waits
/// on the disk (see LiveLog), so the worker's frames never wait behind `on` / `off` either.
pub struct Control {
    /// Where files go (`<data folder>/drift`).
    pub dir: PathBuf,
    log: Option<LiveLog>,
    /// The status of the file being written, or of the last one once it was turned off.
    status: Arc<Mutex<DriftStatus>>,
}

impl Default for Control {
    fn default() -> Self {
        let home = std::env::var_os("HOME").map(PathBuf::from).unwrap_or_else(|| PathBuf::from("."));
        Self { dir: home.join("eyecam").join(DIR_NAME), log: None, status: Arc::default() }
    }
}

impl Control {
    pub fn is_on(&self) -> bool {
        self.log.is_some()
    }

    /// Start a new file (closing the one being written, if any): its path, and the closing of the one before.
    pub fn on(&mut self, log: LiveLog, wall_unix: f64) -> (PathBuf, Closing) {
        let before = self.off(wall_unix);
        eprintln!("driftlog: writing {}", log.path.display());
        let path = log.path.clone();
        self.status = log.status.clone();
        self.log = Some(log);
        (path, before)
    }

    /// Close the file being written (nothing when off): its closing.
    pub fn off(&mut self, wall_unix: f64) -> Closing {
        Closing(self.log.take().map(|log| {
            let _ = log.tx.send(Msg::Finish(wall_unix));
            log.thread
        }))
    }

    pub fn push(&mut self, eye: usize, p: &Probe, wall_unix: f64) {
        if let Some(log) = &self.log {
            let _ = log.tx.send(Msg::Frame(eye, *p, wall_unix));
        }
    }

    pub fn status(&self) -> DriftStatus {
        DriftStatus { on: self.is_on(), ..self.status.lock().unwrap().clone() }
    }

    /// Wait until the frames pushed so far are written (tests; with the worker running, let go of the lock first).
    #[cfg(test)]
    pub(crate) fn sync(&self) {
        if let Some(log) = &self.log {
            let (done, wait) = mpsc::channel();
            if log.tx.send(Msg::Sync(done)).is_ok() {
                let _ = wait.recv();
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::feat::XMinFrom;

    fn state(t_first: f64) -> WearState {
        WearState {
            t_first,
            x_min: 180.0,
            xmax: 346.0,
            search_x_min: 162.0,
            x_min_from: XMinFrom::Shade,
            x_min_at: t_first + 19.5,
            x_min_frames: 40,
            box_open: 50.0,
            box_open_n: 2700,
            base_skin_px: 60.0,
            base_ap_px: 110.0,
            ready: true,
        }
    }

    /// Frame `k` of a wear that began at `t_first`, 90 a second from `t0`: the pupil seen on even frames at x 240 + k
    /// (so the median moves), every 10th frame closed.
    fn probe(t0: f64, t_first: f64, k: u32) -> Probe {
        let ok = k.is_multiple_of(2);
        Probe {
            t: t0 + k as f64 / 90.0,
            state: state(t_first),
            ok_pupil: ok,
            cx: if ok { 240.0 + k as f64 } else { f64::NAN },
            cy: if ok { 200.0 } else { f64::NAN },
            pupil_r: if ok { 20.0 } else { f64::NAN },
            iris_r: 55.0,
            edge_held: k % 30 == 1,
            closed: k % 10 == 5,
            box_f: if k % 10 == 5 { 70.0 } else { 45.0 },
            skin_px: 61.0,
            ap_px: 112.0,
            step_px: 10.4,
            lid: 0.75,
            wide: 0.0,
            squint: 0.0,
            valve_open: 0.9,
            valve_pitch: -14.0,
        }
    }

    /// A writer the test keeps a handle on (what was written stays readable after the log closes it).
    #[derive(Clone, Default)]
    struct Sink(std::rc::Rc<std::cell::RefCell<Vec<u8>>>);

    impl Write for Sink {
        fn write(&mut self, b: &[u8]) -> std::io::Result<usize> {
            self.0.borrow_mut().extend_from_slice(b);
            Ok(b.len())
        }

        fn flush(&mut self) -> std::io::Result<()> {
            Ok(())
        }
    }

    impl Sink {
        fn text(&self) -> String {
            String::from_utf8(self.0.borrow().clone()).unwrap()
        }

        fn lines(&self) -> Vec<String> {
            self.text().lines().map(str::to_string).collect()
        }
    }

    fn log_with_cap(cap: u64) -> (DriftLog<Sink>, Sink) {
        let sink = Sink::default();
        (DriftLog::new(PathBuf::from("/x/drift_t.csv"), sink.clone(), cap), sink)
    }

    fn log() -> (DriftLog<Sink>, Sink) {
        log_with_cap(CAP_BYTES)
    }

    #[test]
    fn a_second_of_frames_is_one_row_per_eye() {
        let (mut log, sink) = log();
        for k in 0..90 {
            log.push(0, &probe(1000.0, 980.0, k), 1.7e9);
        }
        assert_eq!(sink.lines().len(), 1, "the second is not over yet");
        // The next second's first frame ends it
        log.push(0, &probe(1001.0, 980.0, 0), 1.7e9 + 1.0);
        let l = sink.lines();
        assert_eq!(l[0], HEADER);
        assert_eq!(l.len(), 2);
        let head: Vec<&str> = HEADER.split(',').collect();
        let row: Vec<&str> = l[1].split(',').collect();
        assert_eq!(row.len(), head.len());
        let get = |name: &str| row[head.iter().position(|h| *h == name).unwrap()];
        assert_eq!((get("wall_unix"), get("eye"), get("wear"), get("frames")), ("1700000001.000", "L", "0", "90"));
        assert_eq!((get("t_raw"), get("wear_s")), ("1000.989", "21.0"));
        assert_eq!((get("x_min"), get("x_max"), get("y_min"), get("y_max")), ("180", "346", "70", "320"));
        assert_eq!((get("x_min_src"), get("x_min_set_s"), get("x_min_frames"), get("search_x_min")), ("shade", "19.5", "40", "162"));
        // 45 of 90 frames with the pupil at 240, 242, ..., 328: median 284
        assert_eq!((get("pupil_share"), get("pupil_cx"), get("pupil_cy"), get("pupil_r"), get("iris_r")), ("0.500", "284.0", "200.0", "20.0", "55.0"));
        // Frames 1, 31, 61 held at the edge; 9 closed (5, 15, ..., 85)
        assert_eq!((get("edge_held_share"), get("closed_share")), ("0.033", "0.100"));
        assert_eq!((get("box_f"), get("box_open"), get("box_open_n"), get("box_ratio"), get("box_ratio_max")), ("45.0", "50.0", "2700", "0.900", "1.400"));
        assert_eq!((get("skin_px"), get("ap_px"), get("base_skin_px"), get("base_ap_px"), get("widen_step_px"), get("ready")), ("61.0", "112.0", "60.0", "110.0", "10.4", "1"));
        assert_eq!((get("lid"), get("wide"), get("squint"), get("valve_open"), get("valve_pitch")), ("0.750", "0.000", "0.000", "0.900", "-14.0"));
        assert_eq!(log.rows, 1);
    }

    #[test]
    fn eyes_are_kept_apart_and_a_new_wear_ends_a_row() {
        let (mut log, sink) = log();
        for k in 0..30 {
            log.push(0, &probe(1000.0, 980.0, k), f64::NAN);
            log.push(1, &probe(1000.0, 980.0, k), f64::NAN);
        }
        // The right eye's engine starts a new wear (both do, live): its row so far ends and the wear count goes up
        log.push(1, &probe(1015.0, 1015.0, 0), f64::NAN);
        let l = sink.lines();
        assert_eq!(l.len(), 2);
        assert!(l[1].starts_with(",1000.322,R,0,20.3,30,"), "{}", l[1]);
        // Closing writes what is left of each eye
        log.finish(f64::NAN);
        let l = sink.lines();
        assert_eq!(l.len(), 4);
        assert!(l[2].starts_with(",1000.322,L,0,20.3,30,"), "{}", l[2]);
        assert!(l[3].starts_with(",1015.000,R,1,0.0,1,"), "{}", l[3]);
        assert!(log.out.is_none(), "closed");
        // Nothing more after that
        log.push(0, &probe(1020.0, 980.0, 0), f64::NAN);
        log.finish(f64::NAN);
        assert_eq!((log.rows, sink.lines().len()), (3, 4));
    }

    #[test]
    fn no_pupil_and_no_reference_leave_cells_empty() {
        let (mut log, sink) = log();
        let mut p = probe(5.0, f64::NAN, 1);
        p.state.box_open = f64::NAN;
        p.state.ready = false;
        p.valve_open = f64::NAN;
        log.push(0, &p, f64::NAN);
        log.finish(f64::NAN);
        let row = sink.lines()[1].clone();
        assert_eq!(row, ",5.011,L,0,,1,180,346,70,320,shade,,40,162,0.000,,,,55.0,1.000,0.000,45.0,,2700,,,61.0,112.0,60.0,110.0,10.4,0,0.750,0.000,0.000,,-14.0");
    }

    #[test]
    fn it_stops_writing_at_the_cap() {
        let header = HEADER.len() as u64 + 1;
        let (mut log, sink) = log_with_cap(header + 500);
        for s in 0..10 {
            for k in 0..90 {
                log.push(0, &probe(1000.0 + s as f64, 980.0, k), f64::NAN);
            }
        }
        let st = log.status(true);
        assert!(st.full && st.on && st.bytes <= header + 500, "{st:?}");
        assert!(log.out.is_none(), "closed at the cap");
        let written = sink.text();
        assert_eq!(written.len() as u64, st.bytes);
        assert_eq!(written.lines().count() as u64, st.rows + 1);
        assert!(st.rows >= 1 && st.rows < 9, "{st:?}");
        assert!(st.to_json().contains("\"full\":true"), "{}", st.to_json());
    }

    #[test]
    fn files_are_new_each_time_and_closed_when_turned_off() {
        let dir = std::env::temp_dir().join(format!("eyecam-driftlog-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        let mut c = Control { dir: dir.join(DIR_NAME), ..Control::default() };
        assert_eq!(c.status(), DriftStatus::default());
        let start = |c: &Control, stamp: &str| DriftLog::create(&c.dir, stamp).unwrap().start().unwrap();
        let (a, closing) = c.on(start(&c, "2026-10-12_10-00-00"), 1.0);
        closing.wait();
        assert!(a.ends_with("drift/drift_2026-10-12_10-00-00.csv"), "{}", a.display());
        let mode = |p: &Path| fs::metadata(p).unwrap().permissions().mode() & 0o777;
        assert_eq!((mode(&c.dir), mode(&a)), (0o700, 0o600));
        for k in 0..100 {
            c.push(1, &probe(100.0, 90.0, k), 2.0);
        }
        c.sync();
        assert!(c.status().on && c.status().rows == 1, "{:?}", c.status());
        // `on` again in the same second: a second file, the first one closed with its last part
        let (b, closing) = c.on(start(&c, "2026-10-12_10-00-00"), 3.0);
        closing.wait();
        assert!(b.ends_with("drift_2026-10-12_10-00-00_2.csv"), "{}", b.display());
        let first = fs::read_to_string(&a).unwrap();
        assert_eq!(first.lines().count(), 3, "{first}");
        assert!(first.lines().nth(2).unwrap().starts_with("3.000,101.100,R,0,11.1,10,"), "{first}");
        c.off(4.0).wait();
        let st = c.status();
        assert!(!st.on && st.path == b.display().to_string() && st.rows == 0, "{st:?}");
        assert_eq!(fs::read_to_string(&b).unwrap(), format!("{HEADER}\n"));
        assert!(st.to_json().starts_with("{\"on\":false,\"path\":\""), "{}", st.to_json());
        fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn an_eye_log_stem_is_only_the_panels_name() {
        assert!(is_eye_log_stem("eyes_2026-10-12_14-03-09"));
        for bad in [
            "",
            "eyes_",
            "eyes_2026-10-12_14-03-0",
            "eyes_2026-10-12_14-03-090",
            "eyes_2026-10-12_14-03-09_2",
            "eyes_2026-10-12 14-03-09",
            "eyes_2026/10/12_14-03-09",
            "eyes_2026-1a-12_14-03-09",
            "drift_2026-10-12_14-03-09",
            "../eyes_2026-10-12_14-03",
            "eyes_２026-10-12_14-03-09",
        ] {
            assert!(!is_eye_log_stem(bad), "{bad}");
        }
    }

    /// `on` with the eye log's name: `<stem>.eyecam.csv`, `_2` before `.eyecam.csv` when taken; `on` again while on
    /// closes the first file.
    #[test]
    fn named_files_follow_the_eye_log() {
        let dir = std::env::temp_dir().join(format!("eyecam-driftlog-named-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        let mut c = Control { dir: dir.join(DIR_NAME), ..Control::default() };
        let start = |c: &Control| DriftLog::create_named(&c.dir, "eyes_2026-10-12_14-03-09").unwrap().start().unwrap();
        let (a, closing) = c.on(start(&c), 1.0);
        closing.wait();
        assert!(a.ends_with("drift/eyes_2026-10-12_14-03-09.eyecam.csv"), "{}", a.display());
        for k in 0..100 {
            c.push(0, &probe(100.0, 90.0, k), 2.0);
        }
        let (b, closing) = c.on(start(&c), 3.0);
        closing.wait();
        assert!(b.ends_with("drift/eyes_2026-10-12_14-03-09_2.eyecam.csv"), "{}", b.display());
        assert_eq!(fs::read_to_string(&a).unwrap().lines().count(), 3, "closed with its last part");
        assert!(c.status().on && c.status().path == b.display().to_string());
        c.off(4.0).wait();
        fs::remove_dir_all(&dir).unwrap();
    }

    /// A drift/ folder that was there before with a looser mode is made 0700; the files are 0600 whatever the umask
    /// (the usual 022 would give 0644).
    #[test]
    fn the_folder_and_files_are_private() {
        let dir = std::env::temp_dir().join(format!("eyecam-driftlog-mode-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        fs::set_permissions(&dir, fs::Permissions::from_mode(0o755)).unwrap();
        let log = DriftLog::create(&dir, "2026-10-12_10-00-00").unwrap();
        let mode = |p: &Path| fs::metadata(p).unwrap().permissions().mode() & 0o777;
        assert_eq!((mode(&dir), mode(log.path())), (0o700, 0o600));
        drop(log);
        // One that is already private is left as it is
        fs::set_permissions(&dir, fs::Permissions::from_mode(0o500)).unwrap();
        assert!(DriftLog::create(&dir, "x").is_err(), "not writable: refused");
        assert_eq!(mode(&dir), 0o500);
        fs::set_permissions(&dir, fs::Permissions::from_mode(0o700)).unwrap();
        fs::remove_dir_all(&dir).unwrap();
    }

    /// Live, the thread writes what the worker hands it: a frame path that only sends, the rows and the status as the
    /// file has them, and the last part written when the log is dropped without `off` (eyecam-rec ending).
    #[test]
    fn a_live_log_is_written_by_its_own_thread() {
        let dir = std::env::temp_dir().join(format!("eyecam-driftlog-live-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        let mut c = Control { dir: dir.join(DIR_NAME), ..Control::default() };
        let (path, closing) = c.on(DriftLog::create(&c.dir, "live").unwrap().start().unwrap(), 1.0);
        closing.wait();
        for s in 0..3 {
            for k in 0..90 {
                c.push(0, &probe(100.0 + s as f64, 90.0, k), 2.0);
            }
        }
        c.sync();
        let st = c.status();
        assert!(st.on && st.rows == 2 && st.path == path.display().to_string(), "{st:?}");
        // Dropped while on: the thread still writes the last second and closes the file
        let thread = c.log.take().map(|log| log.thread).unwrap();
        thread.join().unwrap();
        let text = fs::read_to_string(&path).unwrap();
        assert_eq!(text.lines().count(), 1 + 3, "{text}");
        assert!(text.lines().nth(3).unwrap().starts_with(",102.989,L,0,13.0,90,"), "{text}");
        let st = c.status();
        assert!(!st.on && st.rows == 3 && st.bytes == text.len() as u64, "{st:?}");
        fs::remove_dir_all(&dir).unwrap();
    }
}
