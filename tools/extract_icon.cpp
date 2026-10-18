/**
 * @file    tools/extract_icon.cpp
 * @brief   Build-time extractor for the game's title icon: writes the PNG as a
 *          byte array (src/icon.generated.h) and wrapped in a one-image .ico
 *          (src/icon.generated.ico) for the executables' icon resource.
 *
 * The icon is the title icon entry of the XDBF blob that default.xex carries
 * as a resource named after its title ID, so nothing besides the XEX (already
 * a codegen input) is needed. rr6's scripts/gen-icon.py does the same in
 * Python but only for uncompressed or basic-compressed XEXes; Sylpheed's is
 * LZX compressed, so this decompresses it with the SDK's lzx_decompress the
 * way XexModule::ReadImageCompressed does.
 *
 *   sylpheed_icon <default.xex> <out .h> <out .ico>
 */
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include <rex/system/lzx.h>

namespace {

using Bytes = std::vector<uint8_t>;

uint16_t Be16(const Bytes &d, size_t o) {
  if (o + 2 > d.size())
    throw std::runtime_error("read past the end of the XEX");
  return uint16_t(d[o] << 8 | d[o + 1]);
}

uint32_t Be32(const Bytes &d, size_t o) {
  if (o + 4 > d.size())
    throw std::runtime_error("read past the end of the XEX");
  return uint32_t(d[o]) << 24 | uint32_t(d[o + 1]) << 16 | uint32_t(d[o + 2]) << 8 |
         d[o + 3];
}

uint64_t Be64(const Bytes &d, size_t o) {
  return uint64_t(Be32(d, o)) << 32 | Be32(d, o + 4);
}

// --- AES-128 decrypt, CBC with a zero IV (the XEX image encryption) --------

struct Aes128 {
  std::array<uint8_t, 256> sbox{}, inv{};
  std::array<uint8_t, 176> round_keys{};

  static uint8_t Mul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    for (int i = 0; i < 8; ++i) {
      if (b & 1)
        p ^= a;
      const bool hi = a & 0x80;
      a = uint8_t(a << 1);
      if (hi)
        a ^= 0x1B;
      b >>= 1;
    }
    return p;
  }

  explicit Aes128(const uint8_t key[16]) {
    // S-box from the multiplicative inverse and the affine transform.
    for (int a = 0; a < 256; ++a) {
      uint8_t x = 0;
      for (int b = 1; a && b < 256; ++b)
        if (Mul(uint8_t(a), uint8_t(b)) == 1) {
          x = uint8_t(b);
          break;
        }
      uint8_t s = x, r = x;
      for (int i = 0; i < 4; ++i) {
        r = uint8_t(r << 1 | r >> 7);
        s ^= r;
      }
      sbox[a] = s ^ 0x63;
    }
    for (int a = 0; a < 256; ++a)
      inv[sbox[a]] = uint8_t(a);
    // Key expansion.
    std::memcpy(round_keys.data(), key, 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
      uint8_t t[4] = {round_keys[i - 4], round_keys[i - 3], round_keys[i - 2],
                      round_keys[i - 1]};
      if (i % 16 == 0) {
        const uint8_t first = t[0];
        t[0] = uint8_t(sbox[t[1]] ^ rcon);
        t[1] = sbox[t[2]];
        t[2] = sbox[t[3]];
        t[3] = sbox[first];
        rcon = Mul(rcon, 2);
      }
      for (int j = 0; j < 4; ++j)
        round_keys[i + j] = round_keys[i - 16 + j] ^ t[j];
    }
  }

  void DecryptBlock(const uint8_t in[16], uint8_t out[16]) const {
    uint8_t s[16];
    for (int i = 0; i < 16; ++i)
      s[i] = in[i] ^ round_keys[160 + i];
    for (int round = 9; round >= 0; --round) {
      // Inverse shift rows (column-major state), then inverse sub bytes.
      uint8_t t[16];
      for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
          t[r + 4 * ((c + r) % 4)] = s[r + 4 * c];
      for (int i = 0; i < 16; ++i)
        s[i] = inv[t[i]] ^ round_keys[round * 16 + i];
      if (round == 0)
        break;
      for (int c = 0; c < 4; ++c) {
        const uint8_t *col = s + 4 * c;
        const uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
        s[4 * c + 0] = Mul(a0, 14) ^ Mul(a1, 11) ^ Mul(a2, 13) ^ Mul(a3, 9);
        s[4 * c + 1] = Mul(a0, 9) ^ Mul(a1, 14) ^ Mul(a2, 11) ^ Mul(a3, 13);
        s[4 * c + 2] = Mul(a0, 13) ^ Mul(a1, 9) ^ Mul(a2, 14) ^ Mul(a3, 11);
        s[4 * c + 3] = Mul(a0, 11) ^ Mul(a1, 13) ^ Mul(a2, 9) ^ Mul(a3, 14);
      }
    }
    std::memcpy(out, s, 16);
  }

