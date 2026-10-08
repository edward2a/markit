// Implementation of private HTML entity decoding. See entity_decode.hpp.
#include "entity_decode.hpp"

// md4c's entity table is C; declare it with C linkage (its header lacks the
// usual __cplusplus guards).
extern "C" {
#include "entity.h"
}

#include <cstdint>  // for uint32_t, uint64_t

namespace markit {

namespace {

void AppendUtf8(uint32_t cp, std::string* out) {
  if (cp <= 0x7F) {
    out->push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FF) {
    out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
    out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

constexpr std::string_view kReplacement = "\xEF\xBF\xBD";  // U+FFFD.

// Longest plausible reference: named entities are < 32 bytes, numeric < 12.
constexpr size_t kMaxReferenceLength = 32;

// Find the semicolon ending the reference that starts at `start` (known to be
// '&'), or npos when the candidate is incomplete, over-long, or interrupted by
// another '&' (which cannot appear inside a reference). Stopping at the next
// '&' keeps an unknown candidate from swallowing a later valid reference.
size_t FindReferenceEnd(std::string_view text, size_t start) {
  size_t limit = start + kMaxReferenceLength;
  if (limit > text.size()) {
    limit = text.size();
  }
  for (size_t j = start + 1; j < limit; ++j) {
    if (text[j] == '&') {
      return std::string_view::npos;
    }
    if (text[j] == ';') {
      return j;
    }
  }
  return std::string_view::npos;
}

bool IsWhitespaceCodePoint(uint32_t cp) {
  return cp == 0x09 || cp == 0x0A || cp == 0x0D;
}

bool IsUnsafeControl(uint32_t cp) {
  return cp <= 0x1F || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F);
}

// Replace controls in decoded display text: legitimate whitespace becomes a
// space, every other unsafe control becomes U+FFFD. Valid text is copied.
std::string SanitizeDisplay(std::string_view decoded) {
  std::string out;
  out.reserve(decoded.size());
  size_t i = 0;
  const size_t n = decoded.size();
  while (i < n) {
    const unsigned char lead = static_cast<unsigned char>(decoded[i]);
    uint32_t cp = lead;
    size_t len = 1;
    if (lead >= 0xC0) {
      if ((lead & 0xE0) == 0xC0) {
        len = 2;
        cp = lead & 0x1F;
      } else if ((lead & 0xF0) == 0xE0) {
        len = 3;
        cp = lead & 0x0F;
      } else if ((lead & 0xF8) == 0xF0) {
        len = 4;
        cp = lead & 0x07;
      }
      if (i + len > n) {
        len = 1;
        cp = lead;
      } else {
        for (size_t k = 1; k < len; ++k) {
          cp = (cp << 6) | (static_cast<unsigned char>(decoded[i + k]) & 0x3F);
        }
      }
    }
    if (IsWhitespaceCodePoint(cp)) {
      out.push_back(' ');
    } else if (IsUnsafeControl(cp)) {
      out.append(kReplacement);
    } else {
      out.append(decoded, i, len);
    }
    i += len;
  }
  return out;
}

}  // namespace

EntityDecodeStatus DecodeEntityReference(std::string_view ref,
                                         std::string* out) {
  out->clear();
  if (ref.size() < 3 || ref.front() != '&' || ref.back() != ';' ||
      ref.size() > kMaxReferenceLength) {
    return EntityDecodeStatus::kNotEntity;
  }

  if (ref[1] == '#') {
    const bool hex = (ref[2] == 'x' || ref[2] == 'X');
    const size_t start = hex ? 3 : 2;
    const size_t end = ref.size() - 1;  // index of ';'
    if (start >= end) {
      return EntityDecodeStatus::kNotEntity;  // "&#;" / "&#x;"
    }
    uint64_t value = 0;
    bool overflow = false;
    for (size_t i = start; i < end; ++i) {
      const char c = ref[i];
      int digit;
      if (c >= '0' && c <= '9') {
        digit = c - '0';
      } else if (hex && c >= 'a' && c <= 'f') {
        digit = c - 'a' + 10;
      } else if (hex && c >= 'A' && c <= 'F') {
        digit = c - 'A' + 10;
      } else {
        return EntityDecodeStatus::kNotEntity;  // malformed numeric reference.
      }
      if (!overflow) {
        if (value > 0x10FFFF) {
          overflow = true;
        } else {
          value = value * (hex ? 16 : 10) + static_cast<uint64_t>(digit);
          if (value > 0x10FFFF) {
            overflow = true;
          }
        }
      }
    }
    if (overflow || value == 0 || value > 0x10FFFF ||
        (value >= 0xD800 && value <= 0xDFFF)) {
      out->assign(kReplacement);
      return EntityDecodeStatus::kInvalid;
    }
    AppendUtf8(static_cast<uint32_t>(value), out);
    return EntityDecodeStatus::kDecoded;
  }

  const ENTITY* ent = entity_lookup(ref.data(), ref.size());
  if (ent == nullptr) {
    return EntityDecodeStatus::kNotEntity;
  }
  AppendUtf8(ent->codepoints[0], out);
  if (ent->codepoints[1] != 0) {
    AppendUtf8(ent->codepoints[1], out);
  }
  return EntityDecodeStatus::kDecoded;
}

std::string DecodeEntityForDisplay(std::string_view ref) {
  std::string decoded;
  switch (DecodeEntityReference(ref, &decoded)) {
    case EntityDecodeStatus::kNotEntity:
      return std::string(ref);
    case EntityDecodeStatus::kInvalid:
      return std::string(kReplacement);
    case EntityDecodeStatus::kDecoded:
      return SanitizeDisplay(decoded);
  }
  return std::string(ref);
}

std::string DecodeEntityText(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  size_t i = 0;
  const size_t n = text.size();
  while (i < n) {
    if (text[i] != '&') {
      out.push_back(text[i++]);
      continue;
    }
    const size_t semi = FindReferenceEnd(text, i);
    if (semi == std::string_view::npos) {
      out.push_back(text[i++]);  // incomplete/over-long/interrupted: verbatim.
      continue;
    }
    out += DecodeEntityForDisplay(text.substr(i, semi - i + 1));
    i = semi + 1;
  }
  return out;
}

DecodedDestination DecodeDestination(std::string_view text) {
  DecodedDestination result;
  result.text.reserve(text.size());
  size_t i = 0;
  const size_t n = text.size();
  while (i < n) {
    if (text[i] != '&') {
      result.text.push_back(text[i++]);
      continue;
    }
    const size_t semi = FindReferenceEnd(text, i);
    if (semi == std::string_view::npos) {
      result.text.push_back(text[i++]);
      continue;
    }
    std::string decoded;
    switch (DecodeEntityReference(text.substr(i, semi - i + 1), &decoded)) {
      case EntityDecodeStatus::kNotEntity:
        result.text.append(text.substr(i, semi - i + 1));
        break;
      case EntityDecodeStatus::kInvalid:
        result.invalid = true;
        result.text.append(kReplacement);
        break;
      case EntityDecodeStatus::kDecoded:
        result.text += decoded;
        break;
    }
    i = semi + 1;
  }
  return result;
}

}  // namespace markit
