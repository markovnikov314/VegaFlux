#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aegis_transport {

struct QuarantineRecord {
  std::size_t offset{};
  std::string reason;
  std::vector<std::uint8_t> raw;
  std::optional<std::uint64_t> sequence_number;
  std::optional<std::string> session;
};

struct Frame {
  std::uint64_t sequence_number{};
  std::string session;
  std::vector<std::uint8_t> payload;
  std::vector<std::uint8_t> raw;
  std::size_t offset{};
};

struct ReadResult {
  std::vector<Frame> frames;
  std::vector<QuarantineRecord> quarantine;
};

ReadResult ReadFrames(const std::vector<std::uint8_t>& bytes);
std::string Hex(const std::vector<std::uint8_t>& bytes);

}  // namespace aegis_transport
