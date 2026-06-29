#include "vf_execution/execution.hpp"

#include "aegis_replay/replay.hpp"

#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Require(bool ok, const std::string& message) {
  if (!ok) {
    throw std::runtime_error(message);
  }
}

aegis_lob::CanonicalEvent Event(std::uint64_t seq,
                                std::int64_t receive_ts_ns,
                                const std::string& type,
                                aegis_lob::Side side,
                                std::int64_t price,
                                std::int64_t qty,
                                std::uint64_t order_id) {
  return {"vegaflux.canonical_market.v0.1", "SYNTH", "SYNTH-A", "SYNTH-EXEC-TEST", seq, receive_ts_ns - 10, receive_ts_ns,
          "VGFX", type, side, price, qty, order_id, type == "ADD" ? 0 : order_id};
}

struct FeatureSpec {
  std::uint64_t sequence_number{};
  std::int64_t available_at_ns{};
  std::optional<std::int64_t> mid_ticks_x2;
};

std::vector<vf_execution::FeatureRow> Features(const std::vector<aegis_lob::CanonicalEvent>& events,
                                               const std::vector<FeatureSpec>& specs) {
  aegis_replay::ReplayCursor cursor(events);
  std::vector<vf_execution::FeatureRow> rows;
  while (cursor.has_next()) {
    const auto& event = cursor.next_event();
    for (const auto& spec : specs) {
      if (event.sequence_number != spec.sequence_number) {
        continue;
      }
      vf_execution::FeatureRow row;
      row.schema_version = "vegaflux.canonical_market.v0.1";
      row.dataset_id = "execution_test_features";
      row.row_index = rows.size();
      row.sequence_number = event.sequence_number;
      row.source_sequence_number = event.sequence_number;
      row.available_at_ns = spec.available_at_ns;
      row.snapshot_state_checksum = cursor.state_checksum();
      row.mid_ticks_x2 = spec.mid_ticks_x2;
      rows.push_back(row);
    }
  }
  const auto replay_checksum = cursor.state_checksum();
  for (auto& row : rows) {
    row.replay_checksum = replay_checksum;
  }
  return rows;
}

vf_execution::SimulationConfig Config() {
  vf_execution::SimulationConfig config;
  config.fixed_latency_ns = 0;
  config.horizon_ns = 1'000;
  config.markout_horizon_ns = 10;
  return config;
}

vf_execution::SimOrder Buy(std::int64_t qty) {
  vf_execution::SimOrder order;
  order.order_id = "test-buy";
  order.side = vf_execution::Side::kBuy;
  order.price_ticks = 100;
  order.quantity = qty;
  order.decision_sequence_number = 1;
  return order;
}

void CheckFullFillAndMarkout() {
  const std::vector<aegis_lob::CanonicalEvent> events{
      Event(1, 100, "ADD", aegis_lob::Side::kBid, 100, 5, 1),
      Event(2, 120, "ADD", aegis_lob::Side::kBid, 100, 3, 2),
      Event(3, 200, "EXECUTE", aegis_lob::Side::kBid, 100, 5, 1),
      Event(4, 300, "EXECUTE", aegis_lob::Side::kBid, 100, 3, 2),
  };
  const auto features = Features(events, {{1, 100, 200}, {4, 310, 204}});
  const auto result = vf_execution::SimulateVisibleFifo(Config(), Buy(3), events, features);
  Require(result.visible_qty_ahead_at_decision == 5, "visible ahead mismatch");
  Require(result.filled_quantity == 3, "full fill quantity mismatch");
  Require(result.first_fill_ts_ns == 300, "first fill timestamp mismatch");
  Require(result.first_fill_sequence_number == 4, "first fill sequence mismatch");
  Require(result.status == "FULL", "full fill status mismatch");
  Require(result.markout_ticks_x2 == 4, "markout mismatch");
  Require(result.queue_model_id == "visible_fifo_v1", "missing queue model id");
  Require(result.latency_model_id == "fixed_latency_ns_v1", "missing latency model id");
}

void CheckPartialAndCensored() {
  const std::vector<aegis_lob::CanonicalEvent> events{
      Event(1, 100, "ADD", aegis_lob::Side::kBid, 100, 5, 1),
      Event(2, 120, "ADD", aegis_lob::Side::kBid, 100, 2, 2),
      Event(3, 200, "EXECUTE", aegis_lob::Side::kBid, 100, 5, 1),
      Event(4, 300, "EXECUTE", aegis_lob::Side::kBid, 100, 2, 2),
  };
  const auto features = Features(events, {{1, 100, 200}, {4, 310, 200}});
  const auto result = vf_execution::SimulateVisibleFifo(Config(), Buy(3), events, features);
  Require(result.filled_quantity == 2, "partial fill quantity mismatch");
  Require(result.status == "PARTIAL_CENSORED", "partial status mismatch");
}

