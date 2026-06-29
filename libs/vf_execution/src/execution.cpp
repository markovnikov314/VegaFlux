#include "vf_execution/execution.hpp"

#include "aegis_replay/replay.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <stdexcept>

namespace vf_execution {
namespace {

constexpr const char* kVisibleFifoQueueModelId = "visible_fifo_v1";
constexpr const char* kFixedLatencyModelId = "fixed_latency_ns_v1";

struct LiveOrder {
  aegis_lob::Side side{aegis_lob::Side::kUnspecified};
  std::int64_t price_ticks{};
  std::int64_t quantity{};
};

std::string Escape(const std::string& value) {
  std::string out;
  for (const char ch : value) {
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
        out.push_back(ch);
    }
  }
  return out;
}

std::string JsonString(const std::string& value) {
  return "\"" + Escape(value) + "\"";
}

std::string JsonInt(std::optional<std::int64_t> value) {
  return value.has_value() ? std::to_string(*value) : "null";
}

std::string JsonUint(std::optional<std::uint64_t> value) {
  return value.has_value() ? std::to_string(*value) : "null";
}

std::string JsonDouble(double value) {
  std::ostringstream out;
  out << value;
  return out.str();
}

void AddJsonField(std::ostringstream& out, const std::string& name, const std::string& value, bool& first) {
  if (!first) {
    out << ',';
  }
  first = false;
  out << '"' << name << "\":" << value;
}

std::size_t ValuePos(const std::string& text, const std::string& key) {
  const auto needle = "\"" + key + "\":";
  const auto pos = text.find(needle);
  if (pos == std::string::npos) {
    throw std::runtime_error("missing JSON key: " + key);
  }
  return pos + needle.size();
}

void SkipSpaces(const std::string& text, std::size_t& pos) {
  while (pos < text.size() && text[pos] == ' ') {
    ++pos;
  }
}

std::string StringField(const std::string& text, const std::string& key) {
  auto pos = ValuePos(text, key);
  SkipSpaces(text, pos);
  if (pos >= text.size() || text[pos] != '"') {
    throw std::runtime_error("expected string JSON field: " + key);
  }
  ++pos;
  std::string out;
  for (; pos < text.size(); ++pos) {
    if (text[pos] == '"') {
      return out;
    }
    if (text[pos] == '\\' && pos + 1 < text.size()) {
      ++pos;
    }
    out.push_back(text[pos]);
  }
  throw std::runtime_error("unterminated string JSON field: " + key);
}

std::int64_t IntField(const std::string& text, const std::string& key) {
  auto pos = ValuePos(text, key);
  SkipSpaces(text, pos);
  const auto end = text.find_first_of(",}\r\n", pos);
  return std::stoll(text.substr(pos, end - pos));
}

std::uint64_t UintField(const std::string& text, const std::string& key) {
  auto pos = ValuePos(text, key);
  SkipSpaces(text, pos);
  const auto end = text.find_first_of(",}\r\n", pos);
  return std::stoull(text.substr(pos, end - pos));
}

std::optional<std::int64_t> OptionalIntField(const std::string& text, const std::string& key) {
  auto pos = ValuePos(text, key);
  SkipSpaces(text, pos);
  if (text.compare(pos, 4, "null") == 0) {
    return std::nullopt;
  }
  const auto end = text.find_first_of(",}\r\n", pos);
  return std::stoll(text.substr(pos, end - pos));
}

const FeatureRow& FindFeature(const std::vector<FeatureRow>& features, std::uint64_t sequence_number) {
  const auto found = std::find_if(features.begin(), features.end(), [sequence_number](const FeatureRow& row) {
    return row.sequence_number == sequence_number || row.source_sequence_number == sequence_number;
  });
  if (found == features.end()) {
    throw std::runtime_error("decision sequence not found in features features");
  }
  return *found;
}

aegis_lob::Side QueueSide(Side side) {
  return side == Side::kBuy ? aegis_lob::Side::kBid : aegis_lob::Side::kAsk;
}

bool SnapshotContainsPrice(const aegis_lob::BookSnapshot& snapshot, Side side, std::int64_t price_ticks) {
  const auto& levels = side == Side::kBuy ? snapshot.bids : snapshot.asks;
  return std::find_if(levels.begin(), levels.end(), [price_ticks](const aegis_lob::PriceLevel& level) {
    return level.price_ticks == price_ticks;
  }) != levels.end();
}

std::int64_t VisibleAtPrice(const std::map<std::uint64_t, LiveOrder>& orders,
                            Side side,
                            std::int64_t price_ticks) {
  std::int64_t quantity = 0;
  for (const auto& [_, order] : orders) {
    if (order.side == QueueSide(side) && order.price_ticks == price_ticks) {
      quantity += order.quantity;
    }
  }
  return quantity;
}

bool IsReduce(const std::string& event_type) {
  return event_type == "EXECUTE" || event_type == "CANCEL" || event_type == "DELETE";
}

std::uint64_t ReferencedOrderId(const aegis_lob::CanonicalEvent& event) {
  return event.reference_order_id != 0 ? event.reference_order_id : event.order_id;
}

std::optional<LiveOrder> ReferencedLiveOrder(const std::map<std::uint64_t, LiveOrder>& orders,
                                             const aegis_lob::CanonicalEvent& event) {
  const auto found = orders.find(ReferencedOrderId(event));
  if (found == orders.end()) {
    return std::nullopt;
  }
  return found->second;
}

void ApplyLiveOrderEvent(std::map<std::uint64_t, LiveOrder>& orders, const aegis_lob::CanonicalEvent& event) {
  if (event.event_type == "SESSION_RESET") {
    orders.clear();
    return;
  }
  if (event.event_type == "ADD") {
    if (event.order_id != 0 && event.quantity > 0 && event.price_ticks > 0 &&
        event.side != aegis_lob::Side::kUnspecified) {
      orders[event.order_id] = {event.side, event.price_ticks, event.quantity};
    }
    return;
  }
  if (event.event_type == "REPLACE" || event.event_type == "MODIFY") {
    const auto old_order_id = ReferencedOrderId(event);
    const auto found = orders.find(old_order_id);
    if (found == orders.end()) {
      return;
    }
    const auto old_order = found->second;
    orders.erase(found);
    const auto new_order_id = event.event_type == "MODIFY" ? old_order_id : event.order_id;
    const auto new_side = event.side == aegis_lob::Side::kUnspecified ? old_order.side : event.side;
    if (new_order_id != 0 && event.quantity > 0 && event.price_ticks > 0) {
      orders[new_order_id] = {new_side, event.price_ticks, event.quantity};
    }
    return;
  }
  if (IsReduce(event.event_type)) {
    const auto found = orders.find(ReferencedOrderId(event));
    if (found == orders.end() || event.quantity <= 0) {
      return;
    }
    found->second.quantity -= std::min(found->second.quantity, event.quantity);
    if (found->second.quantity == 0) {
      orders.erase(found);
    }
  }
}

std::int64_t ScaledQuantity(std::int64_t quantity, double multiplier) {
  return std::max<std::int64_t>(0, static_cast<std::int64_t>(std::llround(static_cast<double>(quantity) * multiplier)));
}

void ValidateConfig(const SimulationConfig& config) {
  if (config.queue_model_id != kVisibleFifoQueueModelId) {
    throw std::runtime_error("unsupported queue_model_id: " + config.queue_model_id);
  }
  if (config.latency_model_id != kFixedLatencyModelId) {
    throw std::runtime_error("unsupported latency_model_id: " + config.latency_model_id);
  }
  if (config.horizon_ns <= 0) {
    throw std::runtime_error("horizon_ns must be positive");
  }
  if (config.fixed_latency_ns < 0) {
    throw std::runtime_error("fixed_latency_ns must be non-negative");
  }
  if (config.depth == 0) {
    throw std::runtime_error("depth must be positive");
  }
  if (config.markout_horizon_ns < 0) {
    throw std::runtime_error("markout_horizon_ns must be non-negative");
  }
  if (!std::isfinite(config.hidden_ahead_multiplier) || config.hidden_ahead_multiplier < 0.0) {
    throw std::runtime_error("hidden_ahead_multiplier must be finite and non-negative");
  }
  if (!std::isfinite(config.latency_multiplier) || config.latency_multiplier < 0.0) {
    throw std::runtime_error("latency_multiplier must be finite and non-negative");
  }
  if (!std::isfinite(config.cancel_rate_multiplier) || config.cancel_rate_multiplier < 0.0) {
    throw std::runtime_error("cancel_rate_multiplier must be finite and non-negative");
  }
}

std::optional<std::int64_t> MarkoutTicksX2(const FillResult& result, const FeatureRow& decision,
                                           const std::vector<FeatureRow>& features) {
  if (!result.first_fill_ts_ns.has_value() || !decision.mid_ticks_x2.has_value()) {
    return std::nullopt;
  }
  const auto target_ts = *result.first_fill_ts_ns + result.markout_horizon_ns;
  const auto found = std::find_if(features.begin(), features.end(), [&](const FeatureRow& row) {
    return row.available_at_ns >= target_ts && row.available_at_ns <= result.horizon_end_ts_ns &&
           row.mid_ticks_x2.has_value();
  });
  if (found == features.end()) {
    return std::nullopt;
  }
  const auto move = *found->mid_ticks_x2 - *decision.mid_ticks_x2;
  return result.side == Side::kBuy ? move : -move;
}

}  // namespace

