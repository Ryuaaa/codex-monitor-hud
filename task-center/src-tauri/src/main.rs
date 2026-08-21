fn main() {
    if std::env::args().any(|arg| arg == "--read-only-diagnostic") {
        std::process::exit(codex_monitor_task_center_lib::read_only_diagnostic());
    }
    if let Some(delay) = std::env::args().find_map(|arg| {
        arg.strip_prefix("--auto-close-ms=")
            .and_then(|value| value.parse::<u64>().ok())
            .filter(|value| (50..=5_000).contains(value))
    }) {
        std::thread::spawn(move || {
            std::thread::sleep(std::time::Duration::from_millis(delay));
            std::process::exit(0);
        });
    }
    codex_monitor_task_center_lib::run();
}