void CheckCancelAheadThenFill() {
  const std::vector<aegis_lob::CanonicalEvent> events{
      Event(1, 100, "ADD", aegis_lob::Side::kBid, 100, 5, 1),
      Event(2, 200, "CANCEL", aegis_lob::Side::kBid, 100, 5, 1),
      Event(3, 220, "ADD", aegis_lob::Side::kBid, 100, 3, 2),
      Event(4, 300, "EXECUTE", aegis_lob::Side::kBid, 100, 3, 2),
  };
  const auto features = Features(events, {{1, 100, 200}, {4, 310, 200}});
  const auto result = vf_execution::SimulateVisibleFifo(Config(), Buy(3), events, features);
  Require(result.filled_quantity == 3, "cancel-ahead fill mismatch");
  Require(result.status == "FULL", "cancel-ahead status mismatch");
}

void CheckDeleteAheadThenFill() {
  const std::vector<aegis_lob::CanonicalEvent> events{
      Event(1, 100, "ADD", aegis_lob::Side::kBid, 100, 5, 1),
      Event(2, 200, "DELETE", aegis_lob::Side::kBid, 100, 5, 1),
      Event(3, 220, "ADD", aegis_lob::Side::kBid, 100, 3, 2),
      Event(4, 300, "EXECUTE", aegis_lob::Side::kBid, 100, 3, 2),
  };
  const auto features = Features(events, {{1, 100, 200}, {4, 310, 200}});
  const auto result = vf_execution::SimulateVisibleFifo(Config(), Buy(3), events, features);
  Require(result.filled_quantity == 3, "delete-ahead fill mismatch");
  Require(result.status == "FULL", "delete-ahead status mismatch");
}

void CheckFixedLatency() {
  auto config = Config();
  config.fixed_latency_ns = 100;
  const std::vector<aegis_lob::CanonicalEvent> events{
      Event(1, 100, "ADD", aegis_lob::Side::kBid, 100, 5, 1),
      Event(2, 150, "ADD", aegis_lob::Side::kBid, 100, 4, 2),
      Event(3, 160, "EXECUTE", aegis_lob::Side::kBid, 100, 5, 1),
      Event(4, 250, "ADD", aegis_lob::Side::kBid, 100, 3, 3),
      Event(5, 300, "EXECUTE", aegis_lob::Side::kBid, 100, 4, 2),
      Event(6, 350, "EXECUTE", aegis_lob::Side::kBid, 100, 3, 3),
  };
  const auto features = Features(events, {{1, 100, 200}, {6, 360, 200}});
  const auto result = vf_execution::SimulateVisibleFifo(config, Buy(3), events, features);
  Require(result.effective_arrival_ts_ns == 200, "fixed latency not applied");
  Require(result.filled_quantity == 3, "latency fill quantity mismatch");
  Require(result.first_fill_ts_ns == 350, "latency first fill mismatch");
}

void CheckHiddenLiquidityIsNotTruth() {
  const std::vector<aegis_lob::CanonicalEvent> events{
      Event(1, 100, "ADD", aegis_lob::Side::kAsk, 101, 5, 1),
      Event(2, 200, "TRADE", aegis_lob::Side::kBid, 100, 100, 2),
  };
  const auto features = Features(events, {{1, 100, 200}, {2, 200, 200}});
  const auto result = vf_execution::SimulateVisibleFifo(Config(), Buy(3), events, features);
  Require(result.visible_qty_ahead_at_decision == 0, "unexpected displayed ahead");
  Require(result.filled_quantity == 0, "TRADE-only hidden liquidity was treated as fill");
  Require(result.status == "UNFILLED_CENSORED", "hidden-liquidity status mismatch");
  Require(vf_execution::FillResultJson(result).find("hidden_ahead_quantity") == std::string::npos,
          "fill row leaked hidden quantity as truth");
}

template <typename Fn>
void RequireThrows(Fn fn, const std::string& message_fragment) {
  try {
    fn();
  } catch (const std::exception& error) {
    Require(std::string(error.what()).find(message_fragment) != std::string::npos,
            "wrong exception: " + std::string(error.what()));
    return;
  }
  throw std::runtime_error("expected exception: " + message_fragment);
}

void CheckConfigValidation() {
  const std::vector<aegis_lob::CanonicalEvent> events{Event(1, 100, "ADD", aegis_lob::Side::kBid, 100, 5, 1)};
  const auto features = Features(events, {{1, 100, 200}});
  auto config = Config();
  auto order = Buy(1);
  auto bad_queue = config;
  bad_queue.queue_model_id = "visible_fifo_labeled_as_other";
  RequireThrows([&] { vf_execution::SimulateVisibleFifo(bad_queue, order, events, features); },
                "unsupported queue_model_id");
  auto bad_latency = config;
  bad_latency.latency_model_id = "mislabeled_latency";
  RequireThrows([&] { vf_execution::SimulateVisibleFifo(bad_latency, order, events, features); },
                "unsupported latency_model_id");
  auto bad_depth = config;
  bad_depth.depth = 0;
  RequireThrows([&] { vf_execution::SimulateVisibleFifo(bad_depth, order, events, features); }, "depth must be positive");
  auto bad_markout = config;
  bad_markout.markout_horizon_ns = -1;
  RequireThrows([&] { vf_execution::SimulateVisibleFifo(bad_markout, order, events, features); },
                "markout_horizon_ns must be non-negative");
}

