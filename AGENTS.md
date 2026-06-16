# AGENTS.md

This file provides guidance to AI coding assistants (Claude Code, Gemini Code Assist, etc.) when working with code in this repository.

**Note**: Also check `AGENTS.local.md` for additional local development instructions when present.

## Project Overview

`vsql-trgm` is a trigram-based text similarity extension for VillageSQL, inspired by PostgreSQL's `pg_trgm`. It computes similarity between strings using trigram overlap and is useful for fuzzy search, typo-tolerant matching, and ranked text results.

Install name (underscores): `vsql_trgm`
GitHub repo name (hyphens): `vsql-trgm`

## Build System

**Configure and Build:**
```bash
mkdir build && cd build
cmake .. -DVillageSQL_BUILD_DIR=/path/to/villagesql/build
make
```

**Install:**
```bash
make install
```

The build:
1. Uses `cmake/FindVillageSQL.cmake` to locate the VillageSQL Extension SDK via `VillageSQL_BUILD_DIR`
2. Compiles `src/vsql_trgm.cc` into a shared library
3. Packages it with `manifest.json` into `vsql_trgm.veb` via `VEF_CREATE_VEB()`
4. `make install` places the VEB into the build tree's `veb_output_directory`

**CMake Variables:**
- `VillageSQL_BUILD_DIR`: Path to VillageSQL build directory (required)

## Architecture

**Core Components:**
- `src/vsql_trgm.cc` — all trigram logic and VDF registration in one file
- `cmake/FindVillageSQL.cmake` — SDK discovery module
- `manifest.json` — extension metadata (`vsql_trgm`, version `0.0.2`)
- `mysql-test/t/` — MTR test files
- `mysql-test/r/` — expected MTR results

**Available Functions:**

*Similarity (whole-string):*
- `trgm_show(text)` — returns sorted JSON array of trigrams for the input
- `trgm_similarity(text, text)` — similarity score 0–1 (`|A ∩ B| / max(|A|, |B|)`)
- `trgm_distance(text, text)` — 1 − similarity
- `trgm_similar(text, text)` — 1 if similarity ≥ 0.3, else 0
- `trgm_similar_threshold(text, text, real)` — 1 if similarity ≥ threshold, else 0

*Word similarity (str1 vs any substring of str2):*
- `trgm_word_similarity(text, text)` — best match between str1 and any substring of str2
- `trgm_word_distance(text, text)` — 1 − word_similarity
- `trgm_word_similar(text, text)` — 1 if word_similarity ≥ 0.6, else 0
- `trgm_strict_word_similarity(text, text)` — like word_similarity but must align with word boundaries
- `trgm_strict_word_distance(text, text)` — 1 − strict_word_similarity
- `trgm_strict_word_similar(text, text)` — 1 if strict_word_similarity ≥ 0.5, else 0

All functions return NULL if any argument is NULL.

**No external dependencies** beyond the VillageSQL SDK — no OpenSSL, no system libs.

## VEF API

Uses Protocol V3 (`#include <villagesql/vsql.h>`, `using namespace vsql`). Typed wrappers:
- `StringArg` / `StringResult` for string parameters and return values
- `RealArg` / `RealResult` for floating-point parameters and return values
- `IntResult` for integer return values
- `out.set_null()` for NULL results
- `out.warning("msg")` for user-input validation errors (returns NULL, emits Warning 3200)

## Testing

MTR test suite — 4 test files:
- `vsql_trgm_similarity` — `trgm_show`, `trgm_similarity`, `trgm_distance`, NULL handling, edge cases
- `vsql_trgm_threshold` — `trgm_similar`, `trgm_similar_threshold`, out-of-range threshold
- `vsql_trgm_word` — `trgm_word_similarity`, `trgm_word_distance`, `trgm_word_similar`
- `vsql_trgm_strict` — `trgm_strict_word_similarity`, `trgm_strict_word_distance`, `trgm_strict_word_similar`

**Run tests (requires `make install` first):**
```bash
cd /path/to/villagesql/build/mysql-test
perl mysql-test-run.pl --suite=/path/to/vsql-trgm/mysql-test
```

**Re-record results:**
```bash
perl mysql-test-run.pl --suite=/path/to/vsql-trgm/mysql-test --record
```

## Known Limitations

- **No session-level threshold**: `set_limit`/`show_limit` require mutable session state; VEF functions are re-entrant with no session-scoped state. Use `trgm_similar_threshold(a, b, threshold)` instead.
- **No index support**: VEF has no index registration API. `WHERE trgm_similarity(col, 'x') > 0.3` requires a full table scan.
- **`trgm_show` returns JSON**: VEF VDFs return a single scalar; there is no array return type. `trgm_show` returns a JSON array string (`'["  c"," ca",...]'`).