  void DecryptCbc(uint8_t *data, size_t size) const {
    uint8_t prev[16] = {}, block[16];
    for (size_t i = 0; i + 16 <= size; i += 16) {
      std::memcpy(block, data + i, 16);
      DecryptBlock(block, data + i);
      for (int j = 0; j < 16; ++j)
        data[i + j] ^= prev[j];
      std::memcpy(prev, block, 16);
    }
  }
};

// The retail XEX2 key the per-title image key is wrapped with.
constexpr uint8_t kRetailKey[16] = {0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3,
                                    0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91};

constexpr uint32_t kHeaderResourceInfo = 0x000002FF;
constexpr uint32_t kHeaderFileFormatInfo = 0x000003FF;
constexpr uint32_t kHeaderExecutionInfo = 0x00040006;

// The XEX's memory image: decrypted and decompressed.
Bytes LoadImage(const Bytes &xex, uint32_t &load_address,
                std::vector<std::pair<uint32_t, uint32_t>> &headers) {
  if (xex.size() < 0x18 || std::memcmp(xex.data(), "XEX2", 4) != 0)
    throw std::runtime_error("not an XEX2 file");
  const uint32_t header_size = Be32(xex, 0x8);
  const uint32_t security = Be32(xex, 0x10);
  const uint32_t count = Be32(xex, 0x14);
  for (uint32_t i = 0; i < count; ++i)
    headers.emplace_back(Be32(xex, 0x18 + i * 8), Be32(xex, 0x1C + i * 8));
  auto header = [&](uint32_t key) {
    for (const auto &[k, v] : headers)
      if (k == key)
        return v;
    throw std::runtime_error("XEX optional header missing");
  };

  const uint32_t image_size = Be32(xex, security + 0x4);
  load_address = Be32(xex, security + 0x110);

  const uint32_t ffi = header(kHeaderFileFormatInfo);
  const uint32_t info_size = Be32(xex, ffi);
  const uint16_t encryption = Be16(xex, ffi + 4);
  const uint16_t compression = Be16(xex, ffi + 6);

  Bytes body(xex.begin() + header_size, xex.end());
  if (encryption == 1) {
    uint8_t session_key[16];
    Aes128(kRetailKey).DecryptBlock(&xex[security + 0x150], session_key);
    Aes128(session_key).DecryptCbc(body.data(), body.size() & ~size_t(15));
  } else if (encryption != 0) {
    throw std::runtime_error("unsupported XEX encryption type");
  }

  Bytes image(image_size, 0);
  switch (compression) {
  case 0: // none
    std::memcpy(image.data(), body.data(), std::min<size_t>(body.size(), image_size));
    break;
  case 1: { // basic: alternating (data, zero fill) runs
    size_t in = 0, out = 0;
    for (uint32_t p = ffi + 8; p + 8 <= ffi + info_size; p += 8) {
      const uint32_t data = Be32(xex, p), zero = Be32(xex, p + 4);
      if (in + data > body.size() || out + data + zero > image.size())
        throw std::runtime_error("basic compression block out of range");
      std::memcpy(image.data() + out, body.data() + in, data);
      in += data;
      out += data + zero;
    }
    break;
  }
  case 2: { // normal: LZX over chunks of SHA-1 chained blocks
    const uint32_t window_size = Be32(xex, ffi + 8);
    uint32_t block_size = Be32(xex, ffi + 12);
    Bytes stream;
    size_t p = 0;
    while (block_size) {
      if (p + block_size > body.size())
        throw std::runtime_error("compressed block out of range");
      const size_t next = p + block_size;
      const uint32_t next_size = Be32(body, p);
      size_t q = p + 4 + 20; // the next block's size and hash
      while (true) {
        const uint16_t chunk = Be16(body, q);
        q += 2;
        if (!chunk)
          break;
        if (q + chunk > next)
          throw std::runtime_error("compressed chunk out of range");
        stream.insert(stream.end(), body.begin() + q, body.begin() + q + chunk);
        q += chunk;
      }
      p = next;
      block_size = next_size;
    }
    if (lzx_decompress(stream.data(), stream.size(), image.data(), image.size(),
                       window_size, nullptr, 0) != 0)
      throw std::runtime_error("LZX decompression failed (wrong key?)");
    break;
  }
  default:
    throw std::runtime_error("unsupported XEX compression type");
  }
  return image;
}

