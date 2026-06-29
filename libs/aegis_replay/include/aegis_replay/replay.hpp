#pragma once

#include "aegis_lob/book.hpp"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace aegis_replay {

class ReplayCursor {
 public:
  explicit ReplayCursor(std::vector<aegis_lob::CanonicalEvent> events);

  static ReplayCursor FromJsonl(const std::filesystem::path& path);

  void seek_seq(std::uint64_t sequence_number);
  void seek_ts(std::int64_t event_ts_ns);
  bool has_next() const;
  const aegis_lob::CanonicalEvent& next_event();
  void replay_all();

  aegis_lob::BookSnapshot snapshot(std::uint32_t depth) const;
  std::uint64_t state_checksum() const;
  void save_checkpoint(const std::filesystem::path& path) const;
  void load_checkpoint(const std::filesystem::path& path);

  const aegis_lob::Book& book() const { return book_; }
  std::size_t events_replayed() const { return next_index_; }
  std::size_t event_count() const { return events_.size(); }

 private:
  void reset();

  std::vector<aegis_lob::CanonicalEvent> events_;
  aegis_lob::Book book_;
  std::size_t next_index_{};
};

}  // namespace aegis_replay
