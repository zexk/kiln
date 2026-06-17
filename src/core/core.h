#pragma once

#include <stddef.h>

void core_init(void);

/* Resolve a resource directory at runtime into `out` (capacity `cap`). Priority:
   the $<env_var> override; else <executable_dir>/../share/kiln/<name> when it
   exists (the installed layout, e.g. under a Nix store path); else `fallback`
   (the build-tree path baked at compile time, for running from a dev build).
   This keeps the dev workflow working while making an installed binary find its
   shaders/assets relative to itself. */
void core_resource_dir(char *out, size_t cap, const char *env_var,
                       const char *name, const char *fallback);
