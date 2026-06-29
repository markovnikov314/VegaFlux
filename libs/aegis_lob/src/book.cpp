#include "aegis_lob/book.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace aegis_lob {
namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

std::size_t ValuePos(const std::string& line, const std::string& key) {
  const auto pos = line.find("\"" + key + "\"");
  if (pos == std::string::npos) {
    throw std::runtime_error("missing JSON key: " + key);
  }
  auto value_pos = pos + key.size() + 2;
  while (value_pos < line.size() && std::isspace(static_cast<unsigned char>(line[value_pos]))) {
    ++value_pos;
  }
  if (value_pos >= line.size() || line[value_pos] != ':') {
    throw std::runtime_error("missing JSON colon after key: " + key);
  }
  ++value_pos;
  while (value_pos < line.size() && std::isspace(static_cast<unsigned char>(line[value_pos]))) {
    ++value_pos;
  }
  return value_pos;
}

std::string StringField(const std::string& line, const std::string& key) {
  auto pos = ValuePos(line, key);
  if (pos >= line.size() || line[pos] != '"') {
    throw std::runtime_error("expected string JSON field: " + key);
  }
  ++pos;
  std::string out;
  for (; pos < line.size(); ++pos) {
    const char ch = line[pos];
    if (ch == '"') {
      return out;
    }
    if (ch == '\\') {
      if (++pos >= line.size()) {
        throw std::runtime_error("bad JSON escape in: " + key);
      }
      switch (line[pos]) {
        case '"':
        case '\\':
        case '/':
          out.push_back(line[pos]);
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        default:
          out.push_back(line[pos]);
      }
      continue;
    }
    out.push_back(ch);
  }
  throw std::runtime_error("unterminated string JSON field: " + key);
}

std::int64_t IntField(const std::string& line, const std::string& key) {
  auto pos = ValuePos(line, key);
  const auto end = line.find_first_of(",}", pos);
  return std::stoll(line.substr(pos, end - pos));
}

std::uint64_t UintField(const std::string& line, const std::string& key) {
  auto pos = ValuePos(line, key);
  const auto end = line.find_first_of(",}", pos);
  return std::stoull(line.substr(pos, end - pos));
}

Side ParseSide(const std::string& value) {
  if (value == "BID") {
    return Side::kBid;
  }
  if (value == "ASK") {
    return Side::kAsk;
  }
  return Side::kUnspecified;
}

std::string Escape(const std::string& value) {
  std::ostringstream out;
  for (const char ch : value) {
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
        out << ch;
    }
  }
  return out.str();
}

std::string TrimLeadingSpace(std::string value) {
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
                return !std::isspace(ch);
              }));
  return value;
}

void MixByte(std::uint64_t& hash, std::uint8_t byte) {
  hash ^= byte;
  hash *= kFnvPrime;
}

void MixUint(std::uint64_t& hash, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    MixByte(hash, static_cast<std::uint8_t>((value >> (i * 8)) & 0xffu));
  }
}

void MixInt(std::uint64_t& hash, std::int64_t value) {
  MixUint(hash, static_cast<std::uint64_t>(value));
}

void MixString(std::uint64_t& hash, const std::string& value) {
  for (const unsigned char ch : value) {
    MixByte(hash, ch);
  }
  MixByte(hash, 0);
}

template <typename Ladder>
void AddLevel(Ladder& ladder, Side side, std::int64_t price_ticks, std::int64_t quantity) {
  auto& level = ladder[price_ticks];
  level.price_ticks = price_ticks;
  level.quantity += quantity;
  ++level.order_count;
  (void)side;
}

template <typename Ladder>
void ReduceLevel(Ladder& ladder, std::int64_t price_ticks, std::int64_t quantity, bool remove_order) {
  const auto found = ladder.find(price_ticks);
  if (found == ladder.end()) {
    return;
  }
  found->second.quantity -= quantity;
  if (remove_order && found->second.order_count > 0) {
    --found->second.order_count;
  }
  if (found->second.quantity <= 0 || found->second.order_count == 0) {
    ladder.erase(found);
  }
}

template <typename Ladder>
std::vector<PriceLevel> TopLevels(const Ladder& ladder, std::uint32_t depth) {
  std::vector<PriceLevel> levels;
  for (const auto& [_, level] : ladder) {
    if (levels.size() >= depth) {
      break;
    }
    levels.push_back(level);
  }
  return levels;
}

