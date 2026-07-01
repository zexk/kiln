#pragma once

#include <stddef.h>

void core_init(void);

/* Log an out-of-memory error and abort. For allocations on the engine-internal
   hot path (ECS, renderer, voxel, platform, arena) where the caller has no
   sane recovery path. Allocations at a boundary that processes external/
   attacker-controlled input (file loaders, parsers) should instead check and
   fail gracefully.

   Unconditionally GNU-attributed (no __GNUC__/__clang__ guard): kiln only
   ever builds with gcc (native and mingw cross-compile), and a guard here
   confuses cppcheck's multi-configuration preprocessing into checking a
   branch where the attribute is stripped, which defeats its noreturn-aware
   null-deref analysis after CORE_CHECK_ALLOC. */
__attribute__((noreturn)) void core_oom_abort(const char *file, int line);

#define CORE_CHECK_ALLOC(p) \
    do { if (!(p)) core_oom_abort(__FILE__, __LINE__); } while (0)

/* Install signal handlers that print a backtrace (POSIX) or the signal number
   (Win32) to stderr before flushing the log and aborting.  Call once at
   startup, before any other initialisation. */
void core_install_crash_handler(void);

/* Resolve a resource directory at runtime into `out` (capacity `cap`). Priority:
   the $<env_var> override; else <executable_dir>/../share/kiln/<name> when it
   exists (the installed layout, e.g. under a Nix store path); else `fallback`
   (the build-tree path baked at compile time, for running from a dev build).
   This keeps the dev workflow working while making an installed binary find its
   shaders/assets relative to itself. */
void core_resource_dir(char *out, size_t cap, const char *env_var,
                       const char *name, const char *fallback);
