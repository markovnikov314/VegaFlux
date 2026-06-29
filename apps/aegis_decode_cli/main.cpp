#include "aegis_decode/synthetic_itch.hpp"
#include "aegis_events/canonical_jsonl.hpp"

#include <array>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::filesystem::path Resolve(const std::filesystem::path& root, const std::filesystem::path& path) {
  return path.is_absolute() ? path : root / path;
}

std::string Arg(int& index, int argc, char** argv) {
  if (index + 1 >= argc) {
    throw std::runtime_error(std::string("missing value for ") + argv[index]);
  }
  return argv[++index];
}

std::vector<std::uint8_t> ReadAll(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("failed to open input: " + path.string());
  }
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("failed to open input: " + path.string());
  }
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void Require(bool ok, const std::string& message) {
  if (!ok) {
    throw std::runtime_error(message);
  }
}

std::string JsonEscape(const std::string& value) {
  std::string out;
  constexpr char kHex[] = "0123456789abcdef";
  for (const char ch : value) {
    const auto byte = static_cast<unsigned char>(ch);
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (byte < 0x20) {
          out += "\\u00";
          out.push_back(kHex[(byte >> 4U) & 0x0fU]);
          out.push_back(kHex[byte & 0x0fU]);
        } else {
          out.push_back(ch);
        }
    }
  }
  return out;
}

std::uint32_t RotR(std::uint32_t value, int shift) {
  return (value >> shift) | (value << (32 - shift));
}

std::string Sha256Hex(const std::vector<std::uint8_t>& bytes) {
  static constexpr std::array<std::uint32_t, 64> k = {
      0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
      0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
      0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
      0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
      0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
      0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
      0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
      0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
      0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
      0xc67178f2U};
  std::array<std::uint32_t, 8> h = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                    0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  auto message = bytes;
  const auto bit_len = static_cast<std::uint64_t>(message.size()) * 8U;
  message.push_back(0x80U);
  while ((message.size() % 64U) != 56U) {
    message.push_back(0U);
  }
  for (int i = 7; i >= 0; --i) {
    message.push_back(static_cast<std::uint8_t>((bit_len >> (i * 8)) & 0xffU));
  }
  for (std::size_t chunk = 0; chunk < message.size(); chunk += 64U) {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i) {
      const auto j = chunk + i * 4U;
      w[i] = (static_cast<std::uint32_t>(message[j]) << 24U) | (static_cast<std::uint32_t>(message[j + 1]) << 16U) |
             (static_cast<std::uint32_t>(message[j + 2]) << 8U) | static_cast<std::uint32_t>(message[j + 3]);
    }
    for (std::size_t i = 16; i < 64; ++i) {
      const auto s0 = RotR(w[i - 15], 7) ^ RotR(w[i - 15], 18) ^ (w[i - 15] >> 3U);
      const auto s1 = RotR(w[i - 2], 17) ^ RotR(w[i - 2], 19) ^ (w[i - 2] >> 10U);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    auto a = h[0];
    auto b = h[1];
    auto c = h[2];
    auto d = h[3];
    auto e = h[4];
    auto f = h[5];
    auto g = h[6];
    auto hh = h[7];
    for (std::size_t i = 0; i < 64; ++i) {
      const auto s1 = RotR(e, 6) ^ RotR(e, 11) ^ RotR(e, 25);
      const auto ch = (e & f) ^ ((~e) & g);
      const auto temp1 = hh + s1 + ch + k[i] + w[i];
      const auto s0 = RotR(a, 2) ^ RotR(a, 13) ^ RotR(a, 22);
      const auto maj = (a & b) ^ (a & c) ^ (b & c);
      const auto temp2 = s0 + maj;
      hh = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
  }
  constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (const auto word : h) {
    for (int shift = 28; shift >= 0; shift -= 4) {
      out.push_back(kHex[(word >> shift) & 0x0fU]);
    }
  }
  return out;
}

void CheckManifestEntry(const std::filesystem::path& root, const std::string& relative_path, const std::string& manifest) {
  const auto bytes = ReadAll(Resolve(root, relative_path));
  const auto sha = Sha256Hex(bytes);
  Require(manifest.find('"' + relative_path + '"') != std::string::npos, "manifest missing fixture: " + relative_path);
  Require(manifest.find("\"bytes\": " + std::to_string(bytes.size())) != std::string::npos,
          "manifest byte count mismatch: " + relative_path);
  Require(manifest.find("\"sha256\": \"" + sha + '"') != std::string::npos, "manifest sha256 mismatch: " + relative_path);
}

bool HasQuarantineReason(const std::vector<aegis_transport::QuarantineRecord>& records, const std::string& needle) {
  return std::any_of(records.begin(), records.end(), [&needle](const auto& record) {
    return record.reason.find(needle) != std::string::npos;
  });
}

void CheckFixtures(const std::filesystem::path& root,
                   std::string_view transport_decoder,
                   std::string_view application_decoder) {
  const auto manifest = ReadText(Resolve(root, "data_contracts/manifests/decoder_fixture_manifest.json"));
  CheckManifestEntry(root, "data_contracts/fixtures/synthetic_itch.bin", manifest);
  CheckManifestEntry(root, "data_contracts/fixtures/synthetic_itch_gap.bin", manifest);
  CheckManifestEntry(root, "data_contracts/fixtures/synthetic_itch_malformed.bin", manifest);
  CheckManifestEntry(root, "data_contracts/fixtures/synthetic_itch_extended.bin", manifest);
  CheckManifestEntry(root, "data_contracts/fixtures/synthetic_itch_fuzz_lite.bin", manifest);

  const auto normal =
      aegis_decode::DecodeBytes(ReadAll(Resolve(root, "data_contracts/fixtures/synthetic_itch.bin")), transport_decoder,
                                application_decoder);
  Require(normal.events.size() == 5 && normal.gaps == 0 && normal.quarantine.empty(), "normal fixture check failed");
  Require(aegis_events::Jsonl(normal.events) == ReadText(Resolve(root, "data_contracts/fixtures/normalized_events.golden.jsonl")),
          "normal fixture golden mismatch");

  const auto gap =
      aegis_decode::DecodeBytes(ReadAll(Resolve(root, "data_contracts/fixtures/synthetic_itch_gap.bin")), transport_decoder,
                                application_decoder);
  Require(gap.events.size() == 5 && gap.gaps == 1 && gap.quarantine.empty(), "gap fixture check failed");

  const auto malformed = aegis_decode::DecodeBytes(
      ReadAll(Resolve(root, "data_contracts/fixtures/synthetic_itch_malformed.bin")), transport_decoder, application_decoder);
  Require(malformed.events.size() == 1 && malformed.quarantine.size() == 1, "malformed fixture check failed");

  const auto extended = aegis_decode::DecodeBytes(
      ReadAll(Resolve(root, "data_contracts/fixtures/synthetic_itch_extended.bin")), transport_decoder, application_decoder);
  Require(extended.events.size() == 8 && extended.gaps == 0 && extended.quarantine.empty(), "extended fixture check failed");
  Require(extended.events[1].event_type == "REPLACE" && extended.events[2].event_type == "DELETE" &&
              extended.events[5].event_type == "BROKEN_TRADE" && extended.events[6].event_type == "SESSION_RESET" &&
              extended.events[7].sequence_number == 1,
          "extended fixture event semantics failed");

  const auto fuzz = aegis_decode::DecodeBytes(
      ReadAll(Resolve(root, "data_contracts/fixtures/synthetic_itch_fuzz_lite.bin")), transport_decoder, application_decoder);
  Require(fuzz.events.size() == 1 && fuzz.quarantine.size() == 4, "fuzz-lite fixture check failed");
  Require(HasQuarantineReason(fuzz.quarantine, "unsupported message type") && HasQuarantineReason(fuzz.quarantine, "unknown side") &&
              HasQuarantineReason(fuzz.quarantine, "negative timestamp/price/quantity") &&
              HasQuarantineReason(fuzz.quarantine, "truncated_frame"),
          "fuzz-lite quarantine reasons missing");
}

}  // namespace