void WriteLevels(std::ostringstream& out, const std::vector<PriceLevel>& levels) {
  out << '[';
  for (std::size_t i = 0; i < levels.size(); ++i) {
    if (i != 0) {
      out << ',';
    }
    out << "{\"price_ticks\":" << levels[i].price_ticks << ",\"quantity\":" << levels[i].quantity
        << ",\"order_count\":" << levels[i].order_count << '}';
  }
  out << ']';
}

void WriteAnomaly(std::ostringstream& out, const Anomaly& anomaly) {
  out << "{\"schema_version\":\"vegaflux.canonical_market.v0.1\",\"sequence_number\":" << anomaly.sequence_number
      << ",\"event_ts_ns\":" << anomaly.event_ts_ns << ",\"event_type\":\"" << Escape(anomaly.event_type)
      << "\",\"order_id\":" << anomaly.order_id << ",\"reference_order_id\":" << anomaly.reference_order_id
      << ",\"reason\":\"" << Escape(anomaly.reason) << "\",\"status\":\"recoverable\"}\n";
}

void WriteAudit(std::ostringstream& out, const AuditBoundary& audit) {
  out << "{\"schema_version\":\"vegaflux.canonical_market.v0.1\",\"sequence_number\":" << audit.sequence_number
      << ",\"event_ts_ns\":" << audit.event_ts_ns << ",\"event_type\":\"" << Escape(audit.event_type)
      << "\",\"order_id\":" << audit.order_id << ",\"missing_count\":" << audit.missing_count
      << ",\"reason\":\"" << Escape(audit.reason) << "\"}\n";
}

std::string CheckpointString(const std::string& value) {
  return value.empty() ? "-" : value;
}

std::string RestoreString(const std::string& value) {
  return value == "-" ? "" : value;
}

}  // namespace

std::string SideName(Side side) {
  switch (side) {
    case Side::kBid:
      return "BID";
    case Side::kAsk:
      return "ASK";
    case Side::kUnspecified:
      return "SIDE_UNSPECIFIED";
  }
  return "SIDE_UNSPECIFIED";
}

CanonicalEvent ParseCanonicalJson(const std::string& line) {
  // ponytail: canonical JSONL parser only; replace with a real JSON dependency if external feeds arrive.
  CanonicalEvent event;
  event.schema_version = StringField(line, "schema_version");
  event.venue = StringField(line, "venue");
  event.channel = StringField(line, "channel");
  event.session = StringField(line, "session");
  event.sequence_number = UintField(line, "sequence_number");
  event.event_ts_ns = IntField(line, "event_ts_ns");
  event.receive_ts_ns = IntField(line, "receive_ts_ns");
  event.symbol = StringField(line, "symbol");
  event.event_type = StringField(line, "event_type");
  event.side = ParseSide(StringField(line, "side"));
  event.price_ticks = IntField(line, "price_ticks");
  event.quantity = IntField(line, "quantity");
  event.order_id = UintField(line, "order_id");
  event.reference_order_id = UintField(line, "reference_order_id");
  return event;
}

std::vector<CanonicalEvent> LoadCanonicalJsonl(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("failed to read canonical JSONL: " + path.string());
  }
  std::vector<CanonicalEvent> events;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty()) {
      events.push_back(ParseCanonicalJson(line));
    }
  }
  return events;
}

void Book::Clear() {
  orders_.clear();
  bids_.clear();
  asks_.clear();
  anomalies_.clear();
  audit_boundaries_.clear();
  schema_version_.clear();
  venue_.clear();
  symbol_.clear();
  session_.clear();
  last_sequence_number_ = 0;
  last_event_ts_ns_ = 0;
}

void Book::RememberMetadata(const CanonicalEvent& event) {
  schema_version_ = event.schema_version;
  venue_ = event.venue;
  symbol_ = event.symbol;
  session_ = event.session;
  last_sequence_number_ = event.sequence_number;
  last_event_ts_ns_ = event.event_ts_ns;
}

