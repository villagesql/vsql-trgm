/* Copyright (c) 2026 VillageSQL Contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 */

#include <villagesql/vsql.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "unicode_tables.h"

using namespace vsql;

// =============================================================================
// Core trigram algorithm
// =============================================================================

// Length in bytes of the well-formed UTF-8 character at s[i], or 0 when the
// bytes there are not one (bad lead byte, missing or bad continuation byte,
// overlong form, surrogate, or above U+10FFFF). Never reads past s.size().
static size_t utf8_char_len(std::string_view s, size_t i, char32_t* cp) {
  auto b = [&](size_t k) { return static_cast<unsigned char>(s[i + k]); };
  unsigned char c = b(0);
  size_t len;
  char32_t v, min;
  if (c < 0x80) { *cp = c; return 1; }
  if (c >= 0xC2 && c <= 0xDF) { len = 2; v = c & 0x1F; min = 0x80; }
  else if (c >= 0xE0 && c <= 0xEF) { len = 3; v = c & 0x0F; min = 0x800; }
  else if (c >= 0xF0 && c <= 0xF4) { len = 4; v = c & 0x07; min = 0x10000; }
  else return 0;
  if (s.size() - i < len) return 0;
  for (size_t k = 1; k < len; ++k) {
    if ((b(k) & 0xC0) != 0x80) return 0;
    v = (v << 6) | (b(k) & 0x3F);
  }
  if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) return 0;
  *cp = v;
  return len;
}

// A word character is a letter (general category L*), a combining mark (Mn,
// Mc) other than a variation selector, or a decimal digit (Nd). Marks keep
// Thai, Devanagari and decomposed (NFD) words whole.
static bool is_word_char(char32_t cp) {
  auto it = std::upper_bound(
      std::begin(kWordRanges), std::end(kWordRanges), cp,
      [](char32_t c, const CpRange& r) { return c < r.lo; });
  return it != std::begin(kWordRanges) && cp <= std::prev(it)->hi;
}

// Simple (one-to-one) Unicode lowercase mapping.
static char32_t to_lower(char32_t cp) {
  auto it = std::upper_bound(
      std::begin(kLowerRuns), std::end(kLowerRuns), cp,
      [](char32_t c, const LowerRun& r) { return c < r.lo; });
  if (it == std::begin(kLowerRuns)) return cp;
  const LowerRun& r = *std::prev(it);
  if (cp > r.hi || (cp - r.lo) % r.stride != 0) return cp;
  return static_cast<char32_t>(static_cast<int32_t>(cp) + r.delta);
}

