#include "aegis_lob/book.hpp"
#include "aegis_replay/replay.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct RefLevel {
  std::int64_t quantity{};
  std::uint32_t order_count{};
};

struct RefOrder {
  aegis_lob::Side side{aegis_lob::Side::kUnspecified};
  std::int64_t price_ticks{};
  std::int64_t quantity{};
};

struct RefBook {
  std::map<std::uint64_t, RefOrder> orders;
  std::map<std::int64_t, RefLevel, std::greater<>> bids;
  std::map<std::int64_t, RefLevel> asks;

  void Apply(const aegis_lob::CanonicalEvent& event) {
    if (event.event_type == "ADD" && event.quantity > 0) {
      orders[event.order_id] = {event.side, event.price_ticks, event.quantity};
      auto& level = event.side == aegis_lob::Side::kBid ? bids[event.price_ticks] : asks[event.price_ticks];
      level.quantity += event.quantity;
      ++level.order_count;
      return;
    }
    if (event.event_type != "EXECUTE" && event.event_type != "CANCEL") {
      return;
    }
    const auto order_id = event.reference_order_id != 0 ? event.reference_order_id : event.order_id;
    const auto found = orders.find(order_id);
    if (found == orders.end()) {
      return;
    }
    auto& order = found->second;
    const auto reduction = std::min(event.quantity, order.quantity);
    order.quantity -= reduction;
    if (order.side == aegis_lob::Side::kBid) {
      auto level = bids.find(order.price_ticks);
      if (level != bids.end()) {
        level->second.quantity -= reduction;
        if (order.quantity == 0 && level->second.order_count > 0) {
          --level->second.order_count;
        }
        if (level->second.quantity <= 0 || level->second.order_count == 0) {
          bids.erase(level);
        }
      }
    } else {
      auto level = asks.find(order.price_ticks);
      if (level != asks.end()) {
        level->second.quantity -= reduction;
        if (order.quantity == 0 && level->second.order_count > 0) {
          --level->second.order_count;
        }
        if (level->second.quantity <= 0 || level->second.order_count == 0) {
          asks.erase(level);
        }
      }
    }
    if (order.quantity == 0) {
      orders.erase(found);
    }
  }
};

void Require(bool ok, const std::string& message) {
  if (!ok) {
    throw std::runtime_error(message);
  }
}

void CompareLevels(const std::vector<aegis_lob::PriceLevel>& got,
                   const std::map<std::int64_t, RefLevel, std::greater<>>& expected) {
  Require(got.size() == expected.size(), "bid reference depth mismatch");
  std::size_t index = 0;
  for (const auto& [price, level] : expected) {
    Require(got[index].price_ticks == price, "bid reference price mismatch");
    Require(got[index].quantity == level.quantity, "bid reference quantity mismatch");
    Require(got[index].order_count == level.order_count, "bid reference order count mismatch");
    ++index;
  }
}

void CompareLevels(const std::vector<aegis_lob::PriceLevel>& got,
                   const std::map<std::int64_t, RefLevel>& expected) {
  Require(got.size() == expected.size(), "ask reference depth mismatch");
  std::size_t index = 0;
  for (const auto& [price, level] : expected) {
    Require(got[index].price_ticks == price, "ask reference price mismatch");
    Require(got[index].quantity == level.quantity, "ask reference quantity mismatch");
    Require(got[index].order_count == level.order_count, "ask reference order count mismatch");
    ++index;
  }
}

aegis_replay::ReplayCursor ReplayAll(const std::filesystem::path& path) {
  auto cursor = aegis_replay::ReplayCursor::FromJsonl(path);
  cursor.replay_all();
  return cursor;
}

void CheckGoldenFinalBook(const std::filesystem::path& root) {
  const auto cursor = ReplayAll(root / "artifacts/decoder/decoded.normalized.jsonl");
  const auto snapshot = cursor.snapshot(10);
  Require(snapshot.sequence_number == 5, "final sequence mismatch");
  Require(snapshot.bids.size() == 2, "bid depth mismatch");
  Require(snapshot.asks.size() == 1, "ask depth mismatch");
  Require(snapshot.bids[0].price_ticks == 10000 && snapshot.bids[0].quantity == 6, "best bid mismatch");
  Require(snapshot.bids[1].price_ticks == 9999 && snapshot.bids[1].quantity == 7, "second bid mismatch");
  Require(snapshot.asks[0].price_ticks == 10005 && snapshot.asks[0].quantity == 10, "best ask mismatch");
  Require(cursor.book().anomalies().empty(), "golden replay emitted anomalies");
}