void Book::Apply(const CanonicalEvent& event) {
  RememberMetadata(event);
  if (event.event_type == "GAP_DETECTED") {
    RecordAudit(event, "gap_detected", event.quantity);
    return;
  }
  if (event.event_type == "SESSION_RESET") {
    RecordAudit(event, "session_reset_cleared_book");
    orders_.clear();
    bids_.clear();
    asks_.clear();
    return;
  }
  if (event.event_type == "ADD") {
    AddOrder(event);
    return;
  }
  if (event.event_type == "REPLACE" || event.event_type == "MODIFY") {
    ReplaceOrder(event);
    return;
  }
  if (event.event_type == "DELETE") {
    DeleteOrder(event);
    return;
  }
  if (event.event_type == "EXECUTE" || event.event_type == "CANCEL") {
    ReduceOrder(event);
    return;
  }
  if (event.event_type == "TRADE") {
    RecordAudit(event, "trade_event_not_book_mutated");
    return;
  }
  if (event.event_type == "BROKEN_TRADE") {
    RecordAudit(event, "broken_trade_not_reversed");
    return;
  }
  if (event.event_type.find("AUCTION") != std::string::npos || event.event_type.find("CONTROL") != std::string::npos) {
    RecordAudit(event, "control_event_not_book_mutated");
    return;
  }
  RecordAudit(event, "unsupported_event_type");
}

void Book::AddOrder(const CanonicalEvent& event) {
  if (event.order_id == 0 || event.quantity <= 0 || event.price_ticks <= 0 || event.side == Side::kUnspecified) {
    RecordAnomaly(event, event.order_id, "invalid_add");
    return;
  }
  if (orders_.contains(event.order_id)) {
    RecordAnomaly(event, event.order_id, "duplicate_add");
    return;
  }
  if (WouldCross(event.side, event.price_ticks)) {
    RecordAnomaly(event, event.order_id, event.side == Side::kBid ? "crossed_book_bid" : "crossed_book_ask");
    return;
  }
  OrderRecord order{event.order_id, event.side, event.price_ticks, event.quantity};
  orders_.emplace(order.order_id, order);
  InsertRestoredOrder(order);
}

void Book::ReplaceOrder(const CanonicalEvent& event) {
  const auto old_order_id = event.reference_order_id != 0 ? event.reference_order_id : event.order_id;
  const auto found = orders_.find(old_order_id);
  if (found == orders_.end()) {
    RecordAnomaly(event, old_order_id, "unknown_reference_order_id");
    return;
  }
  const auto& old_order = found->second;
  const auto new_order_id = event.event_type == "MODIFY" ? old_order_id : event.order_id;
  const auto new_side = event.side == Side::kUnspecified ? old_order.side : event.side;
  if (new_order_id == 0 || event.quantity <= 0 || event.price_ticks <= 0 || new_side == Side::kUnspecified) {
    RecordAnomaly(event, new_order_id, "invalid_replace");
    return;
  }
  if (new_side != old_order.side) {
    RecordAnomaly(event, old_order_id, "side_mismatch");
    return;
  }
  if (new_order_id != old_order_id && orders_.contains(new_order_id)) {
    RecordAnomaly(event, new_order_id, "duplicate_replace_order_id");
    return;
  }
  if (WouldCross(new_side, event.price_ticks)) {
    RecordAnomaly(event, new_order_id, new_side == Side::kBid ? "crossed_book_bid" : "crossed_book_ask");
    return;
  }
  RemoveOrder(old_order);
  orders_.erase(found);
  OrderRecord replacement{new_order_id, new_side, event.price_ticks, event.quantity};
  orders_.emplace(replacement.order_id, replacement);
  InsertRestoredOrder(replacement);
}

void Book::ReduceOrder(const CanonicalEvent& event) {
  const auto order_id = event.reference_order_id != 0 ? event.reference_order_id : event.order_id;
  const auto found = orders_.find(order_id);
  if (found == orders_.end()) {
    RecordAnomaly(event, order_id, "unknown_reference_order_id");
    return;
  }
  if (event.quantity <= 0) {
    RecordAnomaly(event, order_id, "non_positive_reduce_quantity");
    return;
  }
  auto& order = found->second;
  if (event.side != Side::kUnspecified && event.side != order.side) {
    RecordAnomaly(event, order_id, "side_mismatch");
  }
  if (event.price_ticks != 0 && event.price_ticks != order.price_ticks) {
    RecordAnomaly(event, order_id, "price_mismatch");
  }
  auto quantity = event.quantity;
  if (quantity > order.quantity) {
    RecordAnomaly(event, order_id, "reduce_quantity_exceeds_live_order");
    quantity = order.quantity;
  }
  order.quantity -= quantity;
  const bool remove_order = order.quantity == 0;
  if (order.side == Side::kBid) {
    ReduceLevel(bids_, order.price_ticks, quantity, remove_order);
  } else if (order.side == Side::kAsk) {
    ReduceLevel(asks_, order.price_ticks, quantity, remove_order);
  }
  if (remove_order) {
    orders_.erase(found);
  }
}