static void append_utf8(std::string& out, char32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// Splits UTF-8 input into words of word characters, lowercasing each one,
// pads each word with two leading spaces and one trailing space, then calls
// on_word with the padded word as a list of characters (each one a UTF-8
// string). Bytes that are not well-formed UTF-8 act as separators. Shared
// tokenization path for all three extraction functions below.
template <typename F>
static void for_each_word(std::string_view input, F on_word) {
  std::vector<std::string> word;
  auto flush = [&]() {
    if (word.empty()) return;
    word.insert(word.begin(), 2, " ");
    word.push_back(" ");
    on_word(word);
    word.clear();
  };
  for (size_t i = 0; i < input.size();) {
    char32_t cp = 0;
    size_t len = utf8_char_len(input, i, &cp);
    if (len == 0) { flush(); ++i; continue; }
    if (is_word_char(cp)) {
      std::string ch;
      append_utf8(ch, to_lower(cp));
      word.push_back(std::move(ch));
    } else {
      flush();
    }
    i += len;
  }
  flush();
}

// Calls on_trigram for each run of three consecutive characters in each padded
// word, in order.
template <typename F>
static void for_each_trigram(std::string_view input, F on_trigram) {
  for_each_word(input, [&](const std::vector<std::string>& padded) {
    for (size_t i = 0; i + 2 < padded.size(); ++i)
      on_trigram(padded[i] + padded[i + 1] + padded[i + 2]);
  });
}

// Sorted, deduplicated trigram set.
static std::set<std::string> extract_trigrams(std::string_view input) {
  std::set<std::string> result;
  for_each_trigram(input, [&](std::string t) { result.insert(std::move(t)); });
  return result;
}

// Ordered trigram sequence (preserves word order and inter-word duplicates).
static std::vector<std::string> extract_ordered_trigrams(std::string_view input) {
  std::vector<std::string> result;
  for_each_trigram(input, [&](std::string t) { result.push_back(std::move(t)); });
  return result;
}

// |A ∩ B| / |A ∪ B| — pg_trgm's CALCSML formula (trgm.h defines DIVUNION).
static double sml(size_t shared, size_t len1, size_t len2) {
  return static_cast<double>(shared) / (len1 + len2 - shared);
}

static double calc_similarity(const std::set<std::string>& a,
                               const std::set<std::string>& b) {
  if (a.empty() && b.empty()) return 0.0;
  size_t shared = 0;
  for (const auto& t : a) {
    if (b.count(t)) ++shared;
  }
  return sml(shared, a.size(), b.size());
}

// word_similarity: max over all contiguous windows of seq2 of
//   |t1 ∩ window| / |t1 ∪ window|
// Window grows monotonically left-to-right from each start position i;
// matched is maintained incrementally to avoid recounting per step.
static double word_sim(const std::set<std::string>& t1,
                        const std::vector<std::string>& seq2) {
  if (t1.empty() || seq2.empty()) return 0.0;
  double best = 0.0;
  size_t n = seq2.size();
  for (size_t i = 0; i < n; ++i) {
    std::set<std::string> window;
    size_t matched = 0;
    for (size_t j = i; j < n; ++j) {
      if (window.insert(seq2[j]).second && t1.count(seq2[j])) ++matched;
      double sim = sml(matched, t1.size(), window.size());
      if (sim > best) best = sim;
    }
  }
  return best;
}

// Per-trigram word boundary flags for strict_word_similarity.
struct WordBoundaries {
  std::vector<bool> is_word_start;
  std::vector<bool> is_word_end;
};

static WordBoundaries compute_word_boundaries(std::string_view input) {
  std::vector<std::pair<size_t, size_t>> word_ranges;
  size_t idx = 0;
  for_each_word(input, [&](const std::vector<std::string>& padded) {
    size_t count = padded.size() >= 3 ? padded.size() - 2 : 0;
    if (count > 0) {
      word_ranges.push_back({idx, idx + count - 1});
      idx += count;
    }
  });
  WordBoundaries wb;
  wb.is_word_start.resize(idx, false);
  wb.is_word_end.resize(idx, false);
  for (const auto& [start, end] : word_ranges) {
    wb.is_word_start[start] = true;
    wb.is_word_end[end] = true;
  }
  return wb;
}

// strict_word_similarity: same as word_sim but windows must start and end at
// word-trigram boundaries.
static double strict_word_sim(const std::set<std::string>& t1,
                               const std::vector<std::string>& seq2,
                               const WordBoundaries& wb) {
  if (t1.empty() || seq2.empty()) return 0.0;
  double best = 0.0;
  size_t n = seq2.size();
  for (size_t i = 0; i < n; ++i) {
    if (!wb.is_word_start[i]) continue;
    std::set<std::string> window;
    size_t matched = 0;
    for (size_t j = i; j < n; ++j) {
      if (window.insert(seq2[j]).second && t1.count(seq2[j])) ++matched;
      if (!wb.is_word_end[j]) continue;
      double sim = sml(matched, t1.size(), window.size());
      if (sim > best) best = sim;
    }
  }
  return best;
}

// =============================================================================
// trgm_show(text) -> STRING  (sorted JSON array of trigrams)
// =============================================================================

void trgm_show_impl(StringArg arg, StringResult result) {
  try {
    if (arg.is_null()) { result.set_null(); return; }

    auto trgms = extract_trigrams(arg.value());

    std::string out = "[";
    bool first = true;
    for (const auto& t : trgms) {
      if (!first) out += ',';
      first = false;
      out += '"';
      // Trigrams hold only spaces and well-formed UTF-8 word characters
      // (is_word_char), so no character needs a JSON escape.
      out += t;
      out += '"';
    }
    out += ']';

    auto buf = result.buffer();
    if (out.size() > buf.size()) {
      // Report the true (unclamped) length so the server's dispatcher grows
      // the buffer and retries once with enough room; only copy the prefix
      // that fits this time, since the server does not read it on the
      // overflowing attempt.
      memcpy(buf.data(), out.data(), buf.size());
      result.set_length(out.size());
      return;
    }
    memcpy(buf.data(), out.data(), out.size());
    result.set_length(out.size());
  } catch (...) {
    result.warning("trgm_show: internal error");
  }
}

// =============================================================================
// trgm_similarity(text, text) -> REAL
// =============================================================================

void trgm_similarity_impl(StringArg a, StringArg b, RealResult result) {
  try {
    if (a.is_null() || b.is_null()) { result.set_null(); return; }
    auto ta = extract_trigrams(a.value());
    auto tb = extract_trigrams(b.value());
    result.set(calc_similarity(ta, tb));
  } catch (...) {
    result.warning("trgm_similarity: internal error");
  }
}

// =============================================================================
// trgm_distance(text, text) -> REAL   (1 - similarity)
// =============================================================================

void trgm_distance_impl(StringArg a, StringArg b, RealResult result) {
  try {
    if (a.is_null() || b.is_null()) { result.set_null(); return; }
    auto ta = extract_trigrams(a.value());
    auto tb = extract_trigrams(b.value());
    result.set(1.0 - calc_similarity(ta, tb));
  } catch (...) {
    result.warning("trgm_distance: internal error");
  }
}

// =============================================================================
// trgm_similar(text, text) -> INT  (1 if similarity >= 0.3, else 0)
// =============================================================================

void trgm_similar_impl(StringArg a, StringArg b, IntResult result) {
  try {
    if (a.is_null() || b.is_null()) { result.set_null(); return; }
    auto ta = extract_trigrams(a.value());
    auto tb = extract_trigrams(b.value());
    result.set(calc_similarity(ta, tb) >= 0.3 ? 1 : 0);
  } catch (...) {
    result.warning("trgm_similar: internal error");
  }
}

// =============================================================================
// trgm_similar_threshold(text, text, real) -> INT
// =============================================================================

void trgm_similar_threshold_impl(StringArg a, StringArg b, RealArg threshold,
                                  IntResult result) {
  try {
    if (a.is_null() || b.is_null() || threshold.is_null()) {
      result.set_null(); return;
    }
    if (threshold.value() < 0.0 || threshold.value() > 1.0) {
      result.warning("trgm_similar_threshold: threshold must be between 0 and 1");
      return;
    }
    auto ta = extract_trigrams(a.value());
    auto tb = extract_trigrams(b.value());
    result.set(calc_similarity(ta, tb) >= threshold.value() ? 1 : 0);
  } catch (...) {
    result.warning("trgm_similar_threshold: internal error");
  }
}

// =============================================================================
// trgm_word_similarity(text, text) -> REAL
// =============================================================================

void trgm_word_similarity_impl(StringArg a, StringArg b, RealResult result) {
  try {
    if (a.is_null() || b.is_null()) { result.set_null(); return; }
    auto t1 = extract_trigrams(a.value());
    auto seq2 = extract_ordered_trigrams(b.value());
    result.set(word_sim(t1, seq2));
  } catch (...) {
    result.warning("trgm_word_similarity: internal error");
  }
}

// =============================================================================
// trgm_word_distance(text, text) -> REAL
// =============================================================================

void trgm_word_distance_impl(StringArg a, StringArg b, RealResult result) {
  try {
    if (a.is_null() || b.is_null()) { result.set_null(); return; }
    auto t1 = extract_trigrams(a.value());
    auto seq2 = extract_ordered_trigrams(b.value());
    result.set(1.0 - word_sim(t1, seq2));
  } catch (...) {
    result.warning("trgm_word_distance: internal error");
  }
}

// =============================================================================
// trgm_strict_word_similarity(text, text) -> REAL
// =============================================================================

void trgm_strict_word_similarity_impl(StringArg a, StringArg b, RealResult result) {
  try {
    if (a.is_null() || b.is_null()) { result.set_null(); return; }
    auto sv_b = b.value();
    auto t1 = extract_trigrams(a.value());
    auto seq2 = extract_ordered_trigrams(sv_b);
    auto wb = compute_word_boundaries(sv_b);
    result.set(strict_word_sim(t1, seq2, wb));
  } catch (...) {
    result.warning("trgm_strict_word_similarity: internal error");
  }
}

// =============================================================================
// trgm_strict_word_distance(text, text) -> REAL
// =============================================================================

void trgm_strict_word_distance_impl(StringArg a, StringArg b, RealResult result) {
  try {
    if (a.is_null() || b.is_null()) { result.set_null(); return; }
    auto sv_b = b.value();
    auto t1 = extract_trigrams(a.value());
    auto seq2 = extract_ordered_trigrams(sv_b);
    auto wb = compute_word_boundaries(sv_b);
    result.set(1.0 - strict_word_sim(t1, seq2, wb));
  } catch (...) {
    result.warning("trgm_strict_word_distance: internal error");
  }
}

// =============================================================================
// trgm_word_similar(text, text) -> INT  (1 if word_similarity >= 0.6, else 0)
// =============================================================================

void trgm_word_similar_impl(StringArg a, StringArg b, IntResult result) {
  try {
    if (a.is_null() || b.is_null()) { result.set_null(); return; }
    auto t1 = extract_trigrams(a.value());
    auto seq2 = extract_ordered_trigrams(b.value());
    result.set(word_sim(t1, seq2) >= 0.6 ? 1 : 0);
  } catch (...) {
    result.warning("trgm_word_similar: internal error");
  }
}

// =============================================================================
// trgm_strict_word_similar(text, text) -> INT
// =============================================================================

void trgm_strict_word_similar_impl(StringArg a, StringArg b, IntResult result) {
  try {
    if (a.is_null() || b.is_null()) { result.set_null(); return; }
    auto sv_b = b.value();
    auto t1 = extract_trigrams(a.value());
    auto seq2 = extract_ordered_trigrams(sv_b);
    auto wb = compute_word_boundaries(sv_b);
    result.set(strict_word_sim(t1, seq2, wb) >= 0.5 ? 1 : 0);
  } catch (...) {
    result.warning("trgm_strict_word_similar: internal error");
  }
}

// =============================================================================
// Extension registration
// =============================================================================

VEF_GENERATE_ENTRY_POINTS(
  make_extension()
    // max_result_length raises the extension to VEF_PROTOCOL_5, from which the
    // server passes every STRING argument as utf8mb4. The JSON grows with the
    // input and has no fixed cap, so declare the server's ceiling.
    .func(make_func<&trgm_show_impl>("trgm_show")
      .returns(STRING).max_result_length(VEF_MAX_RESULT_LENGTH)
      .param(STRING).buffer_size(4096).deterministic().build())
    .func(make_func<&trgm_similarity_impl>("trgm_similarity")
      .returns(REAL).param(STRING).param(STRING).deterministic().build())
    .func(make_func<&trgm_distance_impl>("trgm_distance")
      .returns(REAL).param(STRING).param(STRING).deterministic().build())
    .func(make_func<&trgm_similar_impl>("trgm_similar")
      .returns(INT).param(STRING).param(STRING).deterministic().build())
    .func(make_func<&trgm_similar_threshold_impl>("trgm_similar_threshold")
      .returns(INT).param(STRING).param(STRING).param(REAL).deterministic().build())
    .func(make_func<&trgm_word_similarity_impl>("trgm_word_similarity")
      .returns(REAL).param(STRING).param(STRING).deterministic().build())
    .func(make_func<&trgm_word_distance_impl>("trgm_word_distance")
      .returns(REAL).param(STRING).param(STRING).deterministic().build())
    .func(make_func<&trgm_word_similar_impl>("trgm_word_similar")
      .returns(INT).param(STRING).param(STRING).deterministic().build())
    .func(make_func<&trgm_strict_word_similarity_impl>("trgm_strict_word_similarity")
      .returns(REAL).param(STRING).param(STRING).deterministic().build())
    .func(make_func<&trgm_strict_word_distance_impl>("trgm_strict_word_distance")
      .returns(REAL).param(STRING).param(STRING).deterministic().build())
    .func(make_func<&trgm_strict_word_similar_impl>("trgm_strict_word_similar")
      .returns(INT).param(STRING).param(STRING).deterministic().build())
)
