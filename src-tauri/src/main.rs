// The Rust side is deliberately thin: it launches the C++ engine (a sidecar process), forwards
// its JSON-lines output to the UI as events, and offers a few file helpers. All audio work
// happens in the engine (see ../native and ../docs/app-architecture.md).
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use std::{collections::HashMap, path::PathBuf, sync::{atomic::{AtomicBool, Ordering}, Mutex}};

use serde::Serialize;
use tauri::{path::BaseDirectory, AppHandle, Emitter, Manager, State};
use tauri_plugin_shell::{
    process::{CommandChild, CommandEvent},
    ShellExt,
};

#[derive(Default)]
struct Jobs(Mutex<HashMap<String, CommandChild>>);

#[derive(Clone, Serialize)]
struct EnginePayload {
    job_id: String,
    /// "line" (one JSON event from the engine), "stderr", or "exit".
    kind: String,
    data: String,
}

fn emit(app: &AppHandle, job_id: &str, kind: &str, data: String) {
    let _ = app.emit(
        "engine-event",
        EnginePayload { job_id: job_id.to_string(), kind: kind.to_string(), data },
    );
}

// ---- diagnostics log ----------------------------------------------------------------------------------
// A small text log (kept outside the scratch folder, so it survives Start again and quitting) that
// "Copy diagnostics" collects. It holds timings and states, never audio.
static LOG_START: std::sync::OnceLock<std::time::Instant> = std::sync::OnceLock::new();

/// A diagnostic build (made with SERMON_DIAG=1) also writes the log to the Desktop as it goes, plus a
/// heartbeat every few seconds, so the log can be read after the window has frozen.
fn diag() -> bool {
    option_env!("SERMON_DIAG") == Some("1")
}

fn desktop_log() -> Option<PathBuf> {
    if !diag() {
        return None;
    }
    let home = std::env::var("USERPROFILE").or_else(|_| std::env::var("HOME")).ok()?;
    Some(PathBuf::from(home).join("Desktop").join("Sermon Polish diagnostic log.txt"))
}

static LATEST_STATE: Mutex<String> = Mutex::new(String::new());
static STATE_COUNT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);

fn log_dir() -> PathBuf {
    std::env::temp_dir().join("sermon-polish-logs")
}

fn log_line_now(msg: &str) {
    use std::io::Write;
    let t = LOG_START.get_or_init(std::time::Instant::now).elapsed().as_secs_f64();
    let _ = std::fs::create_dir_all(log_dir());
    if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open(log_dir().join("app.log")) {
        let _ = writeln!(f, "[{t:9.2}s] {msg}");
    }
    if let Some(p) = desktop_log() {
        if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open(p) {
            let _ = writeln!(f, "[{t:9.2}s] {msg}");
        }
    }
}

fn start_log() {
    let dir = log_dir();
    let _ = std::fs::create_dir_all(&dir);
    let cur = dir.join("app.log");
    let _ = std::fs::rename(&cur, dir.join("app.previous.log"));
    if let Some(p) = desktop_log() {
        let _ = std::fs::rename(&p, p.with_file_name("Sermon Polish diagnostic log (previous run).txt"));
    }
    let cores = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(0);
    log_line_now(&format!(
        "Sermon Polish {} on {} {} with {} logical processors",
        env!("CARGO_PKG_VERSION"),
        std::env::consts::OS,
        std::env::consts::ARCH,
        cores
    ));
}

#[tauri::command]
fn log_line(msg: String) {
    log_line_now(&msg.chars().take(400).collect::<String>());
}

/// Everything worth sending with a bug report: the current and previous session logs.
#[tauri::command]
fn read_diagnostics() -> String {
    let tail = |name: &str| -> String {
        let text = std::fs::read_to_string(log_dir().join(name)).unwrap_or_default();
        let keep = text.len().saturating_sub(60_000);
        text[text.char_indices().map(|(i, _)| i).find(|i| *i >= keep).unwrap_or(0)..].to_string()
    };
    format!("=== this session ===\n{}\n=== previous session ===\n{}\n", tail("app.log"), tail("app.previous.log"))
}

type Spawned = (tauri::async_runtime::Receiver<CommandEvent>, CommandChild);

/// The helper programs bundled with the app (see `externalBin` in tauri.conf.json).
const SIDECARS: [&str; 3] = ["sermon_clean", "sermon_host", "ffmpeg"];

