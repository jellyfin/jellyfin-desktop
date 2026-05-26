//! Pure, host-testable compositor bookkeeping shared by the macOS
//! (`CAMetalLayer`) and Windows (`DirectComposition`) backends.
//!
//! These backends are `#![cfg(target_os = ...)]` and can't be built on a
//! Linux dev machine, so the *logic* in them (transition gating and the
//! surface stack) used to be duplicated and could only be verified
//! on-platform. This crate holds that logic as plain value types with no
//! atomics, locks, or OS calls — the OS-bound compositor stores these inside
//! its own `Mutex`/`AtomicBool` and drives the GPU itself. The result is
//! unit-testable on any host.
//!
//! The two backends are not identical, so each type documents which entry
//! points each platform uses; the tests pin both platforms' exact
//! behavior.

pub mod stack;
pub mod transition;
