#include "aegis_lob/book.hpp"
#include "aegis_replay/replay.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

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

std::string Fnv64File(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("failed to read dataset: " + path.string());
  }
  std::uint64_t hash = 14695981039346656037ull;
  for (const char ch : std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>())) {
    hash ^= static_cast<unsigned char>(ch);
    hash *= 1099511628211ull;
  }
  std::ostringstream out;
  out << "fnv64:" << std::hex << std::setw(16) << std::setfill('0') << hash;
  return out.str();
}

}  // namespace

int main(int argc, char** argv) {
  try {
    std::filesystem::path root = std::filesystem::current_path();
    std::filesystem::path input = "artifacts/decoder/decoded.normalized.jsonl";
    std::filesystem::path output;
    int iterations = 11;

    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--root") {
        root = Arg(i, argc, argv);
      } else if (arg == "--input") {
        input = Arg(i, argc, argv);
      } else if (arg == "--output") {
        output = Arg(i, argc, argv);
      } else if (arg == "--iterations") {
        iterations = std::stoi(Arg(i, argc, argv));
      } else {
        throw std::runtime_error("unknown argument: " + arg);
      }
    }
    if (output.empty()) {
      throw std::runtime_error("--output is required");
    }

    const auto input_path = Resolve(root, input);
    const auto output_path = Resolve(root, output);
    const auto events = aegis_lob::LoadCanonicalJsonl(input_path);
    std::vector<long long> durations;
    durations.reserve(static_cast<std::size_t>(iterations));
    std::uint64_t checksum = 0;
    for (int i = 0; i < iterations; ++i) {
      aegis_replay::ReplayCursor cursor(events);
      const auto start = std::chrono::steady_clock::now();
      cursor.replay_all();
      const auto end = std::chrono::steady_clock::now();
      checksum = cursor.state_checksum();
      durations.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    }
    std::sort(durations.begin(), durations.end());
    if (!output_path.parent_path().empty()) {
      std::filesystem::create_directories(output_path.parent_path());
    }
    std::ofstream out(output_path, std::ios::binary);
    out << "{\n";
    out << "  \"benchmark_id\": \"replay_smoke_cpp\",\n";
    out << "  \"dataset\": \"" << Escape(input_path.string()) << "\",\n";
    out << "  \"dataset_checksum\": \"" << Fnv64File(input_path) << "\",\n";
    out << "  \"events\": " << events.size() << ",\n";
    out << "  \"iterations\": " << iterations << ",\n";
    out << "  \"median_replay_ns\": " << durations[durations.size() / 2] << ",\n";
    out << "  \"min_replay_ns\": " << durations.front() << ",\n";
    out << "  \"note\": \"smoke wiring only; not a performance claim\",\n";
    out << "  \"schema_version\": \"vegaflux.canonical_market.v0.1\",\n";
    out << "  \"state_checksum\": " << checksum << ",\n";
    out << "  \"status\": \"pass\"\n";
    out << "}\n";
    std::cout << "{\"output\":\"" << Escape(output_path.string()) << "\",\"status\":\"pass\"}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