fn spawn_sidecar(app: &AppHandle, name: &str, args: Vec<String>) -> Result<Spawned, String> {
    if !SIDECARS.contains(&name) {
        return Err(format!("unknown helper program: {name}"));
    }
    let mut command = app
        .shell()
        .sidecar(name)
        .map_err(|e| format!("could not find the helper program {name}: {e}"))?
        .args(args);
    // The bundled DeepFilterNet model (the engine's built-in path points at the dev checkout).
    if name == "sermon_clean" || name == "sermon_host" {
        if let Ok(model) = app.path().resolve("resources/DeepFilterNet3_onnx.tar.gz", BaseDirectory::Resource) {
            if model.exists() {
                command = command.env("SERMON_DF_MODEL", model);
            }
        }
    }
    command.spawn().map_err(|e| format!("could not start {name}: {e}"))
}

/// Starts a helper program (`sermon_clean`, the audio engine, or `ffmpeg`, the file converter)
/// and returns immediately. Output arrives as `engine-event` events tagged with `job_id`: one per
/// line of stdout ("line"), chunks of stderr ("stderr"), and finally "exit" with the exit code.
#[tauri::command]
async fn run_sidecar(
    app: AppHandle,
    jobs: State<'_, Jobs>,
    job_id: String,
    sidecar: String,
    args: Vec<String>,
) -> Result<(), String> {
    let (mut rx, child) = spawn_sidecar(&app, &sidecar, args)?;
    log_line_now(&format!("started {sidecar} ({job_id})"));
    jobs.0.lock().unwrap().insert(job_id.clone(), child);

    let app2 = app.clone();
    tauri::async_runtime::spawn(async move {
        let mut buf = String::new();
        let mut code = -1;
        while let Some(event) = rx.recv().await {
            match event {
                CommandEvent::Stdout(bytes) => {
                    buf.push_str(&String::from_utf8_lossy(&bytes));
                    while let Some(pos) = buf.find('\n') {
                        let line: String = buf.drain(..=pos).collect();
                        let line = line.trim();
                        if !line.is_empty() {
                            // Position/level updates arrive many times a second; log only the real events.
                            if line.contains("\"event\":\"state\"") {
                                STATE_COUNT.fetch_add(1, Ordering::Relaxed);
                                if diag() {
                                    *LATEST_STATE.lock().unwrap() = line.chars().take(300).collect();
                                }
                            } else {
                                log_line_now(&format!("{job_id} out: {}", line.chars().take(200).collect::<String>()));
                            }
                            emit(&app2, &job_id, "line", line.to_string());
                        }
                    }
                }
                CommandEvent::Stderr(bytes) => {
                    let text = String::from_utf8_lossy(&bytes).to_string();
                    log_line_now(&format!("{job_id} err: {}", text.trim().chars().take(300).collect::<String>()));
                    emit(&app2, &job_id, "stderr", text)
                }
                CommandEvent::Terminated(payload) => {
                    code = payload.code.unwrap_or(-1);
                }
                _ => {}
            }
        }
        if !buf.trim().is_empty() {
            emit(&app2, &job_id, "line", buf.trim().to_string());
        }
        app2.state::<Jobs>().0.lock().unwrap().remove(&job_id);
        log_line_now(&format!("{job_id} exited with code {code}"));
        emit(&app2, &job_id, "exit", code.to_string());
    });
    Ok(())
}

/// Collects everything a helper prints (stdout and stderr) until it exits.
async fn run_to_end(app: &AppHandle, name: &str, args: Vec<String>) -> Result<(i32, String), String> {
    let (mut rx, _child) = spawn_sidecar(app, name, args)?;
    let (mut text, mut code) = (String::new(), -1);
    while let Some(event) = rx.recv().await {
        match event {
            CommandEvent::Stdout(b) | CommandEvent::Stderr(b) => text.push_str(&String::from_utf8_lossy(&b)),
            CommandEvent::Terminated(p) => code = p.code.unwrap_or(-1),
            _ => {}
        }
    }
    Ok((code, text))
}