std::string SideName(Side side) {
  return side == Side::kBuy ? "BUY" : "SELL";
}

Side ParseSide(const std::string& value) {
  if (value == "BUY") {
    return Side::kBuy;
  }
  if (value == "SELL") {
    return Side::kSell;
  }
  throw std::runtime_error("side must be BUY or SELL");
}

std::vector<FeatureRow> LoadFeatureRows(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("failed to read features features: " + path.string());
  }
  std::vector<FeatureRow> rows;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    FeatureRow row;
    row.schema_version = StringField(line, "schema_version");
    row.dataset_id = StringField(line, "dataset_id");
    row.row_index = UintField(line, "row_index");
    row.sequence_number = UintField(line, "sequence_number");
    row.source_sequence_number = UintField(line, "source_sequence_number");
    row.available_at_ns = IntField(line, "available_at_ns");
    row.snapshot_state_checksum = UintField(line, "snapshot_state_checksum");
    row.replay_checksum = UintField(line, "replay_checksum");
    row.mid_ticks_x2 = OptionalIntField(line, "mid_ticks_x2");
    row.bid_qty_top1 = IntField(line, "bid_qty_top1");
    row.ask_qty_top1 = IntField(line, "ask_qty_top1");
    rows.push_back(row);
  }
  return rows;
}

