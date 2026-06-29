#include "aegis_decode/synthetic_itch.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

std::atomic<unsigned long long> g_decoder_allocations{0};

void* operator new(std::size_t size) {
  g_decoder_allocations.fetch_add(1, std::memory_order_relaxed);
  if (void* ptr = std::malloc(size)) {
    return ptr;
  }
  throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
  return operator new(size);
}

void operator delete(void* ptr) noexcept {
  std::free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept {
  std::free(ptr);
}

void operator delete[](void* ptr) noexcept {
  std::free(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept {
  std::free(ptr);
}

namespace {

std::vector<std::uint8_t> ReadBytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("failed to read " + path.string());
  }
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string Arg(int& index, int argc, char** argv) {
  if (index + 1 >= argc) {
    throw std::runtime_error(std::string("missing value for ") + argv[index]);
  }
  return argv[++index];
}

std::filesystem::path Resolve(const std::filesystem::path& root, const std::filesystem::path& path) {
  return path.is_absolute() ? path : root / path;
}

std::string JsonEscape(const std::string& value) {
  std::string out;
  constexpr char kHex[] = "0123456789abcdef";
  for (const char ch : value) {
    const auto byte = static_cast<unsigned char>(ch);
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (byte < 0x20) {
          out += "\\u00";
          out.push_back(kHex[(byte >> 4U) & 0x0fU]);
          out.push_back(kHex[byte & 0x0fU]);
        } else {
          out.push_back(ch);
        }
    }
  }
  return out;
}

std::size_t PercentileIndex(std::size_t size, int percentile) {
  return std::min(size - 1U, ((size * static_cast<std::size_t>(percentile)) + 99U) / 100U - 1U);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    std::filesystem::path root = std::filesystem::current_path();
    std::filesystem::path input = "data_contracts/fixtures/synthetic_itch.bin";
    std::filesystem::path output;
    int iterations = 7;

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
    if (iterations < 1) {
      throw std::runtime_error("--iterations must be positive");
    }
    if (output.empty()) {
      throw std::runtime_error("--output is required");
    }

    const auto input_path = Resolve(root, input);
    const auto output_path = Resolve(root, output);
    const auto bytes = ReadBytes(input_path);
    std::vector<long long> durations;
    durations.reserve(static_cast<std::size_t>(iterations));
    std::vector<unsigned long long> allocations;
    allocations.reserve(static_cast<std::size_t>(iterations));
    std::size_t events = 0;
    std::size_t quarantined = 0;
    for (int i = 0; i < iterations; ++i) {
      g_decoder_allocations.store(0, std::memory_order_relaxed);
      const auto start = std::chrono::steady_clock::now();
      const auto decoded = aegis_decode::DecodeBytes(bytes);
      const auto end = std::chrono::steady_clock::now();
      durations.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
      allocations.push_back(g_decoder_allocations.load(std::memory_order_relaxed));
      events = decoded.events.size();
      quarantined = decoded.quarantine.size();
    }
    std::sort(durations.begin(), durations.end());
    std::sort(allocations.begin(), allocations.end());
    std::filesystem::create_directories(output_path.parent_path());
    std::ofstream out(output_path, std::ios::binary);
    out << "{\n";
    out << "  \"benchmark_id\": \"decoder_smoke_cpp\",\n";
    out << "  \"dataset\": \"" << JsonEscape(input_path.string()) << "\",\n";
    out << "  \"dataset_checksum\": \"21fc4b10e96b2cea19d04edc7fad6fce94befdeb2dc411ac379a520438ac9cf0\",\n";
    out << "  \"events\": " << events << ",\n";
    out << "  \"iterations\": " << iterations << ",\n";
    out << "  \"allocation_count_note\": \"global operator new count during DecodeBytes; smoke only\",\n";
    out << "  \"allocation_count_p50\": " << allocations[PercentileIndex(allocations.size(), 50)] << ",\n";
    out << "  \"allocation_count_p99\": " << allocations[PercentileIndex(allocations.size(), 99)] << ",\n";
    out << "  \"median_decode_ns\": " << durations[durations.size() / 2] << ",\n";
    out << "  \"min_decode_ns\": " << durations.front() << ",\n";
    out << "  \"note\": \"smoke wiring only; not a performance claim\",\n";
    out << "  \"p50_decode_ns\": " << durations[PercentileIndex(durations.size(), 50)] << ",\n";
    out << "  \"p99_decode_ns\": " << durations[PercentileIndex(durations.size(), 99)] << ",\n";
    out << "  \"quarantined\": " << quarantined << ",\n";
    out << "  \"schema_version\": \"vegaflux.canonical_market.v0.1\",\n";
    out << "  \"status\": \"pass\"\n";
    out << "}\n";
    std::cout << "{\"output\":\"" << JsonEscape(output_path.string()) << "\",\"status\":\"pass\"}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