void CheckDeterministicChecksum(const std::filesystem::path& root) {
  const auto path = root / "artifacts/decoder/decoded.normalized.jsonl";
  const auto first = ReplayAll(path).state_checksum();
  const auto second = ReplayAll(path).state_checksum();
  Require(first == second, "replay checksum changed across runs");
}

void CheckCheckpointRestore(const std::filesystem::path& root) {
  const auto path = root / "artifacts/decoder/decoded.normalized.jsonl";
  auto direct = ReplayAll(path);
  auto checkpointed = aegis_replay::ReplayCursor::FromJsonl(path);
  checkpointed.seek_seq(4);
  const auto checkpoint_path = std::filesystem::temp_directory_path() / "checkpoint_restore_test.txt";
  checkpointed.save_checkpoint(checkpoint_path);
  auto restored = aegis_replay::ReplayCursor::FromJsonl(path);
  restored.load_checkpoint(checkpoint_path);
  restored.replay_all();
  Require(restored.state_checksum() == direct.state_checksum(), "checkpoint replay checksum mismatch");
}

void CheckInvalidEventAnomalies() {
  std::vector<aegis_lob::CanonicalEvent> events;
  events.push_back({"vegaflux.canonical_market.v0.1", "SYNTH", "SYNTH-A", "SYNTH-0001", 1, 1, 1, "VGFX", "ADD",
                    aegis_lob::Side::kBid, 100, 10, 1, 0});
  events.push_back({"vegaflux.canonical_market.v0.1", "SYNTH", "SYNTH-A", "SYNTH-0001", 2, 2, 2, "VGFX", "EXECUTE",
                    aegis_lob::Side::kBid, 100, 1, 999, 999});
  events.push_back({"vegaflux.canonical_market.v0.1", "SYNTH", "SYNTH-A", "SYNTH-0001", 3, 3, 3, "VGFX", "CANCEL",
                    aegis_lob::Side::kBid, 100, 1, 998, 998});
  aegis_replay::ReplayCursor cursor(events);
  cursor.replay_all();
  const auto snapshot = cursor.snapshot(5);
  Require(cursor.book().anomalies().size() == 2, "invalid references should emit two anomalies");
  Require(cursor.book().anomalies()[0].sequence_number == 2, "anomaly sequence was not recorded");
  Require(cursor.book().anomalies()[0].order_id == 999, "anomaly order id was not recorded");
  Require(cursor.book().anomalies()[0].reason == "unknown_reference_order_id", "anomaly reason was not recorded");
  const auto anomaly_jsonl = aegis_lob::AnomalyJsonl(cursor.book().anomalies());
  Require(anomaly_jsonl.find("\"sequence_number\":2") != std::string::npos, "anomaly JSONL missing sequence");
  Require(anomaly_jsonl.find("\"order_id\":999") != std::string::npos, "anomaly JSONL missing order id");
  Require(anomaly_jsonl.find("\"reason\":\"unknown_reference_order_id\"") != std::string::npos,
          "anomaly JSONL missing reason");
  Require(snapshot.bids.size() == 1 && snapshot.bids[0].quantity == 10, "invalid references mutated book");
}

void CheckParserWhitespaceAndEscapes() {
  const auto event = aegis_lob::ParseCanonicalJson(
      "{ \"schema_version\" : \"vegaflux.canonical_market.v0.1\", \"venue\" : \"SYNTH\", \"channel\" : \"SYNTH-A\", "
      "\"session\" : \"VG\\\\TEST\", \"sequence_number\" : 42, \"event_ts_ns\" : 7, \"receive_ts_ns\" : 8, "
      "\"symbol\" : \"VG\\\"TEST\", \"event_type\" : \"ADD\", \"side\" : \"BID\", \"price_ticks\" : 100, "
      "\"quantity\" : 3, \"order_id\" : 55, \"reference_order_id\" : 0 }");
  Require(event.sequence_number == 42, "whitespace JSON parse sequence mismatch");
  Require(event.session == "VG\\TEST", "escaped backslash parse mismatch");
  Require(event.symbol == "VG\"TEST", "escaped quote parse mismatch");
  Require(event.side == aegis_lob::Side::kBid, "whitespace JSON parse side mismatch");
}