void Book::DeleteOrder(const CanonicalEvent& event) {
  const auto order_id = event.reference_order_id != 0 ? event.reference_order_id : event.order_id;
  const auto found = orders_.find(order_id);
  if (found == orders_.end()) {
    RecordAnomaly(event, order_id, "unknown_reference_order_id");
    return;
  }
  const auto order = found->second;
  if (event.side != Side::kUnspecified && event.side != order.side) {
    RecordAnomaly(event, order_id, "side_mismatch");
  }
  if (event.price_ticks != 0 && event.price_ticks != order.price_ticks) {
    RecordAnomaly(event, order_id, "price_mismatch");
  }
  RemoveOrder(order);
  orders_.erase(found);
}

void Book::RecordAudit(const CanonicalEvent& event, const std::string& reason, std::int64_t missing_count) {
  audit_boundaries_.push_back(
      {event.sequence_number, event.event_ts_ns, event.event_type, event.order_id, missing_count, reason});
}

void Book::RecordAnomaly(const CanonicalEvent& event, std::uint64_t order_id, const std::string& reason) {
  anomalies_.push_back(
      {event.sequence_number, event.event_ts_ns, event.event_type, order_id, event.reference_order_id, reason});
}

void Book::InsertRestoredOrder(const OrderRecord& order) {
  if (order.side == Side::kBid) {
    AddLevel(bids_, order.side, order.price_ticks, order.quantity);
  } else if (order.side == Side::kAsk) {
    AddLevel(asks_, order.side, order.price_ticks, order.quantity);
  }
}

void Book::RemoveOrder(const OrderRecord& order) {
  if (order.side == Side::kBid) {
    ReduceLevel(bids_, order.price_ticks, order.quantity, true);
  } else if (order.side == Side::kAsk) {
    ReduceLevel(asks_, order.price_ticks, order.quantity, true);
  }
}

bool Book::WouldCross(Side side, std::int64_t price_ticks) const {
  if (side == Side::kBid) {
    return !asks_.empty() && price_ticks >= asks_.begin()->first;
  }
  if (side == Side::kAsk) {
    return !bids_.empty() && price_ticks <= bids_.begin()->first;
  }
  return false;
}

BookSnapshot Book::Snapshot(std::uint32_t depth, const std::string& checkpoint_id) const {
  BookSnapshot snapshot;
  snapshot.schema_version = schema_version_;
  snapshot.venue = venue_;
  snapshot.symbol = symbol_;
  snapshot.session = session_;
  snapshot.sequence_number = last_sequence_number_;
  snapshot.event_ts_ns = last_event_ts_ns_;
  snapshot.bids = TopLevels(bids_, depth);
  snapshot.asks = TopLevels(asks_, depth);
  snapshot.state_checksum = StateChecksum();
  snapshot.checkpoint_id = checkpoint_id;
  return snapshot;
}

std::uint64_t Book::StateChecksum() const {
  std::uint64_t hash = kFnvOffset;
  for (const auto& [order_id, order] : orders_) {
    MixUint(hash, order_id);
    MixString(hash, SideName(order.side));
    MixInt(hash, order.price_ticks);
    MixInt(hash, order.quantity);
  }
  MixByte(hash, 0xff);
  for (const auto& [price, level] : bids_) {
    MixInt(hash, price);
    MixInt(hash, level.quantity);
    MixUint(hash, level.order_count);
  }
  MixByte(hash, 0xfe);
  for (const auto& [price, level] : asks_) {
    MixInt(hash, price);
    MixInt(hash, level.quantity);
    MixUint(hash, level.order_count);
  }
  return hash;
}

