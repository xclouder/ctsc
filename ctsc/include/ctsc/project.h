#ifndef CTSC_PROJECT_H
#define CTSC_PROJECT_H

/*
 * `ctsc --project <path>` support.
 *
 * Reads a tsconfig.json (or the tsconfig.json inside the given directory),
 * enumerates source files honoring `include` / `exclude` globs, and emits
 * JavaScript to `outDir`. One level of `extends` is resolved.
 *
 * Scope matches the harness reference (harness/src/project.ts):
 *   - target / module ignored for the emitter (it already targets ES2020).
 *   - rootDir, outDir, include, exclude, extends: honored.
 *   - files / references / composite / paths: not yet honored (TODO M3+).
 */

#include "common.h"

typedef struct {
    const char* tsconfig_path;  /* absolute or cwd-relative path to tsconfig.json OR to its directory */
    const char* out_dir_override; /* optional CLI override for compilerOptions.outDir */
    bool        write_package_json;  /* emit dist/package.json {"type":"module"} when module=ES* */
    bool        verbose;
} CtscProjectOptions;

/*
 * Run the project pipeline. Returns 0 on success, non-zero on failure
 * (tsconfig parse error, missing rootDir, emit error, etc.).
 * Diagnostics are written to stderr.
 */
int ctsc_run_project(const CtscProjectOptions* opts);

#endif
