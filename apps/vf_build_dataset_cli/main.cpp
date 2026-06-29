#include "vf_features/features.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

struct Manifest {
  std::filesystem::path input;
  std::optional<std::filesystem::path> fill_labels_input;
  std::filesystem::path features_output;
  std::filesystem::path metadata_output;
  std::string dataset_id{"synthetic_features_v1"};
  std::uint32_t depth{1};
  std::size_t label_horizon_events{2};
  std::size_t feature_window_events{3};
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

void SkipSpaces(const std::string& text, std::size_t& pos) {
  while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
    ++pos;
  }
}

std::optional<std::size_t> TryValuePos(const std::string& text, const std::string& key) {
  const auto needle = "\"" + key + "\"";
  const auto pos = text.find(needle);
  if (pos == std::string::npos) {
    return std::nullopt;
  }
  auto value_pos = pos + needle.size();
  SkipSpaces(text, value_pos);
  if (value_pos >= text.size() || text[value_pos] != ':') {
    throw std::runtime_error("missing manifest colon after key: " + key);
  }
  ++value_pos;
  SkipSpaces(text, value_pos);
  return value_pos;
}

std::size_t ValuePos(const std::string& text, const std::string& key) {
  const auto pos = TryValuePos(text, key);
  if (!pos.has_value()) {
    throw std::runtime_error("missing manifest key: " + key);
  }
  return *pos;
}

std::string StringField(const std::string& text, const std::string& key) {
  auto pos = ValuePos(text, key);
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
  const auto pos = TryValuePos(text, key);
  if (!pos.has_value()) {
    return std::nullopt;
  }
  if (text.compare(*pos, 4, "null") == 0) {
    return std::nullopt;
  }
  return StringField(text, key);
}

std::uint64_t UintField(const std::string& text, const std::string& key) {
  auto pos = ValuePos(text, key);
  const auto end = text.find_first_of(",}\r\n", pos);
  return std::stoull(text.substr(pos, end - pos));
}

std::uint64_t OptionalUintField(const std::string& text, const std::string& key, std::uint64_t fallback) {
  const auto pos = TryValuePos(text, key);
  if (!pos.has_value()) {
    return fallback;
  }
  const auto end = text.find_first_of(",}\r\n", *pos);
  return std::stoull(text.substr(*pos, end - *pos));
}

Manifest ReadManifest(const std::filesystem::path& path) {
  const auto text = ReadFile(path);
  Manifest manifest;
  manifest.input = StringField(text, "input");
  if (const auto fill_labels = OptionalStringField(text, "fill_labels_input")) {
    manifest.fill_labels_input = *fill_labels;
  }
  manifest.features_output = StringField(text, "features_output");
  manifest.metadata_output = StringField(text, "metadata_output");
  manifest.dataset_id = StringField(text, "dataset_id");
  manifest.depth = static_cast<std::uint32_t>(UintField(text, "depth"));
  manifest.label_horizon_events = static_cast<std::size_t>(UintField(text, "label_horizon_events"));
  manifest.feature_window_events = static_cast<std::size_t>(OptionalUintField(text, "feature_window_events", 3));
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
    std::optional<std::filesystem::path> features_override;
    std::optional<std::filesystem::path> metadata_override;

    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--root") {
        root = Arg(i, argc, argv);
      } else if (arg == "--manifest") {
        manifest_path = Arg(i, argc, argv);
      } else if (arg == "--features-output") {
        features_override = Arg(i, argc, argv);
      } else if (arg == "--metadata-output") {
        metadata_override = Arg(i, argc, argv);
      } else {
        throw std::runtime_error("unknown argument: " + arg);
      }
    }
    if (!manifest_path.has_value()) {
      throw std::runtime_error("--manifest is required");
    }

    auto manifest = ReadManifest(Resolve(root, *manifest_path));
    if (features_override.has_value()) {
      manifest.features_output = *features_override;
    }
    if (metadata_override.has_value()) {
      manifest.metadata_output = *metadata_override;
    }

    vf_features::FeatureConfig config;
    config.input_path = Resolve(root, manifest.input);
    if (manifest.fill_labels_input.has_value()) {
      config.fill_labels_path = Resolve(root, *manifest.fill_labels_input);
    }
    config.depth = manifest.depth;
    config.label_horizon_events = manifest.label_horizon_events;
    config.feature_window_events = manifest.feature_window_events;
    config.dataset_id = manifest.dataset_id;
    const auto dataset = vf_features::BuildDataset(config);
    const auto features_output = Resolve(root, manifest.features_output);
    const auto metadata_output = Resolve(root, manifest.metadata_output);
    vf_features::WriteFeaturesJsonl(features_output, dataset.rows);
    vf_features::WriteDatasetMetadata(metadata_output, dataset);

    std::cout << "{\"features_output\":\"" << Escape(features_output.string()) << "\",\"metadata_output\":\""
              << Escape(metadata_output.string()) << "\",\"rows\":" << dataset.rows.size()
              << ",\"status\":\"pass\"}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "vf_build_dataset_cli: " << error.what() << '\n';
    return 1;
  }
}
