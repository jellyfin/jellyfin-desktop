#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Each getter returns a heap-allocated, NUL-terminated UTF-8 path that the
// caller frees with jfn_paths_free(). The directory is created (mkdir -p)
// before the path is returned. Never returns NULL.
char* jfn_paths_config_dir(void);
char* jfn_paths_cache_dir(void);
char* jfn_paths_log_dir(void);
char* jfn_paths_log_path(void);
char* jfn_paths_mpv_home(void);

void jfn_paths_open_mpv_home(void);
void jfn_paths_free(char* s);

#ifdef __cplusplus
}
#endif
