// Private, parser-aware HTML entity decoding shared by the Markdown renderer
// and heading extraction. Not a public markit header; the MD4C dependency
// types stay out of it.
#ifndef MARKIT_ENTITY_DECODE_HPP
#define MARKIT_ENTITY_DECODE_HPP

#include <string>
#include <string_view>

namespace markit {

// Outcome of decoding one complete entity reference.
enum class EntityDecodeStatus {
  kNotEntity,  // not a complete/known reference; keep the original spelling.
  kDecoded,    // decoded to UTF-8 code points (may include control code points).
  kInvalid,    // complete numeric reference with an invalid scalar (zero,
               // surrogate, out of range or overflow); display uses U+FFFD and
               // link destinations must reject it.
};

// Decode a single complete entity reference. `ref` should start with '&' and
// end with ';'. On kNotEntity, `*out` is left empty and the caller keeps the
// original spelling. On kInvalid, `*out` receives U+FFFD. On kDecoded, `*out`
// receives the exact decoded code points (no display sanitizing).
EntityDecodeStatus DecodeEntityReference(std::string_view ref, std::string* out);

// Display-safe decode of one entity reference: controls other than decoded
// whitespace are visibly replaced with U+FFFD; tab/newline/CR become a space
// so layout handles them. Unknown references keep their original spelling.
std::string DecodeEntityForDisplay(std::string_view ref);

// Decode every complete, semicolon-terminated reference in parser-bounded
// text. Text outside references is copied verbatim; incomplete, unknown or
// over-long references keep their spelling. Intended for interpreted HTML text
// after markup boundaries are recognized, never for rescanning emitted output.
std::string DecodeEntityText(std::string_view text);

// A decoded link destination plus whether decoding produced an invalid scalar.
struct DecodedDestination {
  std::string text;
  bool invalid = false;
};

// Decode a link destination, preserving code points exactly (including
// controls) and flagging invalid numeric scalars so the caller can reject the
// destination instead of substituting a different target.
DecodedDestination DecodeDestination(std::string_view text);

}  // namespace markit

#endif  // MARKIT_ENTITY_DECODE_HPP