int main(int argc, char** argv) {
  try {
    std::filesystem::path root = std::filesystem::current_path();
    std::filesystem::path input = "data_contracts/fixtures/synthetic_itch.bin";
    std::optional<std::filesystem::path> output;
    std::optional<std::filesystem::path> quarantine;
    std::string transport_decoder(aegis_decode::kSyntheticTransportDecoderName);
    std::string application_decoder(aegis_decode::kSyntheticApplicationDecoderName);
    bool check_fixtures = false;

    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--root") {
        root = Arg(i, argc, argv);
      } else if (arg == "--input") {
        input = Arg(i, argc, argv);
      } else if (arg == "--output") {
        output = Arg(i, argc, argv);
      } else if (arg == "--quarantine") {
        quarantine = Arg(i, argc, argv);
      } else if (arg == "--transport-decoder") {
        transport_decoder = Arg(i, argc, argv);
      } else if (arg == "--application-decoder") {
        application_decoder = Arg(i, argc, argv);
      } else if (arg == "--check-fixtures") {
        check_fixtures = true;
      } else {
        throw std::runtime_error("unknown argument: " + arg);
      }
    }

    if (check_fixtures) {
      CheckFixtures(root, transport_decoder, application_decoder);
      std::cout << "{\"checked_fixtures\":5,\"root\":\"" << JsonEscape(root.string()) << "\",\"status\":\"pass\"}\n";
      return 0;
    }

    if (!output.has_value()) {
      throw std::runtime_error("--output is required");
    }
    auto input_path = Resolve(root, input);
    auto output_path = Resolve(root, *output);
    auto quarantine_path = quarantine.has_value() ? Resolve(root, *quarantine) : std::filesystem::path();
    auto result = aegis_decode::DecodeFile(input_path, output_path, quarantine.has_value() ? &quarantine_path : nullptr,
                                           transport_decoder, application_decoder);
    std::cout << "{\"events\":" << result.events.size() << ",\"gaps\":" << result.gaps << ",\"input\":\""
              << JsonEscape(input_path.string()) << "\",\"quarantined\":" << result.quarantine.size() << ",\"status\":\"pass\"}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "aegis_decode_cli: " << error.what() << '\n';
    return 1;
  }
}
