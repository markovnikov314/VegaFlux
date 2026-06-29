#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace aegis_lob {

enum class Side { kUnspecified, kBid, kAsk };

struct CanonicalEvent {
  std::string schema_version;
  std::string venue;
  std::string channel;
  std::string session;
  std::uint64_t sequence_number{};
  std::int64_t event_ts_ns{};
  std::int64_t receive_ts_ns{};
  std::string symbol;
  std::string event_type;
  Side side{Side::kUnspecified};
  std::int64_t price_ticks{};
  std::int64_t quantity{};
  std::uint64_t order_id{};
  std::uint64_t reference_order_id{};
};

struct OrderRecord {
  std::uint64_t order_id{};
  Side side{Side::kUnspecified};
  std::int64_t price_ticks{};
  std::int64_t quantity{};
};

struct PriceLevel {
  std::int64_t price_ticks{};
  std::int64_t quantity{};
  std::uint32_t order_count{};
};

struct BookSnapshot {
  std::string schema_version;
  std::string venue;
  std::string symbol;
  std::string session;
  std::uint64_t sequence_number{};
  std::int64_t event_ts_ns{};
  std::vector<PriceLevel> bids;
  std::vector<PriceLevel> asks;
  std::uint64_t state_checksum{};
  std::string checkpoint_id;
};

struct Anomaly {
  std::uint64_t sequence_number{};
  std::int64_t event_ts_ns{};
  std::string event_type;
  std::uint64_t order_id{};
  std::uint64_t reference_order_id{};
  std::string reason;
};

struct AuditBoundary {
  std::uint64_t sequence_number{};
  std::int64_t event_ts_ns{};
  std::string event_type;
  std::uint64_t order_id{};
  std::int64_t missing_count{};
  std::string reason;
};

class Book {
 public:
  void Apply(const CanonicalEvent& event);
  void Clear();

  BookSnapshot Snapshot(std::uint32_t depth, const std::string& checkpoint_id = "") const;
  std::uint64_t StateChecksum() const;

  void SaveCheckpoint(const std::filesystem::path& path) const;
  void LoadCheckpoint(const std::filesystem::path& path);

  const std::vector<Anomaly>& anomalies() const { return anomalies_; }
  const std::vector<AuditBoundary>& audit_boundaries() const { return audit_boundaries_; }
  std::uint64_t last_sequence_number() const { return last_sequence_number_; }
  std::size_t live_order_count() const { return orders_.size(); }

 private:
  void RememberMetadata(const CanonicalEvent& event);
  void AddOrder(const CanonicalEvent& event);
  void ReplaceOrder(const CanonicalEvent& event);
  void ReduceOrder(const CanonicalEvent& event);
  void DeleteOrder(const CanonicalEvent& event);
  void RecordAudit(const CanonicalEvent& event, const std::string& reason, std::int64_t missing_count = 0);
  void RecordAnomaly(const CanonicalEvent& event, std::uint64_t order_id, const std::string& reason);
  void InsertRestoredOrder(const OrderRecord& order);
  void RemoveOrder(const OrderRecord& order);
  bool WouldCross(Side side, std::int64_t price_ticks) const;

  std::map<std::uint64_t, OrderRecord> orders_;
  std::map<std::int64_t, PriceLevel, std::greater<>> bids_;
  std::map<std::int64_t, PriceLevel> asks_;
  std::vector<Anomaly> anomalies_;
  std::vector<AuditBoundary> audit_boundaries_;
  std::string schema_version_;
  std::string venue_;
  std::string symbol_;
  std::string session_;
  std::uint64_t last_sequence_number_{};
  std::int64_t last_event_ts_ns_{};
};

CanonicalEvent ParseCanonicalJson(const std::string& line);
std::vector<CanonicalEvent> LoadCanonicalJsonl(const std::filesystem::path& path);
std::string SideName(Side side);
std::string SnapshotJson(const BookSnapshot& snapshot);
std::string AnomalyJsonl(const std::vector<Anomaly>& anomalies);
std::string AuditBoundaryJsonl(const std::vector<AuditBoundary>& audits);

}  // namespace aegis_lob
