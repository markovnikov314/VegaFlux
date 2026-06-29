#include "aegis_lob/book.hpp"
#include "aegis_replay/replay.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

std::string Arg(int& index, int argc, char** argv) {
  if (index + 1 >= argc) {
    throw std::runtime_error(std::string("missing value for ") + argv[index]);
  }
  return argv[++index];
}

std::filesystem::path Resolve(const std::filesystem::path& root, const std::filesystem::path& path) {
  return path.is_absolute() ? path : root / path;
}

std::string Escape(const std::string& value) {
  std::string out;
  for (const char ch : value) {
    out += ch == '\\' ? "\\\\" : ch == '"' ? "\\\"" : std::string(1, ch);
  }
  return out;
}

void WriteText(const std::filesystem::path& path, const std::string& text) {
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw std::runtime_error("failed to write: " + path.string());
  }
  out << text;
}

std::string ChecksumJson(const std::filesystem::path& input, const aegis_replay::ReplayCursor& cursor) {
  std::string out = "{\n";
  out += "  \"schema_version\": \"vegaflux.canonical_market.v0.1\",\n";
  out += "  \"input\": \"" + Escape(input.string()) + "\",\n";
  out += "  \"events_replayed\": " + std::to_string(cursor.events_replayed()) + ",\n";
  out += "  \"state_checksum\": " + std::to_string(cursor.state_checksum()) + ",\n";
  out += "  \"audit_boundaries\": " + std::to_string(cursor.book().audit_boundaries().size()) + ",\n";
  out += "  \"recoverable_anomalies\": " + std::to_string(cursor.book().anomalies().size()) + ",\n";
  out += "  \"status\": \"pass\"\n";
  out += "}\n";
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    std::filesystem::path root = std::filesystem::current_path();
    std::filesystem::path input = "artifacts/decoder/decoded.normalized.jsonl";
    std::optional<std::filesystem::path> snapshot_output;
    std::optional<std::filesystem::path> checksum_output;
    std::optional<std::filesystem::path> anomalies_output;
    std::optional<std::filesystem::path> audit_output;
    std::optional<std::filesystem::path> checkpoint_output;
    std::optional<std::filesystem::path> restore_checkpoint;
    std::optional<std::uint64_t> seek_seq;
    std::uint32_t depth = 5;

    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--root") {
        root = Arg(i, argc, argv);
      } else if (arg == "--input") {
        input = Arg(i, argc, argv);
      } else if (arg == "--depth") {
        depth = static_cast<std::uint32_t>(std::stoul(Arg(i, argc, argv)));
      } else if (arg == "--snapshot-output") {
        snapshot_output = Arg(i, argc, argv);
      } else if (arg == "--checksum-output") {
        checksum_output = Arg(i, argc, argv);
      } else if (arg == "--anomalies-output") {
        anomalies_output = Arg(i, argc, argv);
      } else if (arg == "--audit-output") {
        audit_output = Arg(i, argc, argv);
      } else if (arg == "--checkpoint-output") {
        checkpoint_output = Arg(i, argc, argv);
      } else if (arg == "--restore-checkpoint") {
        restore_checkpoint = Arg(i, argc, argv);
      } else if (arg == "--seek-seq") {
        seek_seq = std::stoull(Arg(i, argc, argv));
      } else {
        throw std::runtime_error("unknown argument: " + arg);
      }
    }

    const auto input_path = Resolve(root, input);
    auto cursor = aegis_replay::ReplayCursor::FromJsonl(input_path);
    if (restore_checkpoint.has_value()) {
      cursor.load_checkpoint(Resolve(root, *restore_checkpoint));
    } else if (seek_seq.has_value()) {
      cursor.seek_seq(*seek_seq);
    }
    cursor.replay_all();

    const auto snapshot = aegis_lob::SnapshotJson(cursor.snapshot(depth));
    if (snapshot_output.has_value()) {
      WriteText(Resolve(root, *snapshot_output), snapshot);
    } else {
      std::cout << snapshot;
    }
    if (checksum_output.has_value()) {
      WriteText(Resolve(root, *checksum_output), ChecksumJson(input_path, cursor));
    }
    if (anomalies_output.has_value()) {
      WriteText(Resolve(root, *anomalies_output), aegis_lob::AnomalyJsonl(cursor.book().anomalies()));
    }
    if (audit_output.has_value()) {
      WriteText(Resolve(root, *audit_output), aegis_lob::AuditBoundaryJsonl(cursor.book().audit_boundaries()));
    }
    if (checkpoint_output.has_value()) {
      cursor.save_checkpoint(Resolve(root, *checkpoint_output));
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "aegis_replay_cli: " << error.what() << '\n';
    return 1;
  }
}
