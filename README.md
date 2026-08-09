# VillageSQL Trigram Extension

Trigram-based text similarity search for VillageSQL, inspired by PostgreSQL's `pg_trgm`.

Install name (underscores): `vsql_trgm`
GitHub repo name (hyphens): `vsql-trgm`

## What it does

Computes similarity between text strings using the trigram model. A trigram is a group of 3 consecutive characters. Two strings are similar if they share many trigrams.

```sql
INSTALL EXTENSION vsql_trgm;

-- Similarity score (0–1)
SELECT vsql_trgm.trgm_similarity('cat', 'car');      -- 0.5

-- Is above the default 0.3 threshold?
SELECT vsql_trgm.trgm_similar('cat', 'car');         -- 1

-- Find similar rows
SELECT name FROM products
WHERE vsql_trgm.trgm_similarity(name, 'adidas') > 0.3
ORDER BY vsql_trgm.trgm_similarity(name, 'adidas') DESC;
```

## Functions

### Similarity

| Function | PostgreSQL equivalent | Description |
|---|---|---|
| `trgm_show(text)` | `show_trgm(text)` | Returns sorted JSON array of trigrams |
| `trgm_similarity(text, text)` | `similarity(text, text)` | `\|A ∩ B\| / max(\|A\|, \|B\|)` similarity (0–1) |
| `trgm_distance(text, text)` | `text <-> text` | 1 − similarity |
| `trgm_similar(text, text)` | `text % text` | 1 if similarity ≥ 0.3, else 0 |
| `trgm_similar_threshold(text, text, real)` | `text % text` with custom limit | 1 if similarity ≥ threshold, else 0. `threshold` must be between 0 and 1; out of range returns NULL with a warning |

### Word Similarity

| Function | PostgreSQL equivalent | Description |
|---|---|---|
| `trgm_word_similarity(text, text)` | `word_similarity(text, text)` | Best match between str1 and any substring of str2 |
| `trgm_word_distance(text, text)` | `text <<-> text` | 1 − word_similarity |
| `trgm_word_similar(text, text)` | `text <% text` | 1 if word_similarity ≥ 0.6, else 0 |
| `trgm_strict_word_similarity(text, text)` | `strict_word_similarity(text, text)` | Like word_similarity but must align with word boundaries |
| `trgm_strict_word_distance(text, text)` | `text <->> text` | 1 − strict_word_similarity |
| `trgm_strict_word_similar(text, text)` | n/a | 1 if strict_word_similarity ≥ 0.5, else 0 |

All functions return `NULL` if any argument is `NULL`.

All functions are deterministic and can be used in generated columns and CHECK
constraints:

```sql
CREATE TABLE products (
  name         VARCHAR(255) NOT NULL,
  adidas_score REAL AS (trgm_similarity(name, 'adidas')) STORED,
  CHECK (trgm_similar_threshold(name, 'adidas', 0.1) IN (0, 1))
);
INSERT INTO products (name) VALUES ('adidas originals');
SELECT name, adidas_score FROM products;
-- adidas originals | 0.4117647058823529
```

## Prerequisites

- VillageSQL build directory
- CMake 3.16 or higher
- C++ compiler with C++17 support

📚 **Full Documentation**: [villagesql.com/docs](https://villagesql.com/docs)

## Installation

If you installed VillageSQL with the install script, the Docker image, or a
release tarball, `vsql_trgm.veb` is already in the server's `lib/veb/`
directory — this extension is bundled with the server. There is nothing to build
or download:

```sql
INSTALL EXTENSION vsql_trgm;
```

Build from source only if you built the server from source without the bundled
extensions, or if you are working on this extension itself.

## Building

Using the included `build.sh`:

```bash
export VillageSQL_BUILD_DIR=/path/to/villagesql/build
./build.sh
# Produces: build/vsql_trgm.veb
```

Or manually:

**Linux:**
```bash
mkdir build && cd build
cmake .. -DVillageSQL_BUILD_DIR=$HOME/build/villagesql
make -j$(nproc)
```

**macOS:**
```bash
mkdir build && cd build
cmake .. -DVillageSQL_BUILD_DIR="$HOME/build/villagesql"
make -j$(sysctl -n hw.logicalcpu)
```

## Installing

```bash
cd build
make install
```

Then load in VillageSQL:

```sql
INSTALL EXTENSION vsql_trgm;
```

To uninstall:

```sql
UNINSTALL EXTENSION vsql_trgm;
```

## Testing

### Option 1: Using installed VEB

**Linux:**
```bash
cd $HOME/build/villagesql/mysql-test
perl mysql-test-run.pl --suite=/path/to/vsql_trgm/mysql-test
```

**macOS:**
```bash
cd ~/build/villagesql/mysql-test
perl mysql-test-run.pl --suite=/path/to/vsql_trgm/mysql-test
```

### Option 2: Regenerate result files

```bash
perl mysql-test-run.pl --suite=/path/to/vsql_trgm/mysql-test --record
```

The extension must be installed before running tests.

## Known Limitations

### No session-level similarity threshold (`set_limit` / `show_limit`)

PostgreSQL's `set_limit(float4)` and `show_limit()` let you configure a per-session threshold for the `%` operator. VEF functions must be re-entrant with no global or session-scoped mutable state, so these cannot be implemented.

**Workaround:** Pass the threshold explicitly: `trgm_similar_threshold(a, b, 0.4)`. The two-argument `trgm_similar(a, b)` uses the PostgreSQL default of 0.3.

**What VEF would need:** A session variable API — a way for extension functions to read and write connection-scoped state.

### No index support

PostgreSQL's `pg_trgm` provides GIN and GiST operator classes that make `LIKE`, `ILIKE`, `~`, and `%` queries index-accelerated. VEF does not expose an index registration API.

**Consequence:** `WHERE trgm_similarity(col, 'pattern') > 0.3` requires a full table scan. For small to medium tables this is fine; for large tables it can be slow.

**What VEF would need:** A custom index registration API for GIN/GiST-compatible operator classes.

### `trgm_show` returns JSON, not a native array

PostgreSQL's `show_trgm(text)` returns `text[]`. VEF VDFs return a single scalar value; there is no array return type.

**Workaround:** `trgm_show(text)` returns a sorted JSON array string: `'["  c"," ca","at ","cat"]'`. Parse it with `JSON_TABLE` or application code.

**What VEF would need:** An array return type or table-valued function (set-returning function) API.

## Reporting Bugs and Requesting Features

Open an issue at [github.com/villagesql/vsql-trgm/issues](https://github.com/villagesql/vsql-trgm/issues). Include:
- Title and description of the problem
- Steps to reproduce (SQL statements, inputs, expected vs. actual output)
- VillageSQL version (`SHOW VARIABLES LIKE 'villagesql_server_version'`)

## Contact

- [Discord](https://discord.gg/KSr6whd3Fr)
- [GitHub Issues](https://github.com/villagesql/vsql-trgm/issues)
- [GitHub Discussions](https://github.com/villagesql/vsql-trgm/discussions)

## License

GPL-2.0. See the license header in source files for details.