FillResult SimulateVisibleFifo(const SimulationConfig& config,
                               const SimOrder& order,
                               const std::vector<aegis_lob::CanonicalEvent>& events,
                               const std::vector<FeatureRow>& features) {
  ValidateConfig(config);
  if (order.quantity <= 0) {
    throw std::runtime_error("order quantity must be positive");
  }

  const auto& decision = FindFeature(features, order.decision_sequence_number);
  aegis_replay::ReplayCursor cursor(events);
  std::map<std::uint64_t, LiveOrder> live_orders;
  bool applied_decision = false;
  std::size_t decision_index = 0;
  for (; cursor.has_next(); ++decision_index) {
    const auto& event = cursor.next_event();
    ApplyLiveOrderEvent(live_orders, event);
    if (event.sequence_number == order.decision_sequence_number) {
      applied_decision = true;
      break;
    }
  }
  if (!applied_decision) {
    throw std::runtime_error("decision sequence not found in replay replay");
  }
  if (cursor.state_checksum() != decision.snapshot_state_checksum) {
    throw std::runtime_error("decision snapshot checksum mismatch");
  }
  aegis_replay::ReplayCursor final_cursor(events);
  final_cursor.replay_all();
  if (final_cursor.state_checksum() != decision.replay_checksum) {
    throw std::runtime_error("source replay checksum mismatch");
  }

  FillResult result;
  result.simulation_id = config.simulation_id;
  result.sim_order_id = order.order_id;
  result.side = order.side;
  result.price_ticks = order.price_ticks;
  result.order_quantity = order.quantity;
  result.decision_sequence_number = order.decision_sequence_number;
  result.decision_ts_ns = decision.available_at_ns;
  result.effective_arrival_ts_ns =
      decision.available_at_ns + ScaledQuantity(config.fixed_latency_ns, config.latency_multiplier);
  result.horizon_end_ts_ns = decision.available_at_ns + config.horizon_ns;
  const auto decision_snapshot = cursor.snapshot(config.depth);
  result.visible_qty_ahead_at_decision = VisibleAtPrice(live_orders, order.side, order.price_ticks);
  if (result.visible_qty_ahead_at_decision > 0 &&
      !SnapshotContainsPrice(decision_snapshot, order.side, order.price_ticks)) {
    throw std::runtime_error("order price outside requested snapshot depth");
  }
  result.hidden_ahead_multiplier = config.hidden_ahead_multiplier;
  result.latency_multiplier = config.latency_multiplier;
  result.cancel_rate_multiplier = config.cancel_rate_multiplier;
  result.queue_model_id = config.queue_model_id;
  result.latency_model_id = config.latency_model_id;
  result.markout_horizon_ns = config.markout_horizon_ns;
  result.source_replay_checksum = decision.replay_checksum;
  result.decision_snapshot_state_checksum = decision.snapshot_state_checksum;
  result.source_dataset_id = decision.dataset_id;

  auto visible_ahead =
      result.visible_qty_ahead_at_decision + ScaledQuantity(result.visible_qty_ahead_at_decision, config.hidden_ahead_multiplier);
  for (std::size_t i = decision_index + 1; i < events.size(); ++i) {
    const auto& event = events[i];
    const auto reduce_order = IsReduce(event.event_type) ? ReferencedLiveOrder(live_orders, event) : std::nullopt;
    const auto reduce_quantity =
        reduce_order.has_value() ? std::min(event.quantity, reduce_order->quantity) : event.quantity;
    const bool same_visible_price =
        event.event_type == "ADD"
            ? event.side == QueueSide(order.side) && event.price_ticks == order.price_ticks && event.quantity > 0
            : reduce_order.has_value() && reduce_order->side == QueueSide(order.side) &&
                  reduce_order->price_ticks == order.price_ticks && event.quantity > 0;
    if (event.receive_ts_ns > result.horizon_end_ts_ns || !same_visible_price) {
      ApplyLiveOrderEvent(live_orders, event);
      continue;
    }

    if (event.receive_ts_ns <= result.effective_arrival_ts_ns) {
      if (event.event_type == "ADD") {
        visible_ahead += event.quantity;
      } else if (IsReduce(event.event_type)) {
        visible_ahead = std::max<std::int64_t>(0, visible_ahead - reduce_quantity);
      }
      ApplyLiveOrderEvent(live_orders, event);
      continue;
    }

    if (event.event_type == "CANCEL" || event.event_type == "DELETE") {
      visible_ahead =
          std::max<std::int64_t>(0, visible_ahead - ScaledQuantity(reduce_quantity, config.cancel_rate_multiplier));
    } else if (event.event_type == "EXECUTE") {
      const auto ahead_consumed = std::min(visible_ahead, reduce_quantity);
      visible_ahead -= ahead_consumed;
      const auto fillable = reduce_quantity - ahead_consumed;
      const auto fill = std::min(order.quantity - result.filled_quantity, fillable);
      if (fill > 0) {
        if (!result.first_fill_ts_ns.has_value()) {
          result.first_fill_ts_ns = event.receive_ts_ns;
          result.first_fill_sequence_number = event.sequence_number;
        }
        result.filled_quantity += fill;
        if (result.filled_quantity == order.quantity) {
          break;
        }
      }
    }
    ApplyLiveOrderEvent(live_orders, event);
  }

  if (result.filled_quantity == order.quantity) {
    result.status = "FULL";
  } else if (result.filled_quantity > 0) {
    result.status = "PARTIAL_CENSORED";
  } else {
    result.status = "UNFILLED_CENSORED";
  }
  result.markout_ticks_x2 = MarkoutTicksX2(result, decision, features);
  return result;
}