/// Hidden check for the packaged app: `Sermon Polish --selftest <audio or video file>` runs the
/// helpers through the same code path as the UI, prints what it found, and exits (0 = healthy).
/// A file that isn't WAV/MP3 is converted with the bundled ffmpeg first, like the real app does.
fn selftest(app: AppHandle, file: String) {
    tauri::async_runtime::spawn(async move {
        let resources = app.path().resolve("resources", BaseDirectory::Resource).map(|p| p.to_string_lossy().to_string()).unwrap_or_default();
        println!("selftest: resources dir = {resources}");
        let mut ok = true;

        match run_to_end(&app, "ffmpeg", vec!["-hide_banner".into(), "-version".into()]).await {
            Ok((0, text)) if text.contains("ffmpeg version") => {
                println!("selftest: ffmpeg runs from inside the bundle: {}", text.lines().next().unwrap_or(""));
            }
            other => {
                println!("selftest: ffmpeg FAILED: {:?}", other.map(|(c, t)| (c, t.chars().take(200).collect::<String>())));
                ok = false;
            }
        }

        let lower = file.to_lowercase();
        let mut input = file.clone();
        if !(lower.ends_with(".wav") || lower.ends_with(".mp3")) {
            let out = std::env::temp_dir().join("sermon-polish-selftest.wav").to_string_lossy().to_string();
            let args: Vec<String> = ["-hide_banner", "-nostdin", "-y", "-i", file.as_str(), "-vn", "-map", "0:a:0", "-c:a", "pcm_s24le", out.as_str()]
                .iter().map(|s| s.to_string()).collect();
            match run_to_end(&app, "ffmpeg", args).await {
                Ok((0, _)) => {
                    println!("selftest: converted {file} to WAV with the bundled ffmpeg");
                    input = out;
                }
                other => {
                    println!("selftest: conversion FAILED: {:?}", other.map(|(c, t)| (c, t.chars().take(300).collect::<String>())));
                    ok = false;
                }
            }
        }

        let args = vec![
            input.clone(), "-".into(), "--seconds".into(), "30".into(), "--manual".into(), "--noise".into(), "normal".into(),
            "--dereverb".into(), "off".into(), "--json".into(),
        ];
        match spawn_sidecar(&app, "sermon_clean", args) {
            Err(e) => {
                println!("selftest: FAILED to start the engine: {e}");
                ok = false;
            }
            Ok((mut rx, _child)) => {
                let (mut progress, mut result) = (0, String::new());
                while let Some(event) = rx.recv().await {
                    match event {
                        CommandEvent::Stdout(bytes) => {
                            for line in String::from_utf8_lossy(&bytes).lines() {
                                if line.contains("\"event\":\"progress\"") {
                                    progress += 1;
                                } else if line.contains("\"event\":\"result\"") {
                                    result = line.to_string();
                                }
                            }
                        }
                        CommandEvent::Stderr(bytes) => println!("selftest: engine stderr: {}", String::from_utf8_lossy(&bytes)),
                        CommandEvent::Terminated(p) => println!("selftest: engine exited with code {:?}", p.code),
                        _ => {}
                    }
                }
                let model_ok = result.contains("\"noiseModelFound\":true") && result.contains(&resources);
                println!("selftest: {progress} progress events, result received: {}", !result.is_empty());
                println!("selftest: model found inside the app bundle: {model_ok}");
                ok = ok && progress > 0 && !result.is_empty() && model_ok;
            }
        }
        // The real-time engine: open the file on a silent device, play, and expect live state events.
        match spawn_sidecar(&app, "sermon_host", vec![]) {
            Err(e) => {
                println!("selftest: FAILED to start the real-time engine: {e}");
                ok = false;
            }
            Ok((mut rx, mut child)) => {
                let tmp = std::env::temp_dir();
                let q = |n: &str| tmp.join(n).to_string_lossy().replace('\\', "/");
                let open = format!(
                    "{{\"cmd\":\"open\",\"path\":\"{}\",\"work\":\"{}\",\"peaks\":\"{}\",\"nullDevice\":true}}\n",
                    input.replace('\\', "/"), q("sermon-selftest-work.wav"), q("sermon-selftest-work.spk")
                );
                let _ = child.write(open.as_bytes());
                let (mut opened, mut played, mut sent_play, mut rendered) = (false, false, false, false);
                let deadline = std::time::Instant::now() + std::time::Duration::from_secs(120);
                while let Some(event) = rx.recv().await {
                    if let CommandEvent::Stdout(bytes) = event {
                        for line in String::from_utf8_lossy(&bytes).lines() {
                            if line.contains("\"event\":\"opened\"") {
                                opened = true;
                            }
                            if opened && !sent_play {
                                let _ = child.write(b"{\"cmd\":\"play\"}\n");
                                sent_play = true;
                            }
                            if line.contains("\"playing\":true") && !line.contains("\"position\":0.00") && !played {
                                played = true;
                                let cmd = format!("{{\"cmd\":\"render\",\"out\":\"{}\",\"start\":0,\"end\":10}}\n", q("sermon-selftest-render.wav"));
                                let _ = child.write(cmd.as_bytes());
                            }
                            if line.contains("\"event\":\"rendered\"") {
                                rendered = true;
                            }
                        }
                    }
                    if rendered || std::time::Instant::now() > deadline {
                        break;
                    }
                }
                let _ = child.write(b"{\"cmd\":\"shutdown\"}\n");
                println!("selftest: real-time engine opened: {opened}, played: {played}, exported a render: {rendered}");
                ok = ok && opened && played && rendered;
            }
        }
        println!("selftest: {}", if ok { "PASS" } else { "FAIL" });
        app.exit(if ok { 0 } else { 1 });
    });
}

