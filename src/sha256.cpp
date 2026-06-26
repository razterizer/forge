#include "sha256.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace forge
{
  namespace
  {

    constexpr std::array<std::uint32_t, 64> constants {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
      0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
      0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
      0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };

    std::uint32_t rotate_right(std::uint32_t value, unsigned int count)
    {
      return (value >> count) | (value << (32 - count));
    }

    void process_block(const std::array<std::uint8_t, 64>& block,
                       std::array<std::uint32_t, 8>& state)
    {
      std::array<std::uint32_t, 64> words {};

      for (std::size_t index = 0; index < 16; ++index)
      {
        const auto offset = index * 4;
        words[index] =
          (static_cast<std::uint32_t>(block[offset]) << 24)
          | (static_cast<std::uint32_t>(block[offset + 1]) << 16)
          | (static_cast<std::uint32_t>(block[offset + 2]) << 8)
          | static_cast<std::uint32_t>(block[offset + 3]);
      }

      for (std::size_t index = 16; index < words.size(); ++index)
      {
        const auto first =
          rotate_right(words[index - 15], 7)
          ^ rotate_right(words[index - 15], 18)
          ^ (words[index - 15] >> 3);
        const auto second =
          rotate_right(words[index - 2], 17)
          ^ rotate_right(words[index - 2], 19)
          ^ (words[index - 2] >> 10);
        words[index] = words[index - 16] + first + words[index - 7] + second;
      }

      auto working = state;

      for (std::size_t index = 0; index < words.size(); ++index)
      {
        const auto sum_one =
          rotate_right(working[4], 6)
          ^ rotate_right(working[4], 11)
          ^ rotate_right(working[4], 25);
        const auto choice = (working[4] & working[5]) ^ (~working[4] & working[6]);
        const auto first = working[7] + sum_one + choice + constants[index] + words[index];
        const auto sum_zero =
          rotate_right(working[0], 2)
          ^ rotate_right(working[0], 13)
          ^ rotate_right(working[0], 22);
        const auto majority =
          (working[0] & working[1])
          ^ (working[0] & working[2])
          ^ (working[1] & working[2]);
        const auto second = sum_zero + majority;

        working[7] = working[6];
        working[6] = working[5];
        working[5] = working[4];
        working[4] = working[3] + first;
        working[3] = working[2];
        working[2] = working[1];
        working[1] = working[0];
        working[0] = first + second;
      }

      for (std::size_t index = 0; index < state.size(); ++index)
        state[index] += working[index];
    }

  } // namespace

  bool sha256_file(const std::filesystem::path& path,
                   std::string& checksum,
                   std::ostream& error)
  {
    std::ifstream file { path, std::ios::binary };

    if (!file)
    {
      error << "forge: could not read '" << path.string() << "'\n";
      return false;
    }

    std::array<std::uint32_t, 8> state {
      0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    std::array<std::uint8_t, 64> block {};
    std::uint64_t byte_count = 0;

    while (file)
    {
      file.read(reinterpret_cast<char*>(block.data()), block.size());
      const auto count = static_cast<std::size_t>(file.gcount());
      byte_count += count;

      if (file.bad())
      {
        error << "forge: could not read '" << path.string() << "'\n";
        return false;
      }

      if (count == block.size())
      {
        process_block(block, state);
        continue;
      }

      block[count] = 0x80;

      if (count >= 56)
      {
        std::fill(block.begin() + count + 1, block.end(), 0);
        process_block(block, state);
        block.fill(0);
      }
      else
        std::fill(block.begin() + count + 1, block.end(), 0);

      const auto bit_count = byte_count * 8;

      for (std::size_t index = 0; index < 8; ++index)
        block[63 - index] = static_cast<std::uint8_t>(bit_count >> (index * 8));

      process_block(block, state);
      break;
    }

    std::ostringstream result;
    result << std::hex << std::setfill('0');

    for (const auto value : state)
      result << std::setw(8) << value;

    checksum = result.str();
    return true;
  }

} // namespace forge