void CheckCrossedBookAnomalies() {
  std::vector<aegis_lob::CanonicalEvent> events;
  events.push_back({"vegaflux.canonical_market.v0.1", "SYNTH", "SYNTH-A", "SYNTH-0001", 1, 1, 1, "VGFX", "ADD",
                    aegis_lob::Side::kAsk, 101, 10, 1, 0});
  events.push_back({"vegaflux.canonical_market.v0.1", "SYNTH", "SYNTH-A", "SYNTH-0001", 2, 2, 2, "VGFX", "ADD",
                    aegis_lob::Side::kBid, 101, 5, 2, 0});
  events.push_back({"vegaflux.canonical_market.v0.1", "SYNTH", "SYNTH-A", "SYNTH-0001", 3, 3, 3, "VGFX", "ADD",
                    aegis_lob::Side::kBid, 100, 5, 3, 0});
  events.push_back({"vegaflux.canonical_market.v0.1", "SYNTH", "SYNTH-A", "SYNTH-0001", 4, 4, 4, "VGFX", "REPLACE",
                    aegis_lob::Side::kBid, 102, 5, 4, 3});
  aegis_replay::ReplayCursor cursor(events);
  cursor.replay_all();
  const auto snapshot = cursor.snapshot(10);
  Require(cursor.book().anomalies().size() == 2, "crossed add/replace should emit anomalies");
  Require(cursor.book().anomalies()[0].sequence_number == 2, "crossed add anomaly sequence mismatch");
  Require(cursor.book().anomalies()[0].order_id == 2, "crossed add anomaly order mismatch");
  Require(cursor.book().anomalies()[0].reason == "crossed_book_bid", "crossed add anomaly reason mismatch");
  Require(cursor.book().anomalies()[1].event_type == "REPLACE", "crossed replace anomaly type mismatch");
  Require(snapshot.bids.size() == 1 && snapshot.bids[0].price_ticks == 100 && snapshot.bids[0].quantity == 5,
          "crossed replace mutated bid");
  Require(snapshot.asks.size() == 1 && snapshot.asks[0].price_ticks == 101 && snapshot.asks[0].quantity == 10,
          "crossed handling mutated ask");
}

void CheckGapBoundary(const std::filesystem::path& root) {
  auto cursor = aegis_replay::ReplayCursor::FromJsonl(root / "artifacts/decoder/gap.normalized.jsonl");
  cursor.next_event();
  cursor.next_event();
  const auto before_gap = cursor.state_checksum();
  const auto& gap = cursor.next_event();
  Require(gap.event_type == "GAP_DETECTED", "expected gap event");
  Require(cursor.state_checksum() == before_gap, "gap event mutated book checksum");
  Require(cursor.book().audit_boundaries().size() == 1, "gap boundary was not recorded");
  cursor.replay_all();
  const auto snapshot = cursor.snapshot(10);
  Require(snapshot.asks.size() == 1 && snapshot.asks[0].quantity == 10, "post-gap ask state mismatch");
}

void CheckSeekBySequence(const std::filesystem::path& root) {
  auto cursor = aegis_replay::ReplayCursor::FromJsonl(root / "artifacts/decoder/decoded.normalized.jsonl");
  cursor.seek_seq(4);
  Require(cursor.snapshot(10).bids[0].quantity == 6, "seek_seq did not apply earlier execution");
  const auto& next = cursor.next_event();
  Require(next.sequence_number == 4 && next.event_type == "CANCEL", "seek_seq next event mismatch");
}

