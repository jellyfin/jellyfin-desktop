pub struct SignalGuard {
    int_act: libc::sigaction,
    term_act: libc::sigaction,
}

impl Default for SignalGuard {
    fn default() -> Self {
        Self::new()
    }
}

impl SignalGuard {
    #[must_use]
    pub fn new() -> Self {
        let mut int_act: libc::sigaction = unsafe { std::mem::zeroed() };
        let mut term_act: libc::sigaction = unsafe { std::mem::zeroed() };
        unsafe {
            libc::sigaction(libc::SIGINT, std::ptr::null(), &mut int_act);
            libc::sigaction(libc::SIGTERM, std::ptr::null(), &mut term_act);
        }
        Self { int_act, term_act }
    }
}

impl Drop for SignalGuard {
    fn drop(&mut self) {
        unsafe {
            libc::sigaction(libc::SIGINT, &self.int_act, std::ptr::null_mut());
            libc::sigaction(libc::SIGTERM, &self.term_act, std::ptr::null_mut());
        }
    }
}