/// Sends one line to a running helper's stdin (the real-time engine takes commands this way).
#[tauri::command]
fn write_sidecar(jobs: State<'_, Jobs>, job_id: String, line: String) -> Result<(), String> {
    let mut jobs = jobs.0.lock().unwrap();
    let child = jobs.get_mut(&job_id).ok_or("the audio engine is not running")?;
    child.write(format!("{line}\n").as_bytes()).map_err(|e| e.to_string())
}

/// Stops a running engine job (used by Cancel).
#[tauri::command]
fn cancel_engine(jobs: State<'_, Jobs>, job_id: String) {
    if let Some(child) = jobs.0.lock().unwrap().remove(&job_id) {
        let _ = child.kill();
    }
}

/// A path inside the app's scratch folder (created on demand) for intermediate files.
#[tauri::command]
fn scratch_path(name: String) -> Result<String, String> {
    let dir: PathBuf = std::env::temp_dir().join("sermon-polish");
    std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    // Keep only the file name so a caller can't escape the scratch folder.
    let safe = PathBuf::from(name).file_name().map(|n| n.to_owned()).ok_or("bad file name")?;
    Ok(dir.join(safe).to_string_lossy().to_string())
}

/// Opens a web page in the default browser. Only this project's own GitHub pages are allowed.
#[tauri::command]
fn open_release_page(url: String) -> Result<(), String> {
    if !url.starts_with("https://github.com/Wrighteousmike/sermon-polish") {
        return Err("that address is not allowed".into());
    }
    #[cfg(target_os = "macos")]
    std::process::Command::new("open").arg(&url).spawn().map_err(|e| e.to_string())?;
    #[cfg(target_os = "windows")]
    {
        use std::os::windows::process::CommandExt;
        std::process::Command::new("rundll32").raw_arg(format!("url.dll,FileProtocolHandler {url}")).spawn().map_err(|e| e.to_string())?;
    }
    #[cfg(all(unix, not(target_os = "macos")))]
    std::process::Command::new("xdg-open").arg(&url).spawn().map_err(|e| e.to_string())?;
    Ok(())
}

/// Shows a file in the system file manager with the file selected: Finder on a Mac, Explorer on Windows.
#[tauri::command]
fn reveal_in_folder(path: String) -> Result<(), String> {
    let p = PathBuf::from(&path);
    if !p.exists() {
        return Err("that file is no longer there".into());
    }
    #[cfg(target_os = "macos")]
    {
        std::process::Command::new("open").arg("-R").arg(&p).spawn().map_err(|e| e.to_string())?;
    }
    #[cfg(target_os = "windows")]
    {
        use std::os::windows::process::CommandExt;
        // Explorer wants /select,"C:\path with spaces\file.mp3" as one argument, quotes included.
        std::process::Command::new("explorer")
            .raw_arg(format!("/select,\"{}\"", p.display()))
            .spawn()
            .map_err(|e| e.to_string())?;
    }
    #[cfg(all(unix, not(target_os = "macos")))]
    {
        let dir = p.parent().unwrap_or(&p);
        std::process::Command::new("xdg-open").arg(dir).spawn().map_err(|e| e.to_string())?;
    }
    Ok(())
}