void CheckVf1rExtendedReplay(const std::filesystem::path& root) {
  auto cursor = aegis_replay::ReplayCursor::FromJsonl(root / "artifacts/decoder-validation/extended.normalized.jsonl");
  cursor.replay_all();
  const auto snapshot = cursor.snapshot(10);
  Require(cursor.book().anomalies().empty(), "decoder-validation extended replay should not emit anomalies");
  Require(cursor.book().audit_boundaries().size() == 2, "decoder-validation extended replay should audit control/trade events");
  Require(cursor.book().audit_boundaries()[0].event_type == "BROKEN_TRADE", "broken trade audit missing");
  Require(cursor.book().audit_boundaries()[0].reason == "broken_trade_not_reversed", "broken trade audit reason mismatch");
  Require(cursor.book().audit_boundaries()[1].event_type == "SESSION_RESET", "session reset audit missing");
  Require(snapshot.sequence_number == 1, "post-reset sequence mismatch");
  Require(snapshot.bids.size() == 1 && snapshot.bids[0].price_ticks == 10090 && snapshot.bids[0].quantity == 3,
          "post-reset decoder-validation bid mismatch");
  Require(snapshot.asks.empty(), "session reset should clear prior ask");
}

void CheckCheckpointPreservesAuditAnomalyAndCursor() {
  std::vector<aegis_lob::CanonicalEvent> events;
  events.push_back({"vegaflux.canonical_market.v0.1", "SYNTH", "SYNTH-A", "SYNTH-0001", 1, 1, 1, "VGFX", "ADD",
                    aegis_lob::Side::kAsk, 100, 10, 1, 0});
  events.push_back({"vegaflux.canonical_market.v0.1", "SYNTH", "SYNTH-A", "SYNTH-0001", 2, 2, 2, "VGFX", "ADD",
                    aegis_lob::Side::kBid, 100, 1, 2, 0});
  events.push_back({"vegaflux.canonical_market.v0.1", "SYNTH", "SYNTH-A", "SYNTH-0001", 3, 3, 3, "VGFX",
                    "GAP_DETECTED", aegis_lob::Side::kUnspecified, 0, 1, 0, 0});
  events.push_back({"vegaflux.canonical_market.v0.1", "SYNTH", "SYNTH-A", "SYNTH-0001", 4, 4, 4, "VGFX",
                    "SESSION_RESET", aegis_lob::Side::kUnspecified, 0, 0, 0, 0});
  events.push_back({"vegaflux.canonical_market.v0.1", "SYNTH", "SYNTH-A", "SYNTH-0001", 1, 5, 5, "VGFX", "ADD",
                    aegis_lob::Side::kBid, 90, 2, 3, 0});

  aegis_replay::ReplayCursor cursor(events);
  for (int i = 0; i < 4; ++i) {
    cursor.next_event();
  }
  const auto checkpoint_path = std::filesystem::temp_directory_path() / "checkpoint_audit_test.txt";
  cursor.save_checkpoint(checkpoint_path);
  aegis_replay::ReplayCursor restored(events);
  restored.load_checkpoint(checkpoint_path);
  Require(restored.events_replayed() == 4, "checkpoint cursor index was not restored");
  Require(restored.book().anomalies().size() == 1, "checkpoint anomaly history was not restored");
  Require(restored.book().audit_boundaries().size() == 2, "checkpoint audit history was not restored");
  restored.replay_all();
  const auto snapshot = restored.snapshot(10);
  Require(snapshot.bids.size() == 1 && snapshot.bids[0].price_ticks == 90, "checkpoint replay skipped reset sequence");
}

