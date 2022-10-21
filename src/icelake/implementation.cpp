#include "simdutf/icelake/intrinsics.h"

#include "scalar/utf16_to_utf8/valid_utf16_to_utf8.h"
#include "scalar/utf16_to_utf8/utf16_to_utf8.h"
#include "scalar/utf8_to_utf16/valid_utf8_to_utf16.h"
#include "scalar/utf8_to_utf16/utf8_to_utf16.h"
#include "scalar/utf8.h"
#include "scalar/utf16.h"

#include "simdutf/icelake/begin.h"
namespace simdutf {
namespace SIMDUTF_IMPLEMENTATION {
namespace {
#ifndef SIMDUTF_ICELAKE_H
#error "icelake.h must be included"
#endif
#include "icelake/icelake-utf8-common.inl.cpp"
#include "icelake/icelake-macros.inl.cpp"
#include "icelake/icelake-from-valid-utf8.inl.cpp"
#include "icelake/icelake-utf8-validation.inl.cpp"
#include "icelake/icelake-from-utf8.inl.cpp"
#include "icelake/icelake-convert-utf16-to-utf32.inl.cpp"
#include "icelake/icelake-ascii-validation.inl.cpp"
#include "icelake/icelake-utf32-validation.inl.cpp"
#include "icelake/icelake_convert_utf16_to_utf8.inl.cpp"
//#include "icelake/icelake_detect_encodings.cpp"

} // namespace
} // namespace SIMDUTF_IMPLEMENTATION
} // namespace simdutf

