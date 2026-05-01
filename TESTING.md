# Testing vsql_trgm

## Required environment variables

| Variable | Description | Where to find it |
|---|---|---|
| `VillageSQL_BUILD_DIR` | Path to your VillageSQL build directory | The directory where you ran `cmake` and `make` for VillageSQL |

Example:
```bash
export VillageSQL_BUILD_DIR=~/build/villagesql   # macOS
export VillageSQL_BUILD_DIR=$HOME/build/villagesql  # Linux
```

## Build and install before running tests

The extension must be installed in the server before MTR can run the tests.

```bash
# Build
./build.sh

# Install VEB to build tree
cd build && make install

# Load into running server
mysql -u root -e "INSTALL EXTENSION vsql_trgm;"
```

## Run the full test suite

Run from the server's `mysql-test/` directory:

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

MTR fails if any test output differs from its `.result` file.

## Regenerate result files

If you change the implementation and test output changes legitimately, regenerate:

```bash
perl mysql-test-run.pl --suite=/path/to/vsql_trgm/mysql-test --record
```

Inspect the diff before committing — `--record` overwrites `.result` files unconditionally.

## Test files

| File | What it covers |
|---|---|
| `t/vsql_trgm_similarity.test` | `trgm_show`, `trgm_similarity`, `trgm_distance` — trigram extraction, Dice coefficient, NULL handling, case-insensitivity, edge cases |
| `t/vsql_trgm_threshold.test` | `trgm_similar`, `trgm_similar_threshold` — default and explicit threshold, out-of-range threshold error behavior |
| `t/vsql_trgm_word.test` | `trgm_word_similarity`, `trgm_word_distance`, `trgm_word_similar` — word-level similarity, NULL handling |
| `t/vsql_trgm_strict.test` | `trgm_strict_word_similarity`, `trgm_strict_word_distance`, `trgm_strict_word_similar` — word-boundary-aligned similarity |