void CheckLineageValidation() {
  const std::vector<aegis_lob::CanonicalEvent> events{Event(1, 100, "ADD", aegis_lob::Side::kBid, 100, 5, 1)};
  auto features = Features(events, {{1, 100, 200}});
  auto bad_replay = features;
  ++bad_replay[0].replay_checksum;
  RequireThrows([&] { vf_execution::SimulateVisibleFifo(Config(), Buy(1), events, bad_replay); },
                "source replay checksum mismatch");
  auto bad_snapshot = features;
  ++bad_snapshot[0].snapshot_state_checksum;
  RequireThrows([&] { vf_execution::SimulateVisibleFifo(Config(), Buy(1), events, bad_snapshot); },
                "decision snapshot checksum mismatch");
}

void CheckReferenceOrderReduceMatching() {
  const std::vector<aegis_lob::CanonicalEvent> events{
      Event(1, 100, "ADD", aegis_lob::Side::kBid, 100, 5, 1),
      Event(2, 150, "ADD", aegis_lob::Side::kBid, 100, 2, 2),
      Event(3, 200, "EXECUTE", aegis_lob::Side::kBid, 99, 5, 1),
      Event(4, 300, "EXECUTE", aegis_lob::Side::kBid, 100, 2, 2),
  };
  const auto features = Features(events, {{1, 100, 200}, {4, 310, 200}});
  const auto result = vf_execution::SimulateVisibleFifo(Config(), Buy(2), events, features);
  Require(result.filled_quantity == 2, "reference-order reduce did not deplete visible ahead");
  Require(result.status == "FULL", "reference-order reduce status mismatch");
}

void CheckOffDepthPriceFails() {
  const std::vector<aegis_lob::CanonicalEvent> events{
      Event(1, 100, "ADD", aegis_lob::Side::kBid, 100, 5, 1),
      Event(2, 110, "ADD", aegis_lob::Side::kBid, 99, 1, 2),
  };
  auto order = Buy(1);
  order.price_ticks = 99;
  order.decision_sequence_number = 2;
  auto config = Config();
  config.depth = 1;
  const auto features = Features(events, {{2, 110, 200}});
  RequireThrows([&] { vf_execution::SimulateVisibleFifo(config, order, events, features); },
                "order price outside requested snapshot depth");
}

void CheckScenarioSensitivity() {
  const std::vector<aegis_lob::CanonicalEvent> events{
      Event(1, 100, "ADD", aegis_lob::Side::kBid, 100, 10, 1),
      Event(2, 160, "CANCEL", aegis_lob::Side::kBid, 100, 4, 1),
      Event(3, 170, "ADD", aegis_lob::Side::kBid, 100, 3, 2),
      Event(4, 220, "EXECUTE", aegis_lob::Side::kBid, 100, 6, 1),
      Event(5, 320, "EXECUTE", aegis_lob::Side::kBid, 100, 3, 2),
  };
  const auto features = Features(events, {{1, 100, 200}, {5, 330, 204}});
  auto config = Config();
  config.fixed_latency_ns = 50;
  config.markout_horizon_ns = 10;
  auto order = Buy(3);
  const auto baseline = vf_execution::SimulateVisibleFifo(config, order, events, features);
  Require(baseline.status == "FULL" && baseline.filled_quantity == 3 && baseline.markout_ticks_x2 == 4,
          "baseline sensitivity fixture mismatch");
  auto hidden = config;
  hidden.hidden_ahead_multiplier = 1.0;
  const auto hidden_result = vf_execution::SimulateVisibleFifo(hidden, order, events, features);
  Require(hidden_result.status == "UNFILLED_CENSORED", "hidden-ahead sensitivity mismatch");
  auto slow = config;
  slow.latency_multiplier = 4.0;
  const auto slow_result = vf_execution::SimulateVisibleFifo(slow, order, events, features);
  Require(slow_result.status == "UNFILLED_CENSORED", "latency sensitivity mismatch");
  auto cancel_shock = config;
  cancel_shock.cancel_rate_multiplier = 0.5;
  const auto cancel_result = vf_execution::SimulateVisibleFifo(cancel_shock, order, events, features);
  Require(cancel_result.status == "PARTIAL_CENSORED" && cancel_result.filled_quantity == 1,
          "cancel shock sensitivity mismatch");
}

}  // namespace

int main() {
  try {
    CheckFullFillAndMarkout();
    CheckPartialAndCensored();
    CheckCancelAheadThenFill();
    CheckDeleteAheadThenFill();
    CheckFixedLatency();
    CheckHiddenLiquidityIsNotTruth();
    CheckConfigValidation();
    CheckLineageValidation();
    CheckReferenceOrderReduceMatching();
    CheckOffDepthPriceFails();
    CheckScenarioSensitivity();
    std::cout << "execution_tests pass\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
