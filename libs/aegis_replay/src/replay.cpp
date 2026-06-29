#include "aegis_replay/replay.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace aegis_replay {

ReplayCursor::ReplayCursor(std::vector<aegis_lob::CanonicalEvent> events) : events_(std::move(events)) {}

ReplayCursor ReplayCursor::FromJsonl(const std::filesystem::path& path) {
  return ReplayCursor(aegis_lob::LoadCanonicalJsonl(path));
}

void ReplayCursor::reset() {
  book_.Clear();
  next_index_ = 0;
}

void ReplayCursor::seek_seq(std::uint64_t sequence_number) {
  reset();
  while (has_next() && events_[next_index_].sequence_number < sequence_number) {
    next_event();
  }
}

void ReplayCursor::seek_ts(std::int64_t event_ts_ns) {
  reset();
  while (has_next() && events_[next_index_].event_ts_ns < event_ts_ns) {
    next_event();
  }
}

bool ReplayCursor::has_next() const {
  return next_index_ < events_.size();
}

const aegis_lob::CanonicalEvent& ReplayCursor::next_event() {
  if (!has_next()) {
    throw std::out_of_range("replay cursor exhausted");
  }
  const auto& event = events_[next_index_++];
  book_.Apply(event);
  return event;
}

void ReplayCursor::replay_all() {
  while (has_next()) {
    next_event();
  }
}

aegis_lob::BookSnapshot ReplayCursor::snapshot(std::uint32_t depth) const {
  return book_.Snapshot(depth);
}

std::uint64_t ReplayCursor::state_checksum() const {
  return book_.StateChecksum();
}

void ReplayCursor::save_checkpoint(const std::filesystem::path& path) const {
  book_.SaveCheckpoint(path);
  std::ofstream out(path, std::ios::binary | std::ios::app);
  if (!out) {
    throw std::runtime_error("failed to append replay checkpoint cursor: " + path.string());
  }
  out << "CURSOR " << next_index_ << '\n';
}

void ReplayCursor::load_checkpoint(const std::filesystem::path& path) {
  book_.LoadCheckpoint(path);
  std::ifstream in(path, std::ios::binary);
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream line_in(line);
    std::string token;
    line_in >> token;
    if (token == "CURSOR") {
      line_in >> next_index_;
      if (next_index_ > events_.size()) {
        throw std::runtime_error("checkpoint cursor exceeds event log length");
      }
      return;
    }
  }
  const auto after = book_.last_sequence_number();
  next_index_ = static_cast<std::size_t>(std::distance(
      events_.begin(), std::find_if(events_.begin(), events_.end(), [after](const auto& event) {
        return event.sequence_number > after;
      })));
}

}  // namespace aegis_replay
