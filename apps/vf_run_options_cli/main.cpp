#include "vf_options/options.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>

namespace {

struct Manifest {
  std::filesystem::path input;
  std::filesystem::path filtered_quotes_output;
  std::filesystem::path diagnostics_output;
  std::optional<std::filesystem::path> source_table_output;
  std::optional<std::filesystem::path> iv_residuals_output;
  std::optional<std::filesystem::path> static_arbitrage_output;
  std::optional<std::filesystem::path> greek_fd_output;
  std::optional<vf_options::SurfaceInterpolationRequest> interpolation_request;
  vf_options::SurfaceConfig config;
};

std::string Arg(int& index, int argc, char** argv) {
  if (index + 1 >= argc) {
    throw std::runtime_error(std::string("missing value for ") + argv[index]);
  }
  return argv[++index];
}

std::filesystem::path Resolve(const std::filesystem::path& root, const std::filesystem::path& path) {
  return path.is_absolute() ? path : root / path;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("failed to read manifest: " + path.string());
  }
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::optional<std::size_t> FindValuePos(const std::string& text, const std::string& key) {
  const auto needle = "\"" + key + "\":";
  const auto pos = text.find(needle);
  if (pos == std::string::npos) {
    return std::nullopt;
  }
  return pos + needle.size();
}

std::size_t ValuePos(const std::string& text, const std::string& key) {
  auto pos = FindValuePos(text, key);
  if (!pos.has_value()) {
    throw std::runtime_error("missing manifest key: " + key);
  }
  return *pos;
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
    throw std::runtime_error("expected string manifest field: " + key);
  }
  ++pos;
  std::string out;
  for (; pos < text.size(); ++pos) {
    if (text[pos] == '"') {
      return out;
    }
    out.push_back(text[pos]);
  }
  throw std::runtime_error("unterminated manifest string: " + key);
}

std::optional<std::string> OptionalStringField(const std::string& text, const std::string& key) {
  auto pos = FindValuePos(text, key);
  if (!pos.has_value()) {
    return std::nullopt;
  }
  auto value_pos = *pos;
  SkipSpaces(text, value_pos);
  if (value_pos >= text.size() || text[value_pos] != '"') {
    throw std::runtime_error("expected string manifest field: " + key);
  }
  ++value_pos;
  std::string out;
  for (; value_pos < text.size(); ++value_pos) {
    if (text[value_pos] == '"') {
      return out;
    }
    out.push_back(text[value_pos]);
  }
  throw std::runtime_error("unterminated manifest string: " + key);
}

double DoubleField(const std::string& text, const std::string& key) {
  auto pos = ValuePos(text, key);
  SkipSpaces(text, pos);
  const auto end = text.find_first_of(",}\r\n", pos);
  return std::stod(text.substr(pos, end - pos));
}

std::optional<double> OptionalDoubleField(const std::string& text, const std::string& key) {
  auto pos = FindValuePos(text, key);
  if (!pos.has_value()) {
    return std::nullopt;
  }
  auto value_pos = *pos;
  SkipSpaces(text, value_pos);
  const auto end = text.find_first_of(",}\r\n", value_pos);
  return std::stod(text.substr(value_pos, end - value_pos));
}

std::int64_t IntField(const std::string& text, const std::string& key) {
  auto pos = ValuePos(text, key);
  SkipSpaces(text, pos);
  const auto end = text.find_first_of(",}\r\n", pos);
  return std::stoll(text.substr(pos, end - pos));
}

std::optional<bool> OptionalBoolField(const std::string& text, const std::string& key) {
  auto pos = FindValuePos(text, key);
  if (!pos.has_value()) {
    return std::nullopt;
  }
  auto value_pos = *pos;
  SkipSpaces(text, value_pos);
  if (text.compare(value_pos, 4, "true") == 0 || text.compare(value_pos, 1, "1") == 0) {
    return true;
  }
  if (text.compare(value_pos, 5, "false") == 0 || text.compare(value_pos, 1, "0") == 0) {
    return false;
  }
  throw std::runtime_error("expected bool manifest field: " + key);
}

Manifest ReadManifest(const std::filesystem::path& path) {
  const auto text = ReadFile(path);
  Manifest manifest;
  manifest.input = StringField(text, "input");
  manifest.filtered_quotes_output = StringField(text, "filtered_quotes_output");
  manifest.diagnostics_output = StringField(text, "diagnostics_output");
  manifest.config.component_id = StringField(text, "component_id");
  manifest.config.surface_id = StringField(text, "surface_id");
  manifest.config.corrupted_surface_id = StringField(text, "corrupted_surface_id");
  manifest.config.surface_model_id = StringField(text, "surface_model_id");
  manifest.config.algorithm_version = StringField(text, "algorithm_version");
  manifest.config.evaluation_ts_ns = IntField(text, "evaluation_ts_ns");
  manifest.config.max_quote_age_ns = IntField(text, "max_quote_age_ns");
  manifest.config.assumptions.spot = DoubleField(text, "spot");
  manifest.config.assumptions.rate = DoubleField(text, "rate");
  manifest.config.assumptions.dividend_yield = DoubleField(text, "dividend_yield");
  manifest.config.iv.min_vol = DoubleField(text, "min_iv");
  manifest.config.iv.max_vol = DoubleField(text, "max_iv");
  if (auto value = OptionalStringField(text, "source_table_output")) {
    manifest.source_table_output = *value;
  }
  if (auto value = OptionalStringField(text, "iv_residuals_output")) {
    manifest.iv_residuals_output = *value;
  }
  if (auto value = OptionalStringField(text, "static_arbitrage_output")) {
    manifest.static_arbitrage_output = *value;
  }
  if (auto value = OptionalStringField(text, "greek_fd_output")) {
    manifest.greek_fd_output = *value;
  }
  const auto selected_expiry = OptionalDoubleField(text, "selected_expiry_years");
  const auto selected_strike = OptionalDoubleField(text, "selected_strike");
  if (selected_expiry.has_value() || selected_strike.has_value()) {
    if (!selected_expiry.has_value() || !selected_strike.has_value()) {
      throw std::runtime_error("selected_expiry_years and selected_strike must be configured together");
    }
    vf_options::SurfaceInterpolationRequest request;
    request.surface_id = manifest.config.surface_id;
    request.expiry_years = *selected_expiry;
    request.strike = *selected_strike;
    request.is_call = OptionalBoolField(text, "selected_is_call").value_or(true);
    manifest.interpolation_request = request;
  }
  return manifest;
}