/// Stops every running engine process, then deletes everything in the scratch folder. Used by
/// "Start again" and when the app quits, so no multi-gigabyte intermediate files are left behind.
fn clear_scratch_now(jobs: &Jobs) {
    let children: Vec<_> = jobs.0.lock().unwrap().drain().map(|(_, c)| c).collect();
    let had_engine = !children.is_empty();
    for child in children {
        let _ = child.kill();
    }
    if had_engine {
        // Windows will not delete a file a process still has open; give the engine a moment to exit.
        std::thread::sleep(std::time::Duration::from_millis(400));
    }
    let dir = std::env::temp_dir().join("sermon-polish");
    if let Ok(entries) = std::fs::read_dir(&dir) {
        for entry in entries.flatten() {
            let p = entry.path();
            let _ = if p.is_dir() { std::fs::remove_dir_all(&p) } else { std::fs::remove_file(&p) };
        }
    }
}

/// Set once the app has started quitting, so a second close request does not start the clean-up twice.
struct Quitting(AtomicBool);

/// Quitting: tell the window to show its "Cleaning up" dialog, delete the temporary files on a worker thread
/// (the window stays open meanwhile), then exit. The dialog stays up for at least a moment so it can be read.
fn begin_quit(app: &AppHandle) {
    if app.state::<Quitting>().0.swap(true, Ordering::SeqCst) {
        return;
    }
    let _ = app.emit("quitting", ());
    let handle = app.clone();
    std::thread::spawn(move || {
        let started = std::time::Instant::now();
        clear_scratch_now(&handle.state::<Jobs>());
        let least = std::time::Duration::from_millis(900);
        if started.elapsed() < least {
            std::thread::sleep(least - started.elapsed());
        }
        handle.exit(0);
    });
}

#[tauri::command]
fn clear_scratch(jobs: State<'_, Jobs>) {
    clear_scratch_now(&jobs);
}

#[tauri::command]
fn remove_scratch_file(path: String) {
    let scratch = std::env::temp_dir().join("sermon-polish");
    let p = PathBuf::from(&path);
    if p.starts_with(&scratch) {
        let _ = std::fs::remove_file(p);
    }
}

fn main() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_shell::init())
        .manage(Jobs::default())
        .manage(Quitting(AtomicBool::new(false)))
        .on_window_event(|window, event| {
            if let tauri::WindowEvent::CloseRequested { api, .. } = event {
                api.prevent_close();
                begin_quit(window.app_handle());
            }
        })
        .setup(|app| {
            start_log();
            if diag() {
                // Every 5 s: how many position updates the engine sent, and the latest one. If the window freezes
                // but these keep arriving, the engine is fine and it is the window that is stuck.
                std::thread::spawn(|| loop {
                    std::thread::sleep(std::time::Duration::from_secs(5));
                    let n = STATE_COUNT.swap(0, Ordering::Relaxed);
                    let last = LATEST_STATE.lock().unwrap().clone();
                    log_line_now(&format!("heartbeat: {n} engine updates in 5 s; last: {last}"));
                });
            }
            // Open as large as the Export screen needs, but never bigger than the screen can show.
            if let Some(win) = app.get_webview_window("main") {
                if let Ok(Some(m)) = win.current_monitor() {
                    let sf = m.scale_factor();
                    let (mw, mh) = (m.size().width as f64 / sf, m.size().height as f64 / sf);
                    let _ = win.set_size(tauri::LogicalSize::new(1280f64.min(mw * 0.94), 900f64.min(mh * 0.88)));
                    let _ = win.center();
                }
            }
            // Intermediate files from an earlier run (cancelled or crashed) are never needed again.
            if let Ok(entries) = std::fs::read_dir(std::env::temp_dir().join("sermon-polish")) {
                for entry in entries.flatten() {
                    let _ = std::fs::remove_file(entry.path());
                }
            }
            let args: Vec<String> = std::env::args().collect();
            if let Some(i) = args.iter().position(|a| a == "--selftest") {
                if let Some(file) = args.get(i + 1) {
                    selftest(app.handle().clone(), file.clone());
                }
            }
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![run_sidecar, write_sidecar, cancel_engine, scratch_path, remove_scratch_file, clear_scratch, reveal_in_folder, open_release_page, log_line, read_diagnostics])
        .build(tauri::generate_context!())
        .expect("error while building Sermon Polish")
        .run(|app, event| {
            // Quitting (window closed, Cmd+Q, ...): stop the engine and remove the scratch files.
            match event {
                // Cmd+Q / the system asking the app to quit (no exit code yet): do it through the clean-up dialog.
                tauri::RunEvent::ExitRequested { api, code: None, .. } => {
                    api.prevent_exit();
                    begin_quit(app);
                }
                tauri::RunEvent::Exit => clear_scratch_now(&app.state::<Jobs>()),
                _ => {}
            }
        });
}
