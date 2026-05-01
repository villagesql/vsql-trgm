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

#include <villagesql/extension.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace villagesql::extension_builder;
using namespace villagesql::func_builder;

// =============================================================================
// Core trigram algorithm
// =============================================================================

// Lowercases input, splits on non-alphanumeric characters, pads each word with
// two leading spaces and one trailing space, then calls on_word for each padded
// word. Shared tokenization path for all three extraction functions below.
template <typename F>
static void for_each_word(std::string_view input, F on_word) {
  std::string word;
  word.reserve(64);
  auto flush = [&]() {
    if (word.empty()) return;
    on_word("  " + word + " ");
    word.clear();
  };
  for (unsigned char c : input) {
    if (std::isalnum(c)) word += static_cast<char>(std::tolower(c));
    else flush();
  }
  flush();
}

// Sorted, deduplicated trigram set.
static std::set<std::string> extract_trigrams(std::string_view input) {
  std::set<std::string> result;
  for_each_word(input, [&](const std::string& padded) {
    for (size_t i = 0; i + 2 < padded.size(); ++i)
      result.insert(padded.substr(i, 3));
  });
  return result;
}

// Ordered trigram sequence (preserves word order and inter-word duplicates).
static std::vector<std::string> extract_ordered_trigrams(std::string_view input) {
  std::vector<std::string> result;
  for_each_word(input, [&](const std::string& padded) {
    for (size_t i = 0; i + 2 < padded.size(); ++i)
      result.push_back(padded.substr(i, 3));
  });
  return result;
}

// Dice coefficient: 2 * |A ∩ B| / (|A| + |B|)
static double dice_similarity(const std::set<std::string>& a,
                               const std::set<std::string>& b) {
  if (a.empty() && b.empty()) return 0.0;
  size_t shared = 0;
  for (const auto& t : a) {
    if (b.count(t)) ++shared;
  }
  return 2.0 * shared / (a.size() + b.size());
}

// word_similarity: max over all contiguous windows of seq2 of
//   |t1 ∩ window| / max(|t1|, |window|)
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
      double sim = static_cast<double>(matched) / std::max(t1.size(), window.size());
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
  for_each_word(input, [&](const std::string& padded) {
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
      double sim = static_cast<double>(matched) / std::max(t1.size(), window.size());
      if (sim > best) best = sim;
    }
  }
  return best;
}

// =============================================================================
// trgm_show(text) -> STRING  (sorted JSON array of trigrams)
// =============================================================================

void trgm_show_impl(vef_context_t* ctx, vef_invalue_t* arg,
                    vef_vdf_result_t* result) {
  try {
    if (arg->is_null) { result->type = VEF_RESULT_NULL; return; }

    auto trgms = extract_trigrams(std::string_view(arg->str_value, arg->str_len));

    std::string out = "[";
    bool first = true;
    for (const auto& t : trgms) {
      if (!first) out += ',';
      first = false;
      out += '"';
      out += t;  // trigrams contain only [a-z0-9 ]; safe in JSON strings
      out += '"';
    }
    out += ']';

    if (out.size() > result->max_str_len) {
      result->type = VEF_RESULT_ERROR;
      snprintf(result->error_msg, VEF_MAX_ERROR_LEN,
               "trgm_show: output too large (%zu bytes)", out.size());
      return;
    }
    memcpy(result->str_buf, out.data(), out.size());
    result->actual_len = out.size();
    result->type = VEF_RESULT_VALUE;
  } catch (...) {
    result->type = VEF_RESULT_ERROR;
    snprintf(result->error_msg, VEF_MAX_ERROR_LEN, "trgm_show: internal error");
  }
}

// =============================================================================
// trgm_similarity(text, text) -> REAL
// =============================================================================

void trgm_similarity_impl(vef_context_t* ctx, vef_invalue_t* a, vef_invalue_t* b,
                          vef_vdf_result_t* result) {
  try {
    if (a->is_null || b->is_null) { result->type = VEF_RESULT_NULL; return; }
    auto ta = extract_trigrams(std::string_view(a->str_value, a->str_len));
    auto tb = extract_trigrams(std::string_view(b->str_value, b->str_len));
    result->real_value = dice_similarity(ta, tb);
    result->type = VEF_RESULT_VALUE;
  } catch (...) {
    result->type = VEF_RESULT_ERROR;
    snprintf(result->error_msg, VEF_MAX_ERROR_LEN, "trgm_similarity: internal error");
  }
}

// =============================================================================
// trgm_distance(text, text) -> REAL   (1 - similarity)
// =============================================================================

void trgm_distance_impl(vef_context_t* ctx, vef_invalue_t* a, vef_invalue_t* b,
                        vef_vdf_result_t* result) {
  try {
    if (a->is_null || b->is_null) { result->type = VEF_RESULT_NULL; return; }
    auto ta = extract_trigrams(std::string_view(a->str_value, a->str_len));
    auto tb = extract_trigrams(std::string_view(b->str_value, b->str_len));
    result->real_value = 1.0 - dice_similarity(ta, tb);
    result->type = VEF_RESULT_VALUE;
  } catch (...) {
    result->type = VEF_RESULT_ERROR;
    snprintf(result->error_msg, VEF_MAX_ERROR_LEN, "trgm_distance: internal error");
  }
}

