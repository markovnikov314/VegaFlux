#pragma once

#include "aegis_events/canonical_jsonl.hpp"
#include "aegis_transport/synthetic.hpp"

#include <filesystem>
#include <string_view>
#include <vector>

namespace aegis_decode {

inline constexpr std::string_view kSyntheticTransportDecoderName = "synthetic_transport_v1";
inline constexpr std::string_view kSyntheticApplicationDecoderName = "synthetic_application_v1";

struct DecodeResult {
  std::vector<aegis_events::CanonicalEvent> events;
  std::vector<aegis_transport::QuarantineRecord> quarantine;
  std::size_t gaps{};
};

class ITransportDecoder {
 public:
  virtual ~ITransportDecoder() = default;
  virtual std::string_view name() const = 0;
  virtual aegis_transport::ReadResult Decode(const std::vector<std::uint8_t>& bytes) const = 0;
};

class IApplicationDecoder {
 public:
  virtual ~IApplicationDecoder() = default;
  virtual std::string_view name() const = 0;
  virtual aegis_events::CanonicalEvent Decode(const aegis_transport::Frame& frame) const = 0;
};

const ITransportDecoder& TransportDecoderByName(std::string_view name);
const IApplicationDecoder& ApplicationDecoderByName(std::string_view name);

DecodeResult DecodeBytes(const std::vector<std::uint8_t>& bytes,
                         std::string_view transport_decoder = kSyntheticTransportDecoderName,
                         std::string_view application_decoder = kSyntheticApplicationDecoderName);
DecodeResult DecodeFile(const std::filesystem::path& input,
                        const std::filesystem::path& output,
                        const std::filesystem::path* quarantine_output = nullptr,
                        std::string_view transport_decoder = kSyntheticTransportDecoderName,
                        std::string_view application_decoder = kSyntheticApplicationDecoderName);

}  // namespace aegis_decode