std::string FillResultJson(const FillResult& result) {
  std::ostringstream out;
  bool first = true;
  out << '{';
  AddJsonField(out, "schema_version", JsonString(result.schema_version), first);
  AddJsonField(out, "simulation_id", JsonString(result.simulation_id), first);
  AddJsonField(out, "sim_order_id", JsonString(result.sim_order_id), first);
  AddJsonField(out, "queue_model_id", JsonString(result.queue_model_id), first);
  AddJsonField(out, "latency_model_id", JsonString(result.latency_model_id), first);
  AddJsonField(out, "side", JsonString(SideName(result.side)), first);
  AddJsonField(out, "price_ticks", std::to_string(result.price_ticks), first);
  AddJsonField(out, "order_quantity", std::to_string(result.order_quantity), first);
  AddJsonField(out, "decision_sequence_number", std::to_string(result.decision_sequence_number), first);
  AddJsonField(out, "decision_ts_ns", std::to_string(result.decision_ts_ns), first);
  AddJsonField(out, "effective_arrival_ts_ns", std::to_string(result.effective_arrival_ts_ns), first);
  AddJsonField(out, "horizon_end_ts_ns", std::to_string(result.horizon_end_ts_ns), first);
  AddJsonField(out, "visible_qty_ahead_at_decision", std::to_string(result.visible_qty_ahead_at_decision), first);
  AddJsonField(out, "hidden_ahead_multiplier", JsonDouble(result.hidden_ahead_multiplier), first);
  AddJsonField(out, "latency_multiplier", JsonDouble(result.latency_multiplier), first);
  AddJsonField(out, "cancel_rate_multiplier", JsonDouble(result.cancel_rate_multiplier), first);
  AddJsonField(out, "filled_quantity", std::to_string(result.filled_quantity), first);
  AddJsonField(out, "first_fill_ts_ns", JsonInt(result.first_fill_ts_ns), first);
  AddJsonField(out, "first_fill_sequence_number", JsonUint(result.first_fill_sequence_number), first);
  AddJsonField(out, "status", JsonString(result.status), first);
  AddJsonField(out, "markout_horizon_ns", std::to_string(result.markout_horizon_ns), first);
  AddJsonField(out, "markout_ticks_x2", JsonInt(result.markout_ticks_x2), first);
  AddJsonField(out, "source_replay_checksum", std::to_string(result.source_replay_checksum), first);
  AddJsonField(out, "decision_snapshot_state_checksum", std::to_string(result.decision_snapshot_state_checksum), first);
  AddJsonField(out, "source_dataset_id", JsonString(result.source_dataset_id), first);
  out << '}';
  return out.str();
}

