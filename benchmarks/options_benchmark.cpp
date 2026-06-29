#include "vf_options/options.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
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

vf_options::SurfaceConfig Config() {
  vf_options::SurfaceConfig config;
  config.evaluation_ts_ns = 1'000'000'000;
  config.max_quote_age_ns = 200'000'000;
  config.assumptions.spot = 100.0;
  config.assumptions.rate = 0.05;
  config.assumptions.dividend_yield = 0.0;
  return config;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    std::filesystem::path root = std::filesystem::current_path();
    std::filesystem::path input = "data_contracts/fixtures/synthetic_options.csv";
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
    const auto config = Config();
    const auto input_checksum = vf_options::Fnv64File(input_path);
    const auto loaded = vf_options::LoadOptionQuotesWithRejects(input_path);

    std::vector<long long> durations;
    durations.reserve(static_cast<std::size_t>(iterations));
    vf_options::SurfaceFitResult clean;
    vf_options::SurfaceFitResult corrupt;
    for (int i = 0; i < iterations; ++i) {
      const auto start = std::chrono::steady_clock::now();
      auto filtered = vf_options::FilterQuotes(loaded.quotes, config);
      filtered.insert(filtered.end(), loaded.rejected_rows.begin(), loaded.rejected_rows.end());
      clean = vf_options::FitSurface(filtered, config, config.surface_id, input_checksum);
      corrupt = vf_options::FitSurface(filtered, config, config.corrupted_surface_id, input_checksum);
      const auto end = std::chrono::steady_clock::now();
      durations.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    }
    std::sort(durations.begin(), durations.end());

    if (!output_path.parent_path().empty()) {
      std::filesystem::create_directories(output_path.parent_path());
    }
    std::ofstream out(output_path, std::ios::binary);
    out << "{\n";
    out << "  \"benchmark_id\": \"options_smoke_cpp\",\n";
    out << "  \"dataset\": \"" << Escape(input_path.string()) << "\",\n";
    out << "  \"dataset_checksum\": \"" << input_checksum << "\",\n";
    out << "  \"iterations\": " << iterations << ",\n";
    out << "  \"malformed_rows\": " << loaded.rejected_rows.size() << ",\n";
    out << "  \"median_diagnostics_ns\": " << durations[durations.size() / 2] << ",\n";
    out << "  \"min_diagnostics_ns\": " << durations.front() << ",\n";
    out << "  \"note\": \"smoke wiring only; not a performance claim\",\n";
    out << "  \"surface_model_id\": \"" << Escape(config.surface_model_id) << "\",\n";
    out << "  \"clean_status\": \"" << Escape(clean.status) << "\",\n";
    out << "  \"corrupt_status\": \"" << Escape(corrupt.status) << "\",\n";
    out << "  \"schema_version\": \"vegaflux.canonical_market.v0.1\",\n";
    out << "  \"status\": \"" << (clean.status == "PASS" && corrupt.status == "STATIC_ARBITRAGE_VIOLATION" ? "pass"
                                                                                                           : "fail")
        << "\"\n";
    out << "}\n";
    std::cout << "{\"output\":\"" << Escape(output_path.string()) << "\",\"status\":\"pass\"}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
