#pragma once

#include "aegis_transport/synthetic.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace aegis_events {

struct CanonicalEvent {
  std::string event_type;
  std::int64_t event_ts_ns{};
  std::int64_t receive_ts_ns{};
  std::uint64_t sequence_number{};
  std::string session;
  std::string side;
  std::int64_t price_ticks{};
  std::int64_t quantity{};
  std::uint64_t order_id{};
  std::uint64_t reference_order_id{};
  std::map<std::string, std::string> tags;
};

std::string Jsonl(const std::vector<CanonicalEvent>& events);
std::string QuarantineJsonl(const std::vector<aegis_transport::QuarantineRecord>& records);

}  // namespace aegis_events