std::string SimulationMetadataJson(const SimulationConfig& config,
                                   const std::filesystem::path& input,
                                   const std::filesystem::path& features,
                                   std::size_t result_count,
                                   std::uint64_t replay_checksum) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"schema_version\": \"vegaflux.canonical_market.v0.1\",\n";
  out << "  \"component_id\": " << JsonString(config.component_id) << ",\n";
  out << "  \"simulation_id\": " << JsonString(config.simulation_id) << ",\n";
  out << "  \"queue_model_id\": " << JsonString(config.queue_model_id) << ",\n";
  out << "  \"latency_model_id\": " << JsonString(config.latency_model_id) << ",\n";
  out << "  \"fixed_latency_ns\": " << config.fixed_latency_ns << ",\n";
  out << "  \"latency_multiplier\": " << config.latency_multiplier << ",\n";
  out << "  \"hidden_ahead_multiplier\": " << config.hidden_ahead_multiplier << ",\n";
  out << "  \"cancel_rate_multiplier\": " << config.cancel_rate_multiplier << ",\n";
  out << "  \"horizon_ns\": " << config.horizon_ns << ",\n";
  out << "  \"markout_horizon_ns\": " << config.markout_horizon_ns << ",\n";
  out << "  \"depth\": " << config.depth << ",\n";
  out << "  \"input\": " << JsonString(input.generic_string()) << ",\n";
  out << "  \"features\": " << JsonString(features.generic_string()) << ",\n";
  out << "  \"result_count\": " << result_count << ",\n";
  out << "  \"replay_checksum\": " << replay_checksum << ",\n";
  out << "  \"hidden_liquidity_policy\": \"not_modeled_as_ground_truth\",\n";
  out << "  \"limitations\": [\"visible FIFO baseline\", \"fixed latency\", \"JSONL artifact\"]\n";
  out << "}\n";
  return out.str();
}

void WriteFillsJsonl(const std::filesystem::path& path, const std::vector<FillResult>& results) {
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw std::runtime_error("failed to write fills: " + path.string());
  }
  for (const auto& result : results) {
    out << FillResultJson(result) << '\n';
  }
}

void WriteSimulationMetadata(const std::filesystem::path& path,
                             const SimulationConfig& config,
                             const std::filesystem::path& input,
                             const std::filesystem::path& features,
                             std::size_t result_count,
                             std::uint64_t replay_checksum) {
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw std::runtime_error("failed to write simulation metadata: " + path.string());
  }
  out << SimulationMetadataJson(config, input, features, result_count, replay_checksum);
}

}  // namespace vf_execution