namespace simdutf {
namespace SIMDUTF_IMPLEMENTATION {


simdutf_warn_unused int
implementation::detect_encodings(const char *input,
                                 size_t length) const noexcept {
  if (length % 2 == 0) {
    const char *buf = input;

    const char *start = buf;
    const char *end = input + length;

    bool is_utf8 = true;
    bool is_utf16 = true;
    bool is_utf32 = true;

    int out = 0;

    avx512_utf8_checker checker{};
    __m512i currentmax = _mm512_setzero_si512();
    while (buf + 64 <= end) {
      __m512i in = _mm512_loadu_si512((__m512i *)buf);
      __m512i diff = _mm512_sub_epi16(in, _mm512_set1_epi16(uint16_t(0xD800)));
      __mmask32 surrogates =
          _mm512_cmplt_epu16_mask(diff, _mm512_set1_epi16(uint16_t(0x0800)));
      if (surrogates) {
        is_utf8 = false;

        // Can still be either UTF-16LE or UTF-32LE depending on the positions
        // of the surrogates To be valid UTF-32LE, a surrogate cannot be in the
        // two most significant bytes of any 32-bit word. On the other hand, to
        // be valid UTF-16LE, at least one surrogate must be in the two most
        // significant bytes of a 32-bit word since they always come in pairs in
        // UTF-16LE. Note that we always proceed in multiple of 4 before this
        // point so there is no offset in 32-bit words.

        if ((surrogates & 0xaaaaaaaa) != 0) {
          is_utf32 = false;
          __mmask32 highsurrogates = _mm512_cmplt_epu16_mask(
              diff, _mm512_set1_epi16(uint16_t(0x0400)));
          __mmask32 lowsurrogates = surrogates ^ highsurrogates;
          // high must be followed by low
          if ((highsurrogates << 1) != lowsurrogates) {
            return simdutf::encoding_type::unspecified;
          }

          bool ends_with_high = ((highsurrogates & 0x80000000) != 0);
          if (ends_with_high) {
            buf +=
                31 *
                sizeof(char16_t); // advance only by 31 words so that we start
                                  // with the high surrogate on the next round.
          } else {
            buf += 32 * sizeof(char16_t);
          }
          is_utf16 = validate_utf16le(reinterpret_cast<const char16_t *>(buf),
                                      (end - buf) / sizeof(char16_t));
          if (!is_utf16) {
            return simdutf::encoding_type::unspecified;

          } else {
            return simdutf::encoding_type::UTF16_LE;
          }

        } else {
          is_utf16 = false;
          // Check for UTF-32LE
          if (length % 4 == 0) {
            const char32_t *input32 = reinterpret_cast<const char32_t *>(buf);
            const char32_t *end32 =
                reinterpret_cast<const char32_t *>(start) + length / 4;
            if (validate_utf32(input32, end32 - input32)) {
              return simdutf::encoding_type::UTF32_LE;
            }
          }
          return simdutf::encoding_type::unspecified;
        }
        break;
      }
      // If no surrogate, validate under other encodings as well

      // UTF-32LE validation
      currentmax = _mm512_max_epu32(in, currentmax);

      // UTF-8 validation
      checker.check_next_input(in);

      buf += 64;
    }

    // Check which encodings are possible

    if (is_utf8) {
      size_t current_length = static_cast<size_t>(buf - start);
      if (current_length != length) {
        const __m512i utf8 = _mm512_maskz_loadu_epi8(
            (1ULL << (length - current_length)) - 1, (const __m512i *)buf);
        checker.check_next_input(utf8);
      }
      checker.check_eof();
      if (!checker.errors()) {
        out |= simdutf::encoding_type::UTF8;
      }
    }

    if (is_utf16 && scalar::utf16::validate<endianness::LITTLE>(
                        reinterpret_cast<const char16_t *>(buf),
                        (length - (buf - start)) / 2)) {
      out |= simdutf::encoding_type::UTF16_LE;
    }

    if (is_utf32 && (length % 4 == 0)) {
      currentmax = _mm512_max_epu32(
          _mm512_maskz_loadu_epi8(
              (1ULL << (length - static_cast<size_t>(buf - start))) - 1,
              (const __m512i *)buf),
          currentmax);
      __mmask16 outside_range = _mm512_cmp_epu32_mask(currentmax, _mm512_set1_epi32(0x10ffff),
                                _MM_CMPINT_GT);
      if (outside_range == 0) {
        out |= simdutf::encoding_type::UTF32_LE;
      }
    }

    return out;
  } else if (implementation::validate_utf8(input, length)) {
    return simdutf::encoding_type::UTF8;
  } else {
    return simdutf::encoding_type::unspecified;
  }
}

simdutf_warn_unused bool implementation::validate_utf8(const char *buf, size_t len) const noexcept {
    avx512_utf8_checker checker{};
    const char* ptr = buf;
    const char* end = ptr + len;
    for (; ptr + 64 <= end; ptr += 64) {
        const __m512i utf8 = _mm512_loadu_si512((const __m512i*)ptr);
        checker.check_next_input(utf8);
    }
    {
       const __m512i utf8 = _mm512_maskz_loadu_epi8((1ULL<<(end - ptr))-1, (const __m512i*)ptr);
       checker.check_next_input(utf8);
    }
    checker.check_eof();
    return ! checker.errors();
}

simdutf_warn_unused result implementation::validate_utf8_with_errors(const char *buf, size_t len) const noexcept {
    avx512_utf8_checker checker{};
    const char* ptr = buf;
    const char* end = ptr + len;
    size_t count{0};
    for (; ptr + 64 <= end; ptr += 64) {
      const __m512i utf8 = _mm512_loadu_si512((const __m512i*)ptr);
      checker.check_next_input(utf8);
      if(checker.errors()) {
        if (count != 0) { count--; } // Sometimes the error is only detected in the next chunk
        result res = scalar::utf8::rewind_and_validate_with_errors(reinterpret_cast<const char*>(buf + count), len - count);
        res.count += count;
        return res;
      }
      count += 64;
    }
    {
      const __m512i utf8 = _mm512_maskz_loadu_epi8((1ULL<<(end - ptr))-1, (const __m512i*)ptr);
      checker.check_next_input(utf8);
      if(checker.errors()) {
        if (count != 0) { count--; } // Sometimes the error is only detected in the next chunk
        result res = scalar::utf8::rewind_and_validate_with_errors(reinterpret_cast<const char*>(buf + count), len - count);
        res.count += count;
        return res;
      } else {
        return result(error_code::SUCCESS, len);
      }
    }
}

simdutf_warn_unused bool implementation::validate_ascii(const char *buf, size_t len) const noexcept {
  const char* tail = icelake::validate_ascii(buf, len);
  if (tail) {
    return scalar::ascii::validate(tail, len - (tail - buf));
  } else {
    return false;
  }
}

simdutf_warn_unused result implementation::validate_ascii_with_errors(const char *buf, size_t len) const noexcept {
  const char* buf_orig = buf;
  const char* end = buf + len;
  const __m512i ascii = _mm512_set1_epi8((uint8_t)0x80);
  for (; buf + 64 <= end; buf += 64) {
    const __m512i input = _mm512_loadu_si512((const __m512i*)buf);
    __mmask64 notascii = _mm512_cmp_epu8_mask(input, ascii, _MM_CMPINT_NLT);
    if(notascii) {
      return result(error_code::TOO_LARGE, buf - buf_orig + _tzcnt_u64(notascii));
    }
  }
  {
    const __m512i input = _mm512_maskz_loadu_epi8((1ULL<<(end - buf))-1, (const __m512i*)buf);
    __mmask64 notascii = _mm512_cmp_epu8_mask(input, ascii, _MM_CMPINT_NLT);
    if(notascii) {
      return result(error_code::TOO_LARGE, buf - buf_orig + _tzcnt_u64(notascii));
    }
  }
  return result(error_code::SUCCESS, len);
}

simdutf_warn_unused bool implementation::validate_utf16le(const char16_t *buf, size_t len) const noexcept {
    const char16_t *end = buf + len;

    for(;buf + 32 <= end; ) {
      __m512i in = _mm512_loadu_si512((__m512i*)buf);
      __m512i diff = _mm512_sub_epi16(in, _mm512_set1_epi16(uint16_t(0xD800)));
      __mmask32 surrogates = _mm512_cmplt_epu16_mask(diff, _mm512_set1_epi16(uint16_t(0x0800)));
      if(surrogates) {
        __mmask32 highsurrogates = _mm512_cmplt_epu16_mask(diff, _mm512_set1_epi16(uint16_t(0x0400)));
        __mmask32 lowsurrogates = surrogates ^ highsurrogates;
        // high must be followed by low
        if ((highsurrogates << 1) != lowsurrogates) {
           return false;
        }
        bool ends_with_high = ((highsurrogates & 0x80000000) != 0);
        if(ends_with_high) {
          buf += 31; // advance only by 31 words so that we start with the high surrogate on the next round.
        } else {
          buf += 32;
        }
      } else {
        buf += 32;
      }
    }
    if(buf < end) {
      __m512i in = _mm512_maskz_loadu_epi16((1<<(end-buf))-1,(__m512i*)buf);
      __m512i diff = _mm512_sub_epi16(in, _mm512_set1_epi16(uint16_t(0xD800)));
      __mmask32 surrogates = _mm512_cmplt_epu16_mask(diff, _mm512_set1_epi16(uint16_t(0x0800)));
      if(surrogates) {
        __mmask32 highsurrogates = _mm512_cmplt_epu16_mask(diff, _mm512_set1_epi16(uint16_t(0x0400)));
        __mmask32 lowsurrogates = surrogates ^ highsurrogates;
        // high must be followed by low
        if ((highsurrogates << 1) != lowsurrogates) {
           return false;
        }
        if ((highsurrogates & (1<<(end-buf-1))) != 0) { // cannot end with a high surrogate
          return false;
        }
      }
    }
    return true;
}

simdutf_warn_unused bool implementation::validate_utf16be(const char16_t *buf, size_t len) const noexcept {
   const char16_t *end = buf + len;
   const __m512i byteflip = _mm512_setr_epi64(
            0x0607040502030001,
            0x0e100c0d0a0b0809,
            0x0607040502030001,
            0x0e100c0d0a0b0809,
            0x0607040502030001,
            0x0e100c0d0a0b0809,
            0x0607040502030001,
            0x0e100c0d0a0b0809
        );;
    for(;buf + 32 <= end; ) {
      __m512i in = _mm512_shuffle_epi8(_mm512_loadu_si512((__m512i*)buf), byteflip);
      __m512i diff = _mm512_sub_epi16(in, _mm512_set1_epi16(uint16_t(0xD800)));
      __mmask32 surrogates = _mm512_cmplt_epu16_mask(diff, _mm512_set1_epi16(uint16_t(0x0800)));
      if(surrogates) {
        __mmask32 highsurrogates = _mm512_cmplt_epu16_mask(diff, _mm512_set1_epi16(uint16_t(0x0400)));
        __mmask32 lowsurrogates = surrogates ^ highsurrogates;
        // high must be followed by low
        if ((highsurrogates << 1) != lowsurrogates) {
           return false;
        }
        bool ends_with_high = ((highsurrogates & 0x80000000) != 0);
        if(ends_with_high) {
          buf += 31; // advance only by 31 words so that we start with the high surrogate on the next round.
        } else {
          buf += 32;
        }
      } else {
        buf += 32;
      }
    }
    if(buf < end) {
      __m512i in = _mm512_shuffle_epi8(_mm512_maskz_loadu_epi16((1<<(end-buf))-1,(__m512i*)buf), byteflip);
      __m512i diff = _mm512_sub_epi16(in, _mm512_set1_epi16(uint16_t(0xD800)));
      __mmask32 surrogates = _mm512_cmplt_epu16_mask(diff, _mm512_set1_epi16(uint16_t(0x0800)));
      if(surrogates) {
        __mmask32 highsurrogates = _mm512_cmplt_epu16_mask(diff, _mm512_set1_epi16(uint16_t(0x0400)));
        __mmask32 lowsurrogates = surrogates ^ highsurrogates;
        // high must be followed by low
        if ((highsurrogates << 1) != lowsurrogates) {
           return false;
        }
        if ((highsurrogates & (1<<(end-buf-1))) != 0) { // cannot end with a high surrogate
          return false;
        }
      }
    }
    return true;
}

simdutf_warn_unused result implementation::validate_utf16le_with_errors(const char16_t *buf, size_t len) const noexcept {
    return scalar::utf16::validate_with_errors<endianness::LITTLE>(buf, len);
}

simdutf_warn_unused result implementation::validate_utf16be_with_errors(const char16_t *buf, size_t len) const noexcept {
    return scalar::utf16::validate_with_errors<endianness::BIG>(buf, len);
}

simdutf_warn_unused bool implementation::validate_utf32(const char32_t *buf, size_t len) const noexcept {
  const char32_t * tail = icelake::validate_utf32(buf, len);
  if (tail) {
    return scalar::utf32::validate(tail, len - (tail - buf));
  } else {
    return false;
  }
}

simdutf_warn_unused result implementation::validate_utf32_with_errors(const char32_t *buf, size_t len) const noexcept {
    return scalar::utf32::validate_with_errors(buf, len);
}

simdutf_warn_unused size_t implementation::convert_utf8_to_utf16le(const char* buf, size_t len, char16_t* utf16_output) const noexcept {
  utf8_to_utf16_result ret = fast_avx512_convert_utf8_to_utf16(buf, len, utf16_output);
  if (ret.second == nullptr) {
    return 0;
  }
  return ret.second - utf16_output;
}

simdutf_warn_unused size_t implementation::convert_utf8_to_utf16be(const char* buf, size_t len, char16_t* utf16_output) const noexcept {
   return scalar::utf8_to_utf16::convert<endianness::BIG>(buf, len, utf16_output);
}

simdutf_warn_unused result implementation::convert_utf8_to_utf16le_with_errors(const char* buf, size_t len, char16_t* utf16_output) const noexcept {
   return scalar::utf8_to_utf16::convert_with_errors<endianness::LITTLE>(buf, len, utf16_output);
}

simdutf_warn_unused result implementation::convert_utf8_to_utf16be_with_errors(const char* buf, size_t len, char16_t* utf16_output) const noexcept {
   return scalar::utf8_to_utf16::convert_with_errors<endianness::BIG>(buf, len, utf16_output);
}



simdutf_warn_unused size_t implementation::convert_valid_utf8_to_utf16le(const char* buf, size_t len, char16_t* utf16_output) const noexcept {
  utf8_to_utf16_result ret = icelake::valid_utf8_to_fixed_length<char16_t>(buf, len, utf16_output);
  size_t saved_bytes = ret.second - utf16_output;
  const char* end = buf + len;
  if (ret.first == end) {
    return saved_bytes;
  }

  // Note: AVX512 procedure looks up 4 bytes forward, and
  //       correctly converts multi-byte chars even if their
  //       continuation bytes lie outsiede 16-byte window.
  //       It meas, we have to skip continuation bytes from
  //       the beginning ret.first, as they were already consumed.
  while (ret.first != end && ((uint8_t(*ret.first) & 0xc0) == 0x80)) {
      ret.first += 1;
  }

  if (ret.first != end) {
    const size_t scalar_saved_bytes = scalar::utf8_to_utf16::convert_valid<endianness::LITTLE>(
                                        ret.first, len - (ret.first - buf), ret.second);
    if (scalar_saved_bytes == 0) { return 0; }
    saved_bytes += scalar_saved_bytes;
  }

  return saved_bytes;
}

simdutf_warn_unused size_t implementation::convert_valid_utf8_to_utf16be(const char* buf, size_t len, char16_t* utf16_output) const noexcept {
   return scalar::utf8_to_utf16::convert_valid<endianness::BIG>(buf, len, utf16_output);
}


simdutf_warn_unused size_t implementation::convert_utf8_to_utf32(const char* buf, size_t len, char32_t* utf32_out) const noexcept {
  uint32_t * utf32_output = reinterpret_cast<uint32_t *>(utf32_out);
  utf8_to_utf32_result ret = icelake::validating_utf8_to_fixed_length<uint32_t>(buf, len, utf32_output);
  if (ret.second == nullptr)
    return 0;

  size_t saved_bytes = ret.second - utf32_output;
  const char* end = buf + len;
  if (ret.first == end) {
    return saved_bytes;
  }

  // Note: the AVX512 procedure looks up 4 bytes forward, and
  //       correctly converts multi-byte chars even if their
  //       continuation bytes lie outside 16-byte window.
  //       It means, we have to skip continuation bytes from
  //       the beginning ret.first, as they were already consumed.
  while (ret.first != end and ((uint8_t(*ret.first) & 0xc0) == 0x80)) {
      ret.first += 1;
  }

  if (ret.first != end) {
    const size_t scalar_saved_bytes = scalar::utf8_to_utf32::convert(
                                        ret.first, len - (ret.first - buf), utf32_out + saved_bytes);
    if (scalar_saved_bytes == 0) { return 0; }
    saved_bytes += scalar_saved_bytes;
  }

  return saved_bytes;
}

simdutf_warn_unused result implementation::convert_utf8_to_utf32_with_errors(const char* buf, size_t len, char32_t* utf32_output) const noexcept {
   return scalar::utf8_to_utf32::convert_with_errors(buf, len, utf32_output);
}


simdutf_warn_unused size_t implementation::convert_valid_utf8_to_utf32(const char* buf, size_t len, char32_t* utf32_out) const noexcept {
  uint32_t * utf32_output = reinterpret_cast<uint32_t *>(utf32_out);
  utf8_to_utf32_result ret = icelake::valid_utf8_to_fixed_length<uint32_t>(buf, len, utf32_output);
  size_t saved_bytes = ret.second - utf32_output;
  const char* end = buf + len;
  if (ret.first == end) {
    return saved_bytes;
  }

  // Note: AVX512 procedure looks up 4 bytes forward, and
  //       correctly converts multi-byte chars even if their
  //       continuation bytes lie outsiede 16-byte window.
  //       It meas, we have to skip continuation bytes from
  //       the beginning ret.first, as they were already consumed.
  while (ret.first != end && ((uint8_t(*ret.first) & 0xc0) == 0x80)) {
      ret.first += 1;
  }

  if (ret.first != end) {
    const size_t scalar_saved_bytes = scalar::utf8_to_utf32::convert_valid(
                                        ret.first, len - (ret.first - buf), utf32_out + saved_bytes);
    if (scalar_saved_bytes == 0) { return 0; }
    saved_bytes += scalar_saved_bytes;
  }

  return saved_bytes;
}

simdutf_warn_unused size_t implementation::convert_utf16le_to_utf8(const char16_t* buf, size_t len, char* utf8_output) const noexcept {
  size_t outlen;
  size_t inlen = utf16le_to_utf8_avx512i(buf, len, (unsigned char*)utf8_output, &outlen);
  if(inlen != len) { return 0; }
  return outlen;
}

simdutf_warn_unused size_t implementation::convert_utf16be_to_utf8(const char16_t* buf, size_t len, char* utf8_output) const noexcept {
  return scalar::utf16_to_utf8::convert<endianness::BIG>(buf, len, utf8_output);
}

simdutf_warn_unused result implementation::convert_utf16le_to_utf8_with_errors(const char16_t* buf, size_t len, char* utf8_output) const noexcept {
  return scalar::utf16_to_utf8::convert_with_errors<endianness::LITTLE>(buf, len, utf8_output);
}

simdutf_warn_unused result implementation::convert_utf16be_to_utf8_with_errors(const char16_t* buf, size_t len, char* utf8_output) const noexcept {
  return scalar::utf16_to_utf8::convert_with_errors<endianness::BIG>(buf, len, utf8_output);
}

simdutf_warn_unused size_t implementation::convert_valid_utf16le_to_utf8(const char16_t* buf, size_t len, char* utf8_output) const noexcept {
  return convert_utf16le_to_utf8(buf, len, utf8_output);
}

simdutf_warn_unused size_t implementation::convert_valid_utf16be_to_utf8(const char16_t* buf, size_t len, char* utf8_output) const noexcept {
  return scalar::utf16_to_utf8::convert_valid<endianness::BIG>(buf, len, utf8_output);
}

simdutf_warn_unused size_t implementation::convert_utf32_to_utf8(const char32_t* buf, size_t len, char* utf8_output) const noexcept {
  return scalar::utf32_to_utf8::convert(buf, len, utf8_output);
}

simdutf_warn_unused result implementation::convert_utf32_to_utf8_with_errors(const char32_t* buf, size_t len, char* utf8_output) const noexcept {
  return scalar::utf32_to_utf8::convert_with_errors(buf, len, utf8_output);
}

simdutf_warn_unused size_t implementation::convert_valid_utf32_to_utf8(const char32_t* buf, size_t len, char* utf8_output) const noexcept {
  return scalar::utf32_to_utf8::convert_valid(buf, len, utf8_output);
}

simdutf_warn_unused size_t implementation::convert_utf32_to_utf16le(const char32_t* buf, size_t len, char16_t* utf16_output) const noexcept {
  return scalar::utf32_to_utf16::convert<endianness::LITTLE>(buf, len, utf16_output);
}

simdutf_warn_unused size_t implementation::convert_utf32_to_utf16be(const char32_t* buf, size_t len, char16_t* utf16_output) const noexcept {
  return scalar::utf32_to_utf16::convert<endianness::BIG>(buf, len, utf16_output);
}

simdutf_warn_unused result implementation::convert_utf32_to_utf16le_with_errors(const char32_t* buf, size_t len, char16_t* utf16_output) const noexcept {
  return scalar::utf32_to_utf16::convert_with_errors<endianness::LITTLE>(buf, len, utf16_output);
}

simdutf_warn_unused result implementation::convert_utf32_to_utf16be_with_errors(const char32_t* buf, size_t len, char16_t* utf16_output) const noexcept {
  return scalar::utf32_to_utf16::convert_with_errors<endianness::BIG>(buf, len, utf16_output);
}

simdutf_warn_unused size_t implementation::convert_valid_utf32_to_utf16le(const char32_t* buf, size_t len, char16_t* utf16_output) const noexcept {
  return scalar::utf32_to_utf16::convert_valid<endianness::LITTLE>(buf, len, utf16_output);
}

simdutf_warn_unused size_t implementation::convert_valid_utf32_to_utf16be(const char32_t* buf, size_t len, char16_t* utf16_output) const noexcept {
  return scalar::utf32_to_utf16::convert_valid<endianness::BIG>(buf, len, utf16_output);
}

simdutf_warn_unused size_t implementation::convert_utf16le_to_utf32(const char16_t* buf, size_t len, char32_t* utf32_output) const noexcept {
  std::pair<const char16_t*, char32_t*> ret = icelake::convert_utf16_to_utf32(buf, len, utf32_output);
  if (ret.first == nullptr) { return 0; }
  size_t saved_bytes = ret.second - utf32_output;
  if (ret.first != buf + len) {
    const size_t scalar_saved_bytes = scalar::utf16_to_utf32::convert<endianness::LITTLE>(
                                        ret.first, len - (ret.first - buf), ret.second);
    if (scalar_saved_bytes == 0) { return 0; }
    saved_bytes += scalar_saved_bytes;
  }
  return saved_bytes;
}

simdutf_warn_unused size_t implementation::convert_utf16be_to_utf32(const char16_t* buf, size_t len, char32_t* utf32_output) const noexcept {
  return scalar::utf16_to_utf32::convert<endianness::BIG>(buf, len, utf32_output);
}

simdutf_warn_unused result implementation::convert_utf16le_to_utf32_with_errors(const char16_t* buf, size_t len, char32_t* utf32_output) const noexcept {
  return scalar::utf16_to_utf32::convert_with_errors<endianness::LITTLE>(buf, len, utf32_output);
}

simdutf_warn_unused result implementation::convert_utf16be_to_utf32_with_errors(const char16_t* buf, size_t len, char32_t* utf32_output) const noexcept {
  return scalar::utf16_to_utf32::convert_with_errors<endianness::BIG>(buf, len, utf32_output);
}

simdutf_warn_unused size_t implementation::convert_valid_utf16le_to_utf32(const char16_t* buf, size_t len, char32_t* utf32_output) const noexcept {
  return scalar::utf16_to_utf32::convert_valid<endianness::LITTLE>(buf, len, utf32_output);
}

simdutf_warn_unused size_t implementation::convert_valid_utf16be_to_utf32(const char16_t* buf, size_t len, char32_t* utf32_output) const noexcept {
  return scalar::utf16_to_utf32::convert_valid<endianness::BIG>(buf, len, utf32_output);
}

void implementation::change_endianness_utf16(const char16_t * input, size_t length, char16_t * output) const noexcept {
  scalar::utf16::change_endianness_utf16(input, length, output);
}


simdutf_warn_unused size_t implementation::count_utf16le(const char16_t * input, size_t length) const noexcept {
  const char16_t* end = length >= 32 ? input + length - 32 : nullptr;
  const char16_t* ptr = input;

  const __m512i low = _mm512_set1_epi16((uint16_t)0xdc00);
  const __m512i high = _mm512_set1_epi16((uint16_t)0xdfff);

  size_t count{0};

  while (ptr <= end) {
    __m512i utf16 = _mm512_loadu_si512((const __m512i*)ptr);
    ptr += 32;
    uint64_t not_high_surrogate = static_cast<uint64_t>(_mm512_cmpgt_epu16_mask(utf16, high) | _mm512_cmplt_epu16_mask(utf16, low));
    count += count_ones(not_high_surrogate);
  }

  return count + scalar::utf16::count_code_points<endianness::LITTLE>(ptr, length - (ptr - input));
}

simdutf_warn_unused size_t implementation::count_utf16be(const char16_t * input, size_t length) const noexcept {
  return scalar::utf16::count_code_points<endianness::BIG>(input, length);
}


simdutf_warn_unused size_t implementation::count_utf8(const char * input, size_t length) const noexcept {
  const char* end = length >= 64 ? input + length - 64 : nullptr;
  const char* ptr = input;

  const __m512i continuation = _mm512_set1_epi8(char(0b10111111));

  size_t count{0};

  while (ptr <= end) {
    __m512i utf8 = _mm512_loadu_si512((const __m512i*)ptr);
    ptr += 64;
    uint64_t continuation_bitmask = static_cast<uint64_t>(_mm512_cmple_epi8_mask(utf8, continuation));
    count += 64 - count_ones(continuation_bitmask);
  }

  return count + scalar::utf8::count_code_points(ptr, length - (ptr - input));
}


simdutf_warn_unused size_t implementation::utf8_length_from_utf16le(const char16_t * input, size_t length) const noexcept {
  const char16_t* end = length >= 32 ? input + length - 32 : nullptr;
  const char16_t* ptr = input;

  const __m512i v_007f = _mm512_set1_epi16((uint16_t)0x007f);
  const __m512i v_07ff = _mm512_set1_epi16((uint16_t)0x07ff);
  const __m512i v_dfff = _mm512_set1_epi16((uint16_t)0xdfff);
  const __m512i v_d800 = _mm512_set1_epi16((uint16_t)0xd800);

  size_t count{0};

  while (ptr <= end) {
    __m512i utf16 = _mm512_loadu_si512((const __m512i*)ptr);
    ptr += 32;
    __mmask32 ascii_bitmask = _mm512_cmple_epu16_mask(utf16, v_007f);
    __mmask32 two_bytes_bitmask = _mm512_mask_cmple_epu16_mask(~ascii_bitmask, utf16, v_07ff);
    __mmask32 not_one_two_bytes = ~(ascii_bitmask | two_bytes_bitmask);
    __mmask32 surrogates_bitmask = _mm512_mask_cmple_epu16_mask(not_one_two_bytes, utf16, v_dfff) & _mm512_mask_cmpge_epu16_mask(not_one_two_bytes, utf16, v_d800);

    size_t ascii_count = count_ones(ascii_bitmask);
    size_t two_bytes_count = count_ones(two_bytes_bitmask);
    size_t surrogate_bytes_count = count_ones(surrogates_bitmask);
    size_t three_bytes_count = 32 - ascii_count - two_bytes_count - surrogate_bytes_count;

    count += ascii_count + 2*two_bytes_count + 3*three_bytes_count + 2*surrogate_bytes_count;
  }

  return count + scalar::utf16::utf8_length_from_utf16<endianness::LITTLE>(ptr, length - (ptr - input));
}

simdutf_warn_unused size_t implementation::utf8_length_from_utf16be(const char16_t * input, size_t length) const noexcept {
  return scalar::utf16::utf8_length_from_utf16<endianness::BIG>(input, length);
}

simdutf_warn_unused size_t implementation::utf32_length_from_utf16le(const char16_t * input, size_t length) const noexcept {
  return implementation::count_utf16le(input, length);
}

simdutf_warn_unused size_t implementation::utf32_length_from_utf16be(const char16_t * input, size_t length) const noexcept {
  return scalar::utf16::utf32_length_from_utf16<endianness::BIG>(input, length);
}

simdutf_warn_unused size_t implementation::utf16_length_from_utf8(const char * input, size_t length) const noexcept {
  return scalar::utf8::utf16_length_from_utf8(input, length);
}

simdutf_warn_unused size_t implementation::utf8_length_from_utf32(const char32_t * input, size_t length) const noexcept {
  const char32_t* end = length >= 16 ? input + length - 16 : nullptr;
  const char32_t* ptr = input;

  const __m512i v_0000_007f = _mm512_set1_epi32((uint32_t)0x7f);
  const __m512i v_0000_07ff = _mm512_set1_epi32((uint32_t)0x7ff);
  const __m512i v_0000_ffff = _mm512_set1_epi32((uint32_t)0x0000ffff);

  size_t count{0};

  while (ptr <= end) {
    __m512i utf32 = _mm512_loadu_si512((const __m512i*)ptr);
    ptr += 16;
    __mmask16 ascii_bitmask = _mm512_cmple_epu32_mask(utf32, v_0000_007f);
    __mmask16 two_bytes_bitmask = _mm512_mask_cmple_epu32_mask(_knot_mask16(ascii_bitmask), utf32, v_0000_07ff);
    __mmask16 three_bytes_bitmask = _mm512_mask_cmple_epu32_mask(_knot_mask16(_mm512_kor(ascii_bitmask, two_bytes_bitmask)), utf32, v_0000_ffff);

    size_t ascii_count = count_ones(ascii_bitmask);
    size_t two_bytes_count = count_ones(two_bytes_bitmask);
    size_t three_bytes_count = count_ones(three_bytes_bitmask);
    size_t four_bytes_count = 16 - ascii_count - two_bytes_count - three_bytes_count;
    count += ascii_count + 2*two_bytes_count + 3*three_bytes_count + 4*four_bytes_count;
  }

  return count + scalar::utf32::utf8_length_from_utf32(ptr, length - (ptr - input));
}

simdutf_warn_unused size_t implementation::utf16_length_from_utf32(const char32_t * input, size_t length) const noexcept {
  const char32_t* end = length >= 16 ? input + length - 16 : nullptr;
  const char32_t* ptr = input;

  const __m512i v_0000_ffff = _mm512_set1_epi32((uint32_t)0x0000ffff);

  size_t count{0};

  while (ptr <= end) {
    __m512i utf32 = _mm512_loadu_si512((const __m512i*)ptr);
    ptr += 16;
    __mmask16 surrogates_bitmask = _mm512_cmpgt_epu32_mask(utf32, v_0000_ffff);

    count += 16 + count_ones(surrogates_bitmask);
  }

  return count + scalar::utf32::utf16_length_from_utf32(ptr, length - (ptr - input));
}

simdutf_warn_unused size_t implementation::utf32_length_from_utf8(const char * input, size_t length) const noexcept {
  return implementation::count_utf8(input, length);
}

} // namespace SIMDUTF_IMPLEMENTATION
} // namespace simdutf

#include "simdutf/icelake/end.h"