void Book::SaveCheckpoint(const std::filesystem::path& path) const {
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw std::runtime_error("failed to write checkpoint: " + path.string());
  }
  out << "VFLX_CHECKPOINT 2\n";
  out << "META " << CheckpointString(schema_version_) << ' ' << CheckpointString(venue_) << ' '
      << CheckpointString(symbol_) << ' ' << CheckpointString(session_) << ' ' << last_sequence_number_ << ' '
      << last_event_ts_ns_ << '\n';
  for (const auto& [_, order] : orders_) {
    out << "ORDER " << order.order_id << ' ' << SideName(order.side) << ' ' << order.price_ticks << ' ' << order.quantity
        << '\n';
  }
  for (const auto& anomaly : anomalies_) {
    out << "ANOMALY " << anomaly.sequence_number << ' ' << anomaly.event_ts_ns << ' ' << anomaly.event_type << ' '
        << anomaly.order_id << ' ' << anomaly.reference_order_id << ' ' << anomaly.reason << '\n';
  }
  for (const auto& audit : audit_boundaries_) {
    out << "AUDIT " << audit.sequence_number << ' ' << audit.event_ts_ns << ' ' << audit.event_type << ' '
        << audit.order_id << ' ' << audit.missing_count << ' ' << audit.reason << '\n';
  }
}

void Book::LoadCheckpoint(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("failed to read checkpoint: " + path.string());
  }
  Clear();
  std::string header;
  std::getline(in, header);
  std::istringstream header_in(header);
  std::string magic;
  int version = 0;
  header_in >> magic >> version;
  if (magic != "VFLX_CHECKPOINT" || (version != 1 && version != 2)) {
    throw std::runtime_error("unsupported checkpoint: " + path.string());
  }
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    std::istringstream line_in(line);
    std::string token;
    line_in >> token;
    if (token == "META") {
      std::string schema;
      std::string venue;
      std::string symbol;
      std::string session;
      line_in >> schema >> venue >> symbol >> session >> last_sequence_number_ >> last_event_ts_ns_;
      schema_version_ = RestoreString(schema);
      venue_ = RestoreString(venue);
      symbol_ = RestoreString(symbol);
      session_ = RestoreString(session);
    } else if (token == "ORDER") {
      OrderRecord order;
      std::string side;
      line_in >> order.order_id >> side >> order.price_ticks >> order.quantity;
      order.side = ParseSide(side);
      orders_.emplace(order.order_id, order);
      InsertRestoredOrder(order);
    } else if (token == "ANOMALY" && version >= 2) {
      Anomaly anomaly;
      line_in >> anomaly.sequence_number >> anomaly.event_ts_ns >> anomaly.event_type >> anomaly.order_id >>
          anomaly.reference_order_id;
      std::getline(line_in, anomaly.reason);
      anomaly.reason = TrimLeadingSpace(anomaly.reason);
      anomalies_.push_back(anomaly);
    } else if (token == "AUDIT" && version >= 2) {
      AuditBoundary audit;
      line_in >> audit.sequence_number >> audit.event_ts_ns >> audit.event_type >> audit.order_id >> audit.missing_count;
      std::getline(line_in, audit.reason);
      audit.reason = TrimLeadingSpace(audit.reason);
      audit_boundaries_.push_back(audit);
    }
  }
}

std::string SnapshotJson(const BookSnapshot& snapshot) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"schema_version\": \"" << Escape(snapshot.schema_version) << "\",\n";
  out << "  \"venue\": \"" << Escape(snapshot.venue) << "\",\n";
  out << "  \"symbol\": \"" << Escape(snapshot.symbol) << "\",\n";
  out << "  \"session\": \"" << Escape(snapshot.session) << "\",\n";
  out << "  \"sequence_number\": " << snapshot.sequence_number << ",\n";
  out << "  \"event_ts_ns\": " << snapshot.event_ts_ns << ",\n";
  out << "  \"bids\": ";
  WriteLevels(out, snapshot.bids);
  out << ",\n  \"asks\": ";
  WriteLevels(out, snapshot.asks);
  out << ",\n";
  out << "  \"state_checksum\": " << snapshot.state_checksum << ",\n";
  out << "  \"checkpoint_id\": \"" << Escape(snapshot.checkpoint_id) << "\"\n";
  out << "}\n";
  return out.str();
}

std::string AnomalyJsonl(const std::vector<Anomaly>& anomalies) {
  std::ostringstream out;
  for (const auto& anomaly : anomalies) {
    WriteAnomaly(out, anomaly);
  }
  return out.str();
}

std::string AuditBoundaryJsonl(const std::vector<AuditBoundary>& audits) {
  std::ostringstream out;
  for (const auto& audit : audits) {
    WriteAudit(out, audit);
  }
  return out.str();
}

}  // namespace aegis_lob
