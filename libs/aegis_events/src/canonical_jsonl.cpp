#include "aegis_events/canonical_jsonl.hpp"

#include <sstream>

namespace aegis_events {
namespace {

constexpr const char* kSchemaVersion = "vegaflux.canonical_market.v0.1";
constexpr const char* kVenue = "SYNTH";
constexpr const char* kChannel = "SYNTH-A";
constexpr const char* kSymbol = "VGFX";

std::string Escape(const std::string& value) {
  std::ostringstream out;
  constexpr char kHex[] = "0123456789abcdef";
  for (const char ch : value) {
    const auto byte = static_cast<unsigned char>(ch);
    switch (ch) {
      case '\\':
        out << "\\\\";
        break;
      case '"':
        out << "\\\"";
        break;
      case '\n':
        out << "\\n";
        break;
      case '\r':
        out << "\\r";
        break;
      case '\t':
        out << "\\t";
        break;
      default:
        if (byte < 0x20) {
          out << "\\u00" << kHex[(byte >> 4U) & 0x0fU] << kHex[byte & 0x0fU];
        } else {
          out << ch;
        }
    }
  }
  return out.str();
}

void StringField(std::ostringstream& out, const char* key, const std::string& value) {
  out << '"' << Escape(key) << "\":\"" << Escape(value) << '"';
}

void Tags(std::ostringstream& out, const std::map<std::string, std::string>& tags) {
  out << "\"tags\":{";
  bool first = true;
  for (const auto& [key, value] : tags) {
    if (!first) {
      out << ',';
    }
    first = false;
    StringField(out, key.c_str(), value);
  }
  out << '}';
}

std::string EventJson(const CanonicalEvent& event) {
  std::ostringstream out;
  out << '{';
  StringField(out, "channel", kChannel);
  out << ",\"event_ts_ns\":" << event.event_ts_ns;
  out << ',';
  StringField(out, "event_type", event.event_type);
  out << ",\"order_id\":" << event.order_id;
  out << ",\"price_ticks\":" << event.price_ticks;
  out << ",\"quantity\":" << event.quantity;
  out << ",\"raw_payload\":\"\"";
  out << ",\"receive_ts_ns\":" << event.receive_ts_ns;
  out << ",\"reference_order_id\":" << event.reference_order_id;
  out << ',';
  StringField(out, "schema_version", kSchemaVersion);
  out << ",\"sequence_number\":" << event.sequence_number;
  out << ',';
  StringField(out, "session", event.session);
  out << ',';
  StringField(out, "side", event.side);
  out << ',';
  StringField(out, "symbol", kSymbol);
  out << ',';
  Tags(out, event.tags);
  out << ',';
  StringField(out, "venue", kVenue);
  out << '}';
  return out.str();
}

std::string QuarantineJson(const aegis_transport::QuarantineRecord& record) {
  std::ostringstream out;
  out << "{\"offset\":" << record.offset;
  out << ",\"raw_hex\":\"" << aegis_transport::Hex(record.raw) << '"';
  out << ",\"reason\":\"" << Escape(record.reason) << '"';
  if (record.sequence_number.has_value()) {
    out << ",\"sequence_number\":" << *record.sequence_number;
  }
  if (record.session.has_value()) {
    out << ',';
    StringField(out, "session", *record.session);
  }
  out << '}';
  return out.str();
}

}  // namespace

std::string Jsonl(const std::vector<CanonicalEvent>& events) {
  std::ostringstream out;
  for (const auto& event : events) {
    out << EventJson(event) << '\n';
  }
  return out.str();
}

std::string QuarantineJsonl(const std::vector<aegis_transport::QuarantineRecord>& records) {
  std::ostringstream out;
  for (const auto& record : records) {
    out << QuarantineJson(record) << '\n';
  }
  return out.str();
}

}  // namespace aegis_events