// The title icon PNG from the XDBF resource named after the title ID.
Bytes ExtractIcon(const Bytes &xex) {
  uint32_t load_address = 0;
  std::vector<std::pair<uint32_t, uint32_t>> headers;
  const Bytes image = LoadImage(xex, load_address, headers);
  auto header = [&](uint32_t key) {
    for (const auto &[k, v] : headers)
      if (k == key)
        return v;
    throw std::runtime_error("XEX optional header missing");
  };

  char name[9];
  std::snprintf(name, sizeof(name), "%08X", Be32(xex, header(kHeaderExecutionInfo) + 0xC));
  const uint32_t res = header(kHeaderResourceInfo);
  const uint32_t res_count = (Be32(xex, res) - 4) / 16;
  uint32_t address = 0, size = 0;
  for (uint32_t i = 0; i < res_count; ++i) {
    const size_t e = res + 4 + i * 16;
    if (std::strncmp(reinterpret_cast<const char *>(&xex[e]), name, 8) == 0) {
      address = Be32(xex, e + 8);
      size = Be32(xex, e + 12);
      break;
    }
  }
  if (!size || address < load_address || address - load_address + size > image.size())
    throw std::runtime_error(std::string("no XEX resource named ") + name);
  const Bytes xdbf(image.begin() + (address - load_address),
                   image.begin() + (address - load_address + size));

  if (Be32(xdbf, 0) != 0x58444246) // 'XDBF'
    throw std::runtime_error("title resource is not an XDBF blob");
  const uint32_t entry_count = Be32(xdbf, 8), entry_used = Be32(xdbf, 12);
  const uint32_t free_count = Be32(xdbf, 16);
  const size_t content = 24 + 18 * size_t(entry_count) + 8 * size_t(free_count);
  for (uint32_t i = 0; i < entry_used; ++i) {
    const size_t e = 24 + i * 18;
    const uint16_t section = Be16(xdbf, e);
    const uint64_t id = Be64(xdbf, e + 2);
    const uint32_t offset = Be32(xdbf, e + 10), length = Be32(xdbf, e + 14);
    if (section == 2 && id == 0x8000) { // image section, title icon
      if (content + offset + length > xdbf.size())
        throw std::runtime_error("title icon out of range");
      return Bytes(xdbf.begin() + content + offset,
                   xdbf.begin() + content + offset + length);
    }
  }
  throw std::runtime_error("no title icon in the XDBF resource");
}

// A one-image .ico around the PNG (Vista+ takes PNG payloads as they are).
void WriteIco(const Bytes &png, const char *path) {
  const uint32_t w = Be32(png, 16), h = Be32(png, 20);
  auto put16 = [](Bytes &b, uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); };
  auto put32 = [&](Bytes &b, uint32_t v) { put16(b, uint16_t(v)); put16(b, uint16_t(v >> 16)); };
  Bytes ico;
  put16(ico, 0);
  put16(ico, 1);
  put16(ico, 1);
  ico.push_back(uint8_t(w >= 256 ? 0 : w));
  ico.push_back(uint8_t(h >= 256 ? 0 : h));
  ico.push_back(0); // no palette
  ico.push_back(0);
  put16(ico, 1);  // planes
  put16(ico, 32); // bits per pixel
  put32(ico, uint32_t(png.size()));
  put32(ico, 6 + 16);
  ico.insert(ico.end(), png.begin(), png.end());
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(ico.data()),
                                              std::streamsize(ico.size()));
}

void WriteHeader(const Bytes &png, const char *path) {
  std::string out =
      "#pragma once\n\n"
      "// Generated by tools/extract_icon.cpp from assets/default.xex's title icon.\n"
      "// Rebuilt with the XEX; do not edit or commit.\n\n"
      "namespace icon {\n\n"
      "inline constexpr unsigned char kIconPNG[] = {\n";
  for (size_t i = 0; i < png.size(); i += 20) {
    out += "    ";
    for (size_t j = i; j < std::min(png.size(), i + 20); ++j)
      out += std::to_string(png[j]) + ", ";
    out += "\n";
  }
  out += "};\ninline constexpr unsigned int kIconPNGSize = sizeof(kIconPNG);\n\n"
         "} // namespace icon\n";
  std::ofstream(path, std::ios::binary) << out;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: %s <default.xex> <out .h> <out .ico>\n", argv[0]);
    return 2;
  }
  try {
    std::ifstream f(argv[1], std::ios::binary);
    if (!f)
      throw std::runtime_error(std::string("cannot open ") + argv[1]);
    const Bytes xex((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const Bytes png = ExtractIcon(xex);
    if (png.size() < 24 || std::memcmp(png.data(), "\x89PNG", 4) != 0)
      throw std::runtime_error("title icon is not a PNG");
    WriteHeader(png, argv[2]);
    WriteIco(png, argv[3]);
    std::printf("title icon: %zu byte PNG, %ux%u\n", png.size(), Be32(png, 16), Be32(png, 20));
  } catch (const std::exception &e) {
    std::fprintf(stderr, "extract_icon: %s\n", e.what());
    return 1;
  }
  return 0;
}
