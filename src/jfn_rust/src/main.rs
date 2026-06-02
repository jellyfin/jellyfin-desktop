//! Process entry point. Forwards into [`jfn_rust::app::jfn_app_main`],
//! which owns the full boot/run/shutdown sequence (CEF subprocess
//! dispatch, settings load, platform install, mpv boot, browser run
//! loop, teardown).

fn main() {
    // Panic hook: route panics through tracing so they land in the same log
    // file as everything else (stderr is not captured by `just run` on Windows).
    let default_hook = std::panic::take_hook();
    std::panic::set_hook(Box::new(move |info| {
        let bt = std::backtrace::Backtrace::force_capture();
        tracing::error!(target: "panic", "PANIC: {info}\n{bt}");
        eprintln!("PANIC: {info}\n{bt}");
        default_hook(info);
    }));

    // jfn_app_main reads the OS argv itself (clap for the browser process,
    // CEF's own arg reader for subprocesses), so nothing is threaded through.
    std::process::exit(jfn_rust::app::jfn_app_main());
}
