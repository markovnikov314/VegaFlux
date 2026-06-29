#include "aegis_decode/synthetic_itch.hpp"

#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace aegis_decode {
namespace {

struct Message {
  std::string event_type;
  std::int64_t event_ts_ns{};
  std::int64_t receive_lag_ns{};
  std::string side;
  std::int64_t price_ticks{};
  std::int64_t quantity{};
  std::uint64_t order_id{};
  std::uint64_t reference_order_id{};
};

std::uint64_t U64(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    value = (value << 8U) | bytes[offset + i];
  }
  return value;
}

std::int64_t I64(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  const auto raw = U64(bytes, offset);
  if (raw <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return static_cast<std::int64_t>(raw);
  }
  if (raw == (1ULL << 63U)) {
    return std::numeric_limits<std::int64_t>::min();
  }
  return -static_cast<std::int64_t>((~raw) + 1U);
}

std::string HexByte(std::uint8_t byte) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string out = "0x";
  out.push_back(kHex[(byte >> 4U) & 0x0fU]);
  out.push_back(kHex[byte & 0x0fU]);
  return out;
}

std::int64_t ReceiveTs(const Message& message) {
  if (message.receive_lag_ns > std::numeric_limits<std::int64_t>::max() - message.event_ts_ns) {
    throw std::runtime_error("receive timestamp overflow");
  }
  return message.event_ts_ns + message.receive_lag_ns;
}

Message DecodeMessage(const std::vector<std::uint8_t>& payload) {
  if (payload.size() != 50) {
    throw std::runtime_error("bad application payload size: " + std::to_string(payload.size()));
  }
  Message message;
  switch (payload[0]) {
    case 'A':
      message.event_type = "ADD";
      break;
    case 'E':
      message.event_type = "EXECUTE";
      break;
    case 'C':
      message.event_type = "CANCEL";
      break;
    case 'R':
      message.event_type = "REPLACE";
      break;
    case 'D':
      message.event_type = "DELETE";
      break;
    case 'B':
      message.event_type = "BROKEN_TRADE";
      break;
    case 'S':
      message.event_type = "SESSION_RESET";
      break;
    default:
      throw std::runtime_error("unsupported message type: " + HexByte(payload[0]));
  }
  message.event_ts_ns = I64(payload, 1);
  message.receive_lag_ns = I64(payload, 9);
  if (message.event_type == "SESSION_RESET") {
    message.side = "SIDE_UNSPECIFIED";
  } else {
    switch (payload[17]) {
      case 'B':
        message.side = "BID";
        break;
      case 'A':
        message.side = "ASK";
        break;
      default:
        throw std::runtime_error("unknown side: " + HexByte(payload[17]));
    }
  }
  message.price_ticks = I64(payload, 18);
  message.quantity = I64(payload, 26);
  message.order_id = U64(payload, 34);
  message.reference_order_id = U64(payload, 42);
  if (message.event_ts_ns < 0 || message.receive_lag_ns < 0 || message.price_ticks < 0 || message.quantity < 0) {
    throw std::runtime_error("negative timestamp/price/quantity");
  }
  if (message.event_type != "SESSION_RESET" && message.quantity == 0) {
    throw std::runtime_error("non-positive quantity");
  }
  (void)ReceiveTs(message);
  return message;
}

aegis_events::CanonicalEvent Canonical(const aegis_transport::Frame& frame, const Message& message) {
  return {
      message.event_type,
      message.event_ts_ns,
      ReceiveTs(message),
      frame.sequence_number,
      frame.session,
      message.side,
      message.price_ticks,
      message.quantity,
      message.order_id,
      message.reference_order_id,
      {{"fixture", "foundation"}, {"seed", "424242"}, {"source", "synthetic"}},
  };
}

