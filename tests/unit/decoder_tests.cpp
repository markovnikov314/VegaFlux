#include "aegis_decode/synthetic_itch.hpp"
#include "aegis_events/canonical_jsonl.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> ReadBytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("failed to read " + path.string());
  }
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("failed to read " + path.string());
  }
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void Require(bool ok, const std::string& message) {
  if (!ok) {
    throw std::runtime_error(message);
  }
}

std::uint16_t BeU16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  return static_cast<std::uint16_t>((bytes[offset] << 8U) | bytes[offset + 1]);
}

std::uint64_t BeU64(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    value = (value << 8U) | bytes[offset + i];
  }
  return value;
}

std::int64_t BeI64(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  const auto raw = BeU64(bytes, offset);
  if (raw <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return static_cast<std::int64_t>(raw);
  }
  if (raw == (1ULL << 63U)) {
    return std::numeric_limits<std::int64_t>::min();
  }
  return -static_cast<std::int64_t>((~raw) + 1U);
}

bool HasReason(const std::vector<aegis_transport::QuarantineRecord>& records, const std::string& needle) {
  for (const auto& record : records) {
    if (record.reason.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void CheckByteOffsetsAndEndian(const std::filesystem::path& root) {
  const auto bytes = ReadBytes(root / "data_contracts/fixtures/synthetic_itch.bin");
  Require(bytes.size() >= 75, "normal fixture too small for offset checks");
  Require(std::string(bytes.begin(), bytes.begin() + 4) == "VFLX", "magic offset mismatch");
  Require(BeU16(bytes, 4) == 69, "frame length endian mismatch");
  Require(BeU64(bytes, 6) == 1, "sequence endian mismatch");
  Require(bytes[14] == 10, "session length offset mismatch");
  Require(std::string(bytes.begin() + 15, bytes.begin() + 25) == "SYNTH-0001", "session offset mismatch");
  Require(bytes[25] == 'A', "message type offset mismatch");
  Require(BeI64(bytes, 26) == 1'700'000'000'000'001'000LL, "event timestamp offset/endian mismatch");
  Require(BeI64(bytes, 34) == 2'500, "receive lag offset/endian mismatch");
  Require(bytes[42] == 'B', "side offset mismatch");
  Require(BeI64(bytes, 43) == 10'000, "price offset/endian mismatch");
  Require(BeI64(bytes, 51) == 10, "quantity offset/endian mismatch");
  Require(BeU64(bytes, 59) == 1001, "order id offset/endian mismatch");
  Require(BeU64(bytes, 67) == 0, "reference order id offset/endian mismatch");
}

void CheckExtendedFixture(const std::filesystem::path& root) {
  const auto extended = aegis_decode::DecodeBytes(ReadBytes(root / "data_contracts/fixtures/synthetic_itch_extended.bin"));
  Require(extended.events.size() == 8, "extended fixture event count mismatch");
  Require(extended.gaps == 0, "extended fixture should not emit gaps");
  Require(extended.quarantine.empty(), "extended fixture should not quarantine input");
  Require(extended.events[1].event_type == "REPLACE", "replace fixture did not decode");
  Require(extended.events[2].event_type == "DELETE", "delete fixture did not decode");
  Require(extended.events[5].event_type == "BROKEN_TRADE", "broken trade fixture did not decode");
  Require(extended.events[6].event_type == "SESSION_RESET", "session reset fixture did not decode");
  Require(extended.events[7].sequence_number == 1 && extended.events[7].order_id == 5001,
          "session reset did not clear sequence state");
}

void CheckFuzzLiteCorpus(const std::filesystem::path& root) {
  const auto fuzz = aegis_decode::DecodeBytes(ReadBytes(root / "data_contracts/fixtures/synthetic_itch_fuzz_lite.bin"));
  Require(fuzz.events.size() == 1, "fuzz-lite should retain one good event");
  Require(fuzz.quarantine.size() == 4, "fuzz-lite quarantine count mismatch");
  Require(HasReason(fuzz.quarantine, "unsupported message type"), "unsupported message was not explicit");
  Require(HasReason(fuzz.quarantine, "unknown side"), "bad side was not explicit");
  Require(HasReason(fuzz.quarantine, "negative timestamp/price/quantity"), "high-bit signed field was not quarantined");
  Require(HasReason(fuzz.quarantine, "truncated_frame"), "truncated frame was not quarantined");
}

void CheckRegistryAndSequenceResetBoundaries(const std::filesystem::path& root) {
  Require(aegis_decode::TransportDecoderByName(aegis_decode::kSyntheticTransportDecoderName).name() ==
              aegis_decode::kSyntheticTransportDecoderName,
          "transport registry lookup failed");
  Require(aegis_decode::ApplicationDecoderByName(aegis_decode::kSyntheticApplicationDecoderName).name() ==
              aegis_decode::kSyntheticApplicationDecoderName,
          "application registry lookup failed");
  bool threw = false;
  try {
    (void)aegis_decode::TransportDecoderByName("missing_transport");
  } catch (const std::exception& error) {
    threw = std::string(error.what()).find("unknown transport decoder") != std::string::npos;
  }
  Require(threw, "missing transport decoder did not fail explicitly");

  auto bytes = ReadBytes(root / "data_contracts/fixtures/synthetic_itch.bin");
  const std::size_t second_frame_sequence = 75 + 2;
  for (std::size_t i = 0; i < 8; ++i) {
    bytes[second_frame_sequence + i] = i == 7 ? 1 : 0;
  }
  const auto stale = aegis_decode::DecodeBytes(bytes);
  Require(HasReason(stale.quarantine, "non_monotonic_sequence"), "non-reset sequence rewind was not quarantined");
}

void CheckJsonEscaping() {
  aegis_events::CanonicalEvent event{
      "ADD",
      1,
      1,
      1,
      std::string("S\"\x01\\\n"),
      "BID",
      100,
      1,
      1,
      0,
      {{std::string("bad\x02"), std::string("value\"\t")}},
  };
  const auto json = aegis_events::Jsonl({event});
  Require(json.find("\\\"") != std::string::npos, "quote was not JSON escaped");
  Require(json.find("\\\\") != std::string::npos, "backslash was not JSON escaped");
  Require(json.find("\\n") != std::string::npos, "newline was not JSON escaped");
  Require(json.find("\\u0001") != std::string::npos && json.find("\\u0002") != std::string::npos,
          "control byte was not JSON escaped");

  aegis_transport::QuarantineRecord record;
  record.offset = 7;
  record.reason = std::string("bad\"\x01\\\n");
  record.raw = {0xff};
  record.sequence_number = 9;
  record.session = std::string("Q\t");
  const auto quarantine = aegis_events::QuarantineJsonl({record});
  Require(quarantine.find("\\u0001") != std::string::npos && quarantine.find("\\n") != std::string::npos,
          "quarantine JSON escaping failed");
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const std::filesystem::path root = argc > 1 ? argv[1] : ".";

    const auto golden = aegis_decode::DecodeBytes(ReadBytes(root / "data_contracts/fixtures/synthetic_itch.bin"));
    Require(aegis_events::Jsonl(golden.events) == ReadText(root / "data_contracts/fixtures/normalized_events.golden.jsonl"),
            "golden decode mismatch");
    Require(golden.gaps == 0, "golden fixture emitted a gap");
    Require(golden.quarantine.empty(), "golden fixture quarantined input");

    const auto gap = aegis_decode::DecodeBytes(ReadBytes(root / "data_contracts/fixtures/synthetic_itch_gap.bin"));
    std::size_t gap_events = 0;
    for (const auto& event : gap.events) {
      gap_events += event.event_type == "GAP_DETECTED" ? 1U : 0U;
    }
    Require(gap_events == 1 && gap.gaps == 1, "gap fixture did not emit exactly one gap");

    const auto malformed = aegis_decode::DecodeBytes(ReadBytes(root / "data_contracts/fixtures/synthetic_itch_malformed.bin"));
    Require(malformed.events.size() == 1, "malformed fixture should keep the first good event");
    Require(malformed.quarantine.size() == 1, "malformed fixture should quarantine exactly one frame");

    CheckByteOffsetsAndEndian(root);
    CheckExtendedFixture(root);
    CheckFuzzLiteCorpus(root);
    CheckRegistryAndSequenceResetBoundaries(root);
    CheckJsonEscaping();

    std::cout << "decoder_tests pass\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
