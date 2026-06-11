#[derive(Default)]
pub struct SignalGuard;

impl SignalGuard {
    #[must_use]
    pub fn new() -> Self {
        Self
    }
}