// =============================================================================
// trgm_similar(text, text) -> INT  (1 if similarity >= 0.3, else 0)
// =============================================================================

void trgm_similar_impl(vef_context_t* ctx, vef_invalue_t* a, vef_invalue_t* b,
                       vef_vdf_result_t* result) {
  try {
    if (a->is_null || b->is_null) { result->type = VEF_RESULT_NULL; return; }
    auto ta = extract_trigrams(std::string_view(a->str_value, a->str_len));
    auto tb = extract_trigrams(std::string_view(b->str_value, b->str_len));
    result->int_value = dice_similarity(ta, tb) >= 0.3 ? 1 : 0;
    result->type = VEF_RESULT_VALUE;
  } catch (...) {
    result->type = VEF_RESULT_ERROR;
    snprintf(result->error_msg, VEF_MAX_ERROR_LEN, "trgm_similar: internal error");
  }
}

// =============================================================================
// trgm_similar_threshold(text, text, real) -> INT
// =============================================================================

void trgm_similar_threshold_impl(vef_context_t* ctx, vef_invalue_t* a,
                                  vef_invalue_t* b, vef_invalue_t* threshold,
                                  vef_vdf_result_t* result) {
  try {
    if (a->is_null || b->is_null || threshold->is_null) {
      result->type = VEF_RESULT_NULL; return;
    }
    if (threshold->real_value < 0.0 || threshold->real_value > 1.0) {
      result->type = VEF_RESULT_ERROR;
      snprintf(result->error_msg, VEF_MAX_ERROR_LEN,
               "trgm_similar_threshold: threshold must be between 0 and 1");
      return;
    }
    auto ta = extract_trigrams(std::string_view(a->str_value, a->str_len));
    auto tb = extract_trigrams(std::string_view(b->str_value, b->str_len));
    result->int_value = dice_similarity(ta, tb) >= threshold->real_value ? 1 : 0;
    result->type = VEF_RESULT_VALUE;
  } catch (...) {
    result->type = VEF_RESULT_ERROR;
    snprintf(result->error_msg, VEF_MAX_ERROR_LEN,
             "trgm_similar_threshold: internal error");
  }
}

// =============================================================================
// trgm_word_similarity(text, text) -> REAL
// =============================================================================

void trgm_word_similarity_impl(vef_context_t* ctx, vef_invalue_t* a, vef_invalue_t* b,
                                vef_vdf_result_t* result) {
  try {
    if (a->is_null || b->is_null) { result->type = VEF_RESULT_NULL; return; }
    auto t1 = extract_trigrams(std::string_view(a->str_value, a->str_len));
    auto seq2 = extract_ordered_trigrams(std::string_view(b->str_value, b->str_len));
    result->real_value = word_sim(t1, seq2);
    result->type = VEF_RESULT_VALUE;
  } catch (...) {
    result->type = VEF_RESULT_ERROR;
    snprintf(result->error_msg, VEF_MAX_ERROR_LEN,
             "trgm_word_similarity: internal error");
  }
}

// =============================================================================
// trgm_word_distance(text, text) -> REAL
// =============================================================================

void trgm_word_distance_impl(vef_context_t* ctx, vef_invalue_t* a, vef_invalue_t* b,
                              vef_vdf_result_t* result) {
  try {
    if (a->is_null || b->is_null) { result->type = VEF_RESULT_NULL; return; }
    auto t1 = extract_trigrams(std::string_view(a->str_value, a->str_len));
    auto seq2 = extract_ordered_trigrams(std::string_view(b->str_value, b->str_len));
    result->real_value = 1.0 - word_sim(t1, seq2);
    result->type = VEF_RESULT_VALUE;
  } catch (...) {
    result->type = VEF_RESULT_ERROR;
    snprintf(result->error_msg, VEF_MAX_ERROR_LEN,
             "trgm_word_distance: internal error");
  }
}

// =============================================================================
// trgm_strict_word_similarity(text, text) -> REAL
// =============================================================================

void trgm_strict_word_similarity_impl(vef_context_t* ctx, vef_invalue_t* a,
                                       vef_invalue_t* b, vef_vdf_result_t* result) {
  try {
    if (a->is_null || b->is_null) { result->type = VEF_RESULT_NULL; return; }
    std::string_view sv_b(b->str_value, b->str_len);
    auto t1 = extract_trigrams(std::string_view(a->str_value, a->str_len));
    auto seq2 = extract_ordered_trigrams(sv_b);
    auto wb = compute_word_boundaries(sv_b);
    result->real_value = strict_word_sim(t1, seq2, wb);
    result->type = VEF_RESULT_VALUE;
  } catch (...) {
    result->type = VEF_RESULT_ERROR;
    snprintf(result->error_msg, VEF_MAX_ERROR_LEN,
             "trgm_strict_word_similarity: internal error");
  }
}