aegis_events::CanonicalEvent Gap(const aegis_transport::Frame& frame,
                                 const aegis_events::CanonicalEvent& event,
                                 std::uint64_t expected) {
  const auto missing = frame.sequence_number - expected;
  return {
      "GAP_DETECTED",
      event.event_ts_ns,
      event.receive_ts_ns,
      expected,
      frame.session,
      "SIDE_UNSPECIFIED",
      0,
      static_cast<std::int64_t>(missing),
      0,
      0,
      {{"expected_sequence", std::to_string(expected)},
       {"fixture", "decoder"},
       {"missing_count", std::to_string(missing)},
       {"observed_sequence", std::to_string(frame.sequence_number)},
       {"source", "synthetic"}},
  };
}

class SyntheticTransportDecoder final : public ITransportDecoder {
 public:
  std::string_view name() const override { return kSyntheticTransportDecoderName; }
  aegis_transport::ReadResult Decode(const std::vector<std::uint8_t>& bytes) const override {
    return aegis_transport::ReadFrames(bytes);
  }
};

class SyntheticApplicationDecoder final : public IApplicationDecoder {
 public:
  std::string_view name() const override { return kSyntheticApplicationDecoderName; }
  aegis_events::CanonicalEvent Decode(const aegis_transport::Frame& frame) const override {
    return Canonical(frame, DecodeMessage(frame.payload));
  }
};

std::vector<std::uint8_t> ReadAll(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("failed to open input: " + path.string());
  }
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void WriteText(const std::filesystem::path& path, const std::string& text) {
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw std::runtime_error("failed to open output: " + path.string());
  }
  out << text;
}

}  // namespace

const ITransportDecoder& TransportDecoderByName(std::string_view name) {
  static const SyntheticTransportDecoder kSynthetic;
  if (name == kSynthetic.name()) {
    return kSynthetic;
  }
  throw std::runtime_error("unknown transport decoder: " + std::string(name));
}

const IApplicationDecoder& ApplicationDecoderByName(std::string_view name) {
  static const SyntheticApplicationDecoder kSynthetic;
  if (name == kSynthetic.name()) {
    return kSynthetic;
  }
  throw std::runtime_error("unknown application decoder: " + std::string(name));
}

DecodeResult DecodeBytes(const std::vector<std::uint8_t>& bytes,
                         std::string_view transport_decoder,
                         std::string_view application_decoder) {
  DecodeResult result;
  const auto& transport = TransportDecoderByName(transport_decoder);
  const auto& application = ApplicationDecoderByName(application_decoder);
  auto frames = transport.Decode(bytes);
  result.quarantine = std::move(frames.quarantine);
  std::unordered_map<std::string, std::uint64_t> expected_by_session;

  for (const auto& frame : frames.frames) {
    const auto expected_it = expected_by_session.find(frame.session);
    const auto expected = expected_it == expected_by_session.end() ? 1ULL : expected_it->second;

    aegis_events::CanonicalEvent event;
    try {
      event = application.Decode(frame);
    } catch (const std::exception& error) {
      result.quarantine.push_back({frame.offset, error.what(), frame.raw, frame.sequence_number, frame.session});
      if (frame.sequence_number >= expected) {
        expected_by_session[frame.session] = frame.sequence_number + 1;
      }
      continue;
    }

    if (event.event_type == "SESSION_RESET") {
      result.events.push_back(std::move(event));
      expected_by_session[frame.session] = 1;
      continue;
    }
    if (frame.sequence_number < expected) {
      result.quarantine.push_back({frame.offset, "non_monotonic_sequence", frame.raw, frame.sequence_number, frame.session});
      continue;
    }
    if (frame.sequence_number > expected) {
      result.events.push_back(Gap(frame, event, expected));
      ++result.gaps;
    }
    result.events.push_back(std::move(event));
    expected_by_session[frame.session] = frame.sequence_number + 1;
  }
  return result;
}

DecodeResult DecodeFile(const std::filesystem::path& input,
                        const std::filesystem::path& output,
                        const std::filesystem::path* quarantine_output,
                        std::string_view transport_decoder,
                        std::string_view application_decoder) {
  auto result = DecodeBytes(ReadAll(input), transport_decoder, application_decoder);
  WriteText(output, aegis_events::Jsonl(result.events));
  if (quarantine_output != nullptr) {
    WriteText(*quarantine_output, aegis_events::QuarantineJsonl(result.quarantine));
  }
  return result;
}

}  // namespace aegis_decode