void CheckRandomReferenceReplay() {
  std::uint64_t rng = 0x9e3779b97f4a7c15ull;
  auto next = [&rng]() {
    rng = rng * 6364136223846793005ull + 1;
    return rng;
  };

  std::vector<aegis_lob::CanonicalEvent> events;
  std::map<std::uint64_t, RefOrder> generator_orders;
  std::uint64_t next_order_id = 10'000;
  for (std::uint64_t seq = 1; seq <= 300; ++seq) {
    if (generator_orders.empty() || next() % 3 == 0) {
      const auto side = next() % 2 == 0 ? aegis_lob::Side::kBid : aegis_lob::Side::kAsk;
      const auto price = side == aegis_lob::Side::kBid ? 9'900 + static_cast<std::int64_t>(next() % 50)
                                                       : 10'001 + static_cast<std::int64_t>(next() % 50);
      const auto quantity = 1 + static_cast<std::int64_t>(next() % 25);
      const auto order_id = next_order_id++;
      generator_orders[order_id] = {side, price, quantity};
      events.push_back({"vegaflux.canonical_market.v0.1", "SYNTH", "SYNTH-A", "SYNTH-0001", seq, static_cast<std::int64_t>(seq),
                        static_cast<std::int64_t>(seq), "VGFX", "ADD", side, price, quantity, order_id, 0});
      continue;
    }

    auto found = generator_orders.begin();
    std::advance(found, static_cast<long long>(next() % generator_orders.size()));
    const auto order_id = found->first;
    auto order = found->second;
    const auto quantity = 1 + static_cast<std::int64_t>(next() % static_cast<std::uint64_t>(order.quantity));
    const auto type = next() % 2 == 0 ? "EXECUTE" : "CANCEL";
    events.push_back({"vegaflux.canonical_market.v0.1", "SYNTH", "SYNTH-A", "SYNTH-0001", seq, static_cast<std::int64_t>(seq),
                      static_cast<std::int64_t>(seq), "VGFX", type, order.side, order.price_ticks, quantity, order_id,
                      order_id});
    order.quantity -= quantity;
    if (order.quantity == 0) {
      generator_orders.erase(found);
    } else {
      found->second = order;
    }
  }

  RefBook reference;
  for (const auto& event : events) {
    reference.Apply(event);
  }
  aegis_replay::ReplayCursor cursor(events);
  cursor.replay_all();
  const auto snapshot = cursor.snapshot(1'000);
  CompareLevels(snapshot.bids, reference.bids);
  CompareLevels(snapshot.asks, reference.asks);
  Require(cursor.book().live_order_count() == reference.orders.size(), "reference live order count mismatch");
}

void CheckMutatedSingleEventRegression(const std::filesystem::path& root) {
  auto events = aegis_lob::LoadCanonicalJsonl(root / "artifacts/decoder/decoded.normalized.jsonl");
  events[3].order_id = 999'999;
  events[3].reference_order_id = 999'999;
  aegis_replay::ReplayCursor cursor(events);
  cursor.replay_all();
  const auto snapshot = cursor.snapshot(10);
  Require(cursor.book().anomalies().size() == 1, "mutated cancel should emit one anomaly");
  Require(snapshot.asks.size() == 1 && snapshot.asks[0].quantity == 12, "mutated cancel should not reduce ask");
}

void CheckLargeSyntheticMemoryGrowthSmoke() {
  std::vector<aegis_lob::CanonicalEvent> events;
  for (std::uint64_t i = 1; i <= 2'000; ++i) {
    events.push_back({"vegaflux.canonical_market.v0.1", "SYNTH", "SYNTH-A", "SYNTH-0001", i, static_cast<std::int64_t>(i),
                      static_cast<std::int64_t>(i), "VGFX", "ADD", aegis_lob::Side::kBid,
                      10'000 + static_cast<std::int64_t>(i % 10), 1, i, 0});
  }
  for (std::uint64_t i = 1; i <= 2'000; ++i) {
    events.push_back({"vegaflux.canonical_market.v0.1", "SYNTH", "SYNTH-A", "SYNTH-0001", 2'000 + i,
                      static_cast<std::int64_t>(2'000 + i), static_cast<std::int64_t>(2'000 + i), "VGFX", "CANCEL",
                      aegis_lob::Side::kBid, 10'000 + static_cast<std::int64_t>(i % 10), 1, i, i});
  }
  aegis_replay::ReplayCursor cursor(events);
  cursor.replay_all();
  Require(cursor.book().live_order_count() == 0, "large synthetic replay leaked live orders");
  Require(cursor.snapshot(10).bids.empty(), "large synthetic replay left bid levels");
  Require(cursor.book().anomalies().empty(), "large synthetic replay emitted anomalies");
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const std::filesystem::path root = argc > 1 ? argv[1] : ".";
    CheckGoldenFinalBook(root);
    CheckDeterministicChecksum(root);
    CheckCheckpointRestore(root);
    CheckInvalidEventAnomalies();
    CheckParserWhitespaceAndEscapes();
    CheckCrossedBookAnomalies();
    CheckGapBoundary(root);
    CheckSeekBySequence(root);
    CheckVf1rExtendedReplay(root);
    CheckCheckpointPreservesAuditAnomalyAndCursor();
    CheckRandomReferenceReplay();
    CheckMutatedSingleEventRegression(root);
    CheckLargeSyntheticMemoryGrowthSmoke();
    std::cout << "replay_tests pass\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