std::string Escape(const std::string& value) {
  std::string out;
  for (const char ch : value) {
    out += ch == '\\' ? "\\\\" : ch == '"' ? "\\\"" : std::string(1, ch);
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    std::filesystem::path root = std::filesystem::current_path();
    std::optional<std::filesystem::path> manifest_path;
    std::optional<std::filesystem::path> filtered_override;
    std::optional<std::filesystem::path> diagnostics_override;

    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--root") {
        root = Arg(i, argc, argv);
      } else if (arg == "--manifest") {
        manifest_path = Arg(i, argc, argv);
      } else if (arg == "--filtered-quotes-output") {
        filtered_override = Arg(i, argc, argv);
      } else if (arg == "--diagnostics-output") {
        diagnostics_override = Arg(i, argc, argv);
      } else {
        throw std::runtime_error("unknown argument: " + arg);
      }
    }
    if (!manifest_path.has_value()) {
      throw std::runtime_error("--manifest is required");
    }

    auto manifest = ReadManifest(Resolve(root, *manifest_path));
    if (filtered_override.has_value()) {
      manifest.filtered_quotes_output = *filtered_override;
    }
    if (diagnostics_override.has_value()) {
      manifest.diagnostics_output = *diagnostics_override;
    }

    const auto input = Resolve(root, manifest.input);
    const auto filtered_output = Resolve(root, manifest.filtered_quotes_output);
    const auto diagnostics_output = Resolve(root, manifest.diagnostics_output);
    const auto input_checksum = vf_options::Fnv64File(input);
    const auto loaded = vf_options::LoadOptionQuotesWithRejects(input);
    auto filtered = vf_options::FilterQuotes(loaded.quotes, manifest.config);
    filtered.insert(filtered.end(), loaded.rejected_rows.begin(), loaded.rejected_rows.end());
    std::sort(filtered.begin(), filtered.end(), [](const auto& left, const auto& right) {
      return left.source_line < right.source_line;
    });
    auto clean = vf_options::FitSurface(filtered, manifest.config, manifest.config.surface_id, input_checksum);
    auto corrupt =
        vf_options::FitSurface(filtered, manifest.config, manifest.config.corrupted_surface_id, input_checksum);
    if (manifest.interpolation_request.has_value()) {
      clean.interpolation =
          vf_options::InterpolateSurfaceFairValue(clean, manifest.config, *manifest.interpolation_request);
      auto corrupt_request = *manifest.interpolation_request;
      corrupt_request.surface_id = manifest.config.corrupted_surface_id;
      corrupt.interpolation = vf_options::InterpolateSurfaceFairValue(corrupt, manifest.config, corrupt_request);
    }

    vf_options::WriteFilteredQuotesJsonl(filtered_output, filtered);
    vf_options::WriteSurfaceDiagnostics(diagnostics_output, manifest.config, input, input_checksum, filtered, clean,
                                        corrupt);
    if (manifest.source_table_output.has_value()) {
      vf_options::WriteSourceTableCsv(Resolve(root, *manifest.source_table_output), filtered);
    }
    if (manifest.iv_residuals_output.has_value()) {
      vf_options::WriteIvResidualTableCsv(Resolve(root, *manifest.iv_residuals_output), filtered, clean,
                                          manifest.config);
    }
    if (manifest.static_arbitrage_output.has_value()) {
      std::vector<vf_options::SurfaceFitResult> static_fits{clean, corrupt};
      std::set<std::string> surface_ids{clean.surface_id, corrupt.surface_id};
      for (const auto& quote : filtered) {
        if (quote.quote.surface_id.empty() || quote.status != "ACCEPTED" ||
            surface_ids.count(quote.quote.surface_id) != 0) {
          continue;
        }
        surface_ids.insert(quote.quote.surface_id);
        static_fits.push_back(
            vf_options::FitSurface(filtered, manifest.config, quote.quote.surface_id, input_checksum));
      }
      vf_options::WriteStaticArbitrageTableCsv(Resolve(root, *manifest.static_arbitrage_output), static_fits);
    }
    if (manifest.greek_fd_output.has_value()) {
      const double expiry = manifest.interpolation_request.has_value() ? manifest.interpolation_request->expiry_years
                                                                       : 1.0;
      const double strike = manifest.interpolation_request.has_value() ? manifest.interpolation_request->strike : 100.0;
      vf_options::WriteGreekFiniteDifferenceTableCsv(Resolve(root, *manifest.greek_fd_output),
                                                     manifest.config.assumptions, expiry, strike, 0.2);
    }

    const bool pass = clean.status == "PASS" && corrupt.status == "STATIC_ARBITRAGE_VIOLATION";
    std::cout << "{\"diagnostics_output\":\"" << Escape(diagnostics_output.string())
              << "\",\"filtered_quotes_output\":\"" << Escape(filtered_output.string()) << "\",\"status\":\""
              << (pass ? "pass" : "fail") << "\"}\n";
    return pass ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << "vf_run_options_cli: " << error.what() << '\n';
    return 1;
  }
}
