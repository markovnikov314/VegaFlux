#include "aegis_transport/synthetic.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace aegis_transport {
namespace {

constexpr unsigned char kMagic[] = {'V', 'F', 'L', 'X'};
constexpr std::size_t kMagicSize = 4;
constexpr std::size_t kLenSize = 2;
constexpr std::size_t kSessionHeaderSize = 9;

std::uint16_t U16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  return static_cast<std::uint16_t>((bytes[offset] << 8U) | bytes[offset + 1]);
}

std::uint64_t U64(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    value = (value << 8U) | bytes[offset + i];
  }
  return value;
}

std::vector<std::uint8_t> Slice(const std::vector<std::uint8_t>& bytes, std::size_t first, std::size_t last) {
  first = std::min(first, bytes.size());
  last = std::min(last, bytes.size());
  return {bytes.begin() + static_cast<std::ptrdiff_t>(first), bytes.begin() + static_cast<std::ptrdiff_t>(last)};
}

void AddQuarantine(ReadResult& result, std::size_t offset, std::string reason, std::vector<std::uint8_t> raw) {
  result.quarantine.push_back({offset, std::move(reason), std::move(raw), std::nullopt, std::nullopt});
}

}  // namespace

ReadResult ReadFrames(const std::vector<std::uint8_t>& bytes) {
  ReadResult result;
  if (bytes.size() < kMagicSize || !std::equal(std::begin(kMagic), std::end(kMagic), bytes.begin())) {
    AddQuarantine(result, 0, "bad_magic", bytes);
    return result;
  }

  std::size_t offset = kMagicSize;
  while (offset < bytes.size()) {
    if (offset + kLenSize > bytes.size()) {
      AddQuarantine(result, offset, "truncated_frame_length", Slice(bytes, offset, bytes.size()));
      break;
    }
    const std::size_t frame_start = offset;
    const std::size_t frame_len = U16(bytes, offset);
    offset += kLenSize;
    const std::size_t frame_end = offset + frame_len;
    if (frame_len < kSessionHeaderSize || frame_end > bytes.size()) {
      AddQuarantine(result, frame_start, "truncated_frame", Slice(bytes, frame_start, bytes.size()));
      break;
    }

    const auto raw = Slice(bytes, frame_start, frame_end);
    const auto sequence_number = U64(bytes, offset);
    const auto session_len = bytes[offset + 8];
    const std::size_t session_start = offset + kSessionHeaderSize;
    const std::size_t session_end = session_start + session_len;
    if (sequence_number == 0 || session_len == 0 || session_end > frame_end) {
      result.quarantine.push_back({frame_start, "bad_session_header", raw, sequence_number, std::nullopt});
      offset = frame_end;
      continue;
    }
    bool ascii = true;
    for (std::size_t i = session_start; i < session_end; ++i) {
      ascii = ascii && bytes[i] <= 0x7f;
    }
    if (!ascii) {
      result.quarantine.push_back({frame_start, "non_ascii_session", raw, sequence_number, std::nullopt});
      offset = frame_end;
      continue;
    }
    std::string session(bytes.begin() + static_cast<std::ptrdiff_t>(session_start),
                        bytes.begin() + static_cast<std::ptrdiff_t>(session_end));
    auto payload = Slice(bytes, session_end, frame_end);
    if (payload.empty()) {
      result.quarantine.push_back({frame_start, "empty_payload", raw, sequence_number, session});
      offset = frame_end;
      continue;
    }
    result.frames.push_back({sequence_number, std::move(session), std::move(payload), raw, frame_start});
    offset = frame_end;
  }
  return result;
}

std::string Hex(const std::vector<std::uint8_t>& bytes) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (const auto byte : bytes) {
    out << std::setw(2) << static_cast<int>(byte);
  }
  return out.str();
}

}  // namespace aegis_transport