// =============================================================================
// trgm_strict_word_distance(text, text) -> REAL
// =============================================================================

void trgm_strict_word_distance_impl(vef_context_t* ctx, vef_invalue_t* a,
                                     vef_invalue_t* b, vef_vdf_result_t* result) {
  try {
    if (a->is_null || b->is_null) { result->type = VEF_RESULT_NULL; return; }
    std::string_view sv_b(b->str_value, b->str_len);
    auto t1 = extract_trigrams(std::string_view(a->str_value, a->str_len));
    auto seq2 = extract_ordered_trigrams(sv_b);
    auto wb = compute_word_boundaries(sv_b);
    result->real_value = 1.0 - strict_word_sim(t1, seq2, wb);
    result->type = VEF_RESULT_VALUE;
  } catch (...) {
    result->type = VEF_RESULT_ERROR;
    snprintf(result->error_msg, VEF_MAX_ERROR_LEN,
             "trgm_strict_word_distance: internal error");
  }
}

// =============================================================================
// trgm_word_similar(text, text) -> INT  (1 if word_similarity >= 0.6, else 0)
// =============================================================================

void trgm_word_similar_impl(vef_context_t* ctx, vef_invalue_t* a, vef_invalue_t* b,
                             vef_vdf_result_t* result) {
  try {
    if (a->is_null || b->is_null) { result->type = VEF_RESULT_NULL; return; }
    auto t1 = extract_trigrams(std::string_view(a->str_value, a->str_len));
    auto seq2 = extract_ordered_trigrams(std::string_view(b->str_value, b->str_len));
    result->int_value = word_sim(t1, seq2) >= 0.6 ? 1 : 0;
    result->type = VEF_RESULT_VALUE;
  } catch (...) {
    result->type = VEF_RESULT_ERROR;
    snprintf(result->error_msg, VEF_MAX_ERROR_LEN,
             "trgm_word_similar: internal error");
  }
}

// =============================================================================
// trgm_strict_word_similar(text, text) -> INT
// =============================================================================

void trgm_strict_word_similar_impl(vef_context_t* ctx, vef_invalue_t* a,
                                    vef_invalue_t* b, vef_vdf_result_t* result) {
  try {
    if (a->is_null || b->is_null) { result->type = VEF_RESULT_NULL; return; }
    std::string_view sv_b(b->str_value, b->str_len);
    auto t1 = extract_trigrams(std::string_view(a->str_value, a->str_len));
    auto seq2 = extract_ordered_trigrams(sv_b);
    auto wb = compute_word_boundaries(sv_b);
    result->int_value = strict_word_sim(t1, seq2, wb) >= 0.5 ? 1 : 0;
    result->type = VEF_RESULT_VALUE;
  } catch (...) {
    result->type = VEF_RESULT_ERROR;
    snprintf(result->error_msg, VEF_MAX_ERROR_LEN,
             "trgm_strict_word_similar: internal error");
  }
}

// =============================================================================
// Extension registration
// =============================================================================

VEF_GENERATE_ENTRY_POINTS(
  make_extension("vsql_trgm", "1.0.0")
    .func(make_func<&trgm_show_impl>("trgm_show")
      .returns(STRING).param(STRING).buffer_size(4096).build())
    .func(make_func<&trgm_similarity_impl>("trgm_similarity")
      .returns(REAL).param(STRING).param(STRING).build())
    .func(make_func<&trgm_distance_impl>("trgm_distance")
      .returns(REAL).param(STRING).param(STRING).build())
    .func(make_func<&trgm_similar_impl>("trgm_similar")
      .returns(INT).param(STRING).param(STRING).build())
    .func(make_func<&trgm_similar_threshold_impl>("trgm_similar_threshold")
      .returns(INT).param(STRING).param(STRING).param(REAL).build())
    .func(make_func<&trgm_word_similarity_impl>("trgm_word_similarity")
      .returns(REAL).param(STRING).param(STRING).build())
    .func(make_func<&trgm_word_distance_impl>("trgm_word_distance")
      .returns(REAL).param(STRING).param(STRING).build())
    .func(make_func<&trgm_word_similar_impl>("trgm_word_similar")
      .returns(INT).param(STRING).param(STRING).build())
    .func(make_func<&trgm_strict_word_similarity_impl>("trgm_strict_word_similarity")
      .returns(REAL).param(STRING).param(STRING).build())
    .func(make_func<&trgm_strict_word_distance_impl>("trgm_strict_word_distance")
      .returns(REAL).param(STRING).param(STRING).build())
    .func(make_func<&trgm_strict_word_similar_impl>("trgm_strict_word_similar")
      .returns(INT).param(STRING).param(STRING).build())
)
