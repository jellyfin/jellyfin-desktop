// Tiny C++ entry-point shim. All logic lives in jfn-rust
// (see src/jfn_rust/src/app.rs). main() exists because CEF subprocesses
// re-execute this binary and dispatch through the standard C runtime;
// turning the staticlib into a Rust [[bin]] is a slice-5 concern.

extern "C" int jfn_app_main(int argc, const char* const* argv);

int main(int argc, char* argv[]) {
    return jfn_app_main(argc, const_cast<const char* const*>(argv));
}
