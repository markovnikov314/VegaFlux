#include "vf_options/options.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Require(bool ok, const std::string& message) {
  if (!ok) {
    throw std::runtime_error(message);
  }
}

void RequireNear(double actual, double expected, double tolerance, const std::string& message) {
  if (std::abs(actual - expected) > tolerance) {
    throw std::runtime_error(message + ": actual=" + std::to_string(actual) + " expected=" + std::to_string(expected));
  }
}

vf_options::OptionQuote Quote(const std::string& surface_id,
                              const std::string& quote_id,
                              double expiry,
                              double strike,
                              double bid,
                              double ask,
                              std::int64_t ts = 950) {
  return {surface_id, quote_id, "VFOPT", expiry, strike, true, bid, ask, ts};
}

vf_options::OptionQuote TypedQuote(const std::string& surface_id,
                                   const std::string& quote_id,
                                   double expiry,
                                   double strike,
                                   bool is_call,
                                   double bid,
                                   double ask,
                                   std::int64_t ts = 950) {
  return {surface_id, quote_id, "VFOPT", expiry, strike, is_call, bid, ask, ts};
}

vf_options::SurfaceConfig Config() {
  vf_options::SurfaceConfig config;
  config.evaluation_ts_ns = 1000;
  config.max_quote_age_ns = 100;
  config.assumptions.spot = 100.0;
  config.assumptions.rate = 0.05;
  config.assumptions.dividend_yield = 0.0;
  return config;
}

void CheckKnownBlackValues() {
  const double call = vf_options::BlackScholesPrice(100.0, 100.0, 1.0, 0.05, 0.0, 0.2, true);
  const double put = vf_options::BlackScholesPrice(100.0, 100.0, 1.0, 0.05, 0.0, 0.2, false);
  RequireNear(call, 10.450583572185565, 1e-10, "known call price mismatch");
  RequireNear(put, 5.573526022256971, 1e-10, "known put price mismatch");

  const double forward = 100.0 * std::exp(0.05);
  const double discount = std::exp(-0.05);
  RequireNear(vf_options::BlackPrice(forward, 100.0, discount, 1.0, 0.2, true), call, 1e-12,
              "Black and Black-Scholes call mismatch");

  const auto greeks = vf_options::BlackScholesGreeks(100.0, 100.0, 1.0, 0.05, 0.0, 0.2, true);
  RequireNear(greeks.delta, 0.6368306511756191, 1e-12, "known delta mismatch");
  RequireNear(greeks.gamma, 0.018762017345846895, 1e-12, "known gamma mismatch");
  RequireNear(greeks.vega, 37.52403469169379, 1e-10, "known vega mismatch");
  RequireNear(greeks.theta, -6.414027546438197, 1e-10, "known theta mismatch");
}

void CheckIvRoundTrip() {
  const double vol = 0.32;
  const double price = vf_options::BlackScholesPrice(101.0, 95.0, 0.75, 0.03, 0.01, vol, true);
  const auto iv = vf_options::ImpliedVol(price, 101.0, 95.0, 0.75, 0.03, 0.01, true, vf_options::IvConfig{});
  Require(iv.has_value(), "IV inversion failed");
  RequireNear(*iv, vol, 1e-9, "IV round-trip vol mismatch");
  RequireNear(vf_options::BlackScholesPrice(101.0, 95.0, 0.75, 0.03, 0.01, *iv, true), price, 1e-9,
              "IV round-trip price mismatch");
}

void CheckFiniteDifferenceGreeks() {
  constexpr double spot = 103.0;
  constexpr double strike = 100.0;
  constexpr double expiry = 0.9;
  constexpr double rate = 0.04;
  constexpr double dividend = 0.01;
  constexpr double vol = 0.27;
  for (const bool is_call : {true, false}) {
    const auto price = [is_call](double s, double t, double v) {
      return vf_options::BlackScholesPrice(s, strike, t, rate, dividend, v, is_call);
    };
    const auto greeks = vf_options::BlackScholesGreeks(spot, strike, expiry, rate, dividend, vol, is_call);
    const double ds = 1e-3;
    const double dv = 1e-4;
    const double dt = 1e-5;
    const double base = price(spot, expiry, vol);
    const double delta_fd = (price(spot + ds, expiry, vol) - price(spot - ds, expiry, vol)) / (2.0 * ds);
    const double gamma_fd = (price(spot + ds, expiry, vol) - 2.0 * base + price(spot - ds, expiry, vol)) / (ds * ds);
    const double vega_fd = (price(spot, expiry, vol + dv) - price(spot, expiry, vol - dv)) / (2.0 * dv);
    const double theta_fd = (price(spot, expiry - dt, vol) - price(spot, expiry + dt, vol)) / (2.0 * dt);
    RequireNear(greeks.delta, delta_fd, 1e-7, "finite-difference delta mismatch");
    RequireNear(greeks.gamma, gamma_fd, 1e-5, "finite-difference gamma mismatch");
    RequireNear(greeks.vega, vega_fd, 1e-6, "finite-difference vega mismatch");
    RequireNear(greeks.theta, theta_fd, 1e-6, "finite-difference theta mismatch");
  }
}

void CheckQuoteFiltering() {
  auto config = Config();
  const double good = vf_options::BlackScholesPrice(100.0, 100.0, 0.5, 0.05, 0.0, 0.2, true);
  const std::vector<vf_options::OptionQuote> quotes{
      Quote("clean_smile_v1", "accepted", 0.5, 100.0, good - 0.01, good + 0.01),
      Quote("clean_smile_v1", "crossed", 0.5, 100.0, 7.10, 7.00),
      Quote("clean_smile_v1", "stale", 0.5, 100.0, good - 0.01, good + 0.01, 800),
      Quote("clean_smile_v1", "non_positive", 0.5, 100.0, 0.0, 1.0),
      Quote("clean_smile_v1", "uninvertible", 0.5, 80.0, 0.40, 0.60),
  };
  const auto filtered = vf_options::FilterQuotes(quotes, config);
  std::map<std::string, std::string> statuses;
  for (const auto& quote : filtered) {
    statuses[quote.quote.quote_id] = quote.status;
  }
  Require(statuses["accepted"] == "ACCEPTED", "accepted quote rejected");
  Require(statuses["crossed"] == "CROSSED", "crossed quote status mismatch");
  Require(statuses["stale"] == "STALE", "stale quote status mismatch");
  Require(statuses["non_positive"] == "NON_POSITIVE", "non-positive quote status mismatch");
  Require(statuses["uninvertible"] == "UNINVERTIBLE", "uninvertible quote status mismatch");
}

void CheckHardRejectStatuses() {
  auto config = Config();
  const double good = vf_options::BlackScholesPrice(100.0, 100.0, 0.5, 0.05, 0.0, 0.2, true);
  const std::vector<vf_options::OptionQuote> quotes{
      Quote("clean_smile_v1", "bad_strike", 0.5, 0.0, good - 0.01, good + 0.01),
      Quote("clean_smile_v1", "bad_expiry", 0.0, 100.0, good - 0.01, good + 0.01),
      Quote("clean_smile_v1", "nan_bid", 0.5, 100.0, std::numeric_limits<double>::quiet_NaN(), good + 0.01),
      Quote("clean_smile_v1", "future", 0.5, 100.0, good - 0.01, good + 0.01, 1100),
  };
  const auto filtered = vf_options::FilterQuotes(quotes, config);
  std::map<std::string, std::string> statuses;
  for (const auto& quote : filtered) {
    statuses[quote.quote.quote_id] = quote.status;
  }
  Require(statuses["bad_strike"] == "INVALID_STRIKE", "invalid strike status mismatch");
  Require(statuses["bad_expiry"] == "INVALID_EXPIRY", "invalid expiry status mismatch");
  Require(statuses["nan_bid"] == "NAN_NUMERIC", "NaN numeric status mismatch");
  Require(statuses["future"] == "FUTURE_TIMESTAMP", "future timestamp status mismatch");

  config.assumptions.spot = 0.0;
  const auto invalid_underlying = vf_options::FilterQuotes({Quote("clean_smile_v1", "bad_spot", 0.5, 100.0,
                                                                 good - 0.01, good + 0.01)},
                                                           config);
  Require(invalid_underlying[0].status == "INVALID_UNDERLYING", "invalid underlying status mismatch");
}

void CheckSurfaceDiagnostics() {
  auto config = Config();
  const auto clean_price = [&](double strike, double vol) {
    return vf_options::BlackScholesPrice(100.0, strike, 0.5, 0.05, 0.0, vol, true);
  };
  const std::vector<vf_options::OptionQuote> quotes{
      Quote("clean_smile_v1", "clean_90", 0.5, 90.0, clean_price(90.0, 0.22) - 0.01,
            clean_price(90.0, 0.22) + 0.01),
      Quote("clean_smile_v1", "clean_100", 0.5, 100.0, clean_price(100.0, 0.20) - 0.01,
            clean_price(100.0, 0.20) + 0.01),
      Quote("clean_smile_v1", "clean_110", 0.5, 110.0, clean_price(110.0, 0.21) - 0.01,
            clean_price(110.0, 0.21) + 0.01),
      Quote("corrupt_smile_v1", "corrupt_90", 0.5, 90.0, 13.80, 13.90),
      Quote("corrupt_smile_v1", "corrupt_100", 0.5, 100.0, 14.95, 15.05),
      Quote("corrupt_smile_v1", "corrupt_110", 0.5, 110.0, 3.10, 3.20),
      TypedQuote("corrupt_put_smile_v1", "corrupt_put_90", 0.5, 90.0, false, 0.95, 1.05),
      TypedQuote("corrupt_put_smile_v1", "corrupt_put_100", 0.5, 100.0, false, 14.95, 15.05),
      TypedQuote("corrupt_put_smile_v1", "corrupt_put_110", 0.5, 110.0, false, 11.95, 12.05),
  };
  const auto filtered = vf_options::FilterQuotes(quotes, config);
  const auto clean = vf_options::FitSurface(filtered, config, "clean_smile_v1", "fnv64:test");
  const auto corrupt = vf_options::FitSurface(filtered, config, "corrupt_smile_v1", "fnv64:test");
  const auto corrupt_put = vf_options::FitSurface(filtered, config, "corrupt_put_smile_v1", "fnv64:test");
  Require(clean.status == "PASS", "clean surface did not pass");
  Require(clean.surface_model_id == "quadratic_total_variance_v1", "missing surface model id");
  Require(clean.quote_checksum.rfind("fnv64:", 0) == 0, "missing quote checksum");
  Require(clean.forward_assumptions.size() == 1, "missing forward assumption");
  RequireNear(clean.forward_assumptions[0].forward, 100.0 * std::exp(0.05 * 0.5), 1e-12,
              "forward assumption mismatch");
  Require(corrupt.status == "STATIC_ARBITRAGE_VIOLATION", "corrupt smile was not rejected");
  Require(corrupt.arbitrage.monotonic_violations > 0 || corrupt.arbitrage.butterfly_violations > 0,
          "corrupt smile had no static-arb diagnostics");
  Require(corrupt_put.status == "STATIC_ARBITRAGE_VIOLATION", "corrupt put smile was not rejected");
}

void CheckFixture(const std::filesystem::path& root) {
  auto config = Config();
  config.evaluation_ts_ns = 1'000'000'000;
  config.max_quote_age_ns = 200'000'000;
  const auto path = root / "data_contracts/fixtures/synthetic_options.csv";
  const auto quotes = vf_options::LoadOptionQuotes(path);
  const auto filtered = vf_options::FilterQuotes(quotes, config);
  const auto checksum = vf_options::Fnv64File(path);
  const auto clean = vf_options::FitSurface(filtered, config, "clean_smile_v1", checksum);
  const auto corrupt = vf_options::FitSurface(filtered, config, "corrupt_smile_v1", checksum);
  Require(clean.status == "PASS", "fixture clean surface failed");
  Require(corrupt.status == "STATIC_ARBITRAGE_VIOLATION", "fixture corrupt surface not caught");
}

void CheckVf5rFixture(const std::filesystem::path& root) {
  auto config = Config();
  config.component_id = "surface-validation";
  config.algorithm_version = "vegaflux_options_surface_validation_v1";
  config.evaluation_ts_ns = 1'000'000'000;
  config.max_quote_age_ns = 200'000'000;
  const auto path = root / "data_contracts/fixtures/synthetic_options_surface.csv";
  const auto loaded = vf_options::LoadOptionQuotesWithRejects(path);
  Require(loaded.rejected_rows.size() == 2, "surface-validation malformed rows were not captured");
  auto filtered = vf_options::FilterQuotes(loaded.quotes, config);
  filtered.insert(filtered.end(), loaded.rejected_rows.begin(), loaded.rejected_rows.end());
  std::map<std::string, int> counts;
  for (const auto& quote : filtered) {
    ++counts[quote.status];
  }
  Require(counts["ACCEPTED"] == 29, "surface-validation accepted quote count mismatch");
  Require(counts["MALFORMED_ROW"] == 2, "surface-validation malformed status count mismatch");
  Require(counts["UNINVERTIBLE"] == 2, "surface-validation uninvertible status count mismatch");
  Require(counts["FUTURE_TIMESTAMP"] == 1, "surface-validation future quote status missing");
  Require(counts["INVALID_STRIKE"] == 1, "surface-validation invalid strike status missing");
  Require(counts["INVALID_EXPIRY"] == 1, "surface-validation invalid expiry status missing");
  Require(counts["NAN_NUMERIC"] == 1, "surface-validation NaN numeric status missing");

  const auto checksum = vf_options::Fnv64File(path);
  auto clean = vf_options::FitSurface(filtered, config, "clean_smile_v1", checksum);
  const auto corrupt = vf_options::FitSurface(filtered, config, "corrupt_smile_v1", checksum);
  const auto corrupt_put = vf_options::FitSurface(filtered, config, "corrupt_put_smile_v1", checksum);
  Require(clean.status == "PASS", "surface-validation clean surface failed");
  Require(corrupt.status == "STATIC_ARBITRAGE_VIOLATION", "surface-validation corrupt call surface not caught");
  Require(corrupt_put.status == "STATIC_ARBITRAGE_VIOLATION", "surface-validation corrupt put surface not caught");

  vf_options::SurfaceInterpolationRequest request{"clean_smile_v1", 0.75, 105.0, true};
  const auto interpolation = vf_options::InterpolateSurfaceFairValue(clean, config, request);
  Require(interpolation.status == "PASS", "surface-validation interpolation failed");
  Require(interpolation.fair_value.has_value(), "surface-validation interpolation missing fair value");
  const auto unsafe_interpolation = vf_options::InterpolateSurfaceFairValue(corrupt, config, request);
  Require(unsafe_interpolation.status == "SURFACE_NOT_PASS", "unsafe surface interpolation did not fail closed");

  auto perturbed_quotes = loaded.quotes;
  for (auto& quote : perturbed_quotes) {
    if (quote.surface_id == "clean_smile_v1" && quote.bid > 0.0 && quote.ask > 0.0) {
      quote.bid += 0.001;
      quote.ask += 0.001;
    }
  }
  const auto perturbed_filtered = vf_options::FilterQuotes(perturbed_quotes, config);
  const auto perturbed = vf_options::FitSurface(perturbed_filtered, config, "clean_smile_v1", checksum);
  const auto perturbed_interpolation = vf_options::InterpolateSurfaceFairValue(perturbed, config, request);
  Require(perturbed.status == "PASS", "perturbed surface-validation surface failed");
  Require(perturbed_interpolation.fair_value.has_value(), "perturbed interpolation missing fair value");
  Require(std::abs(*perturbed_interpolation.fair_value - *interpolation.fair_value) < 0.05,
          "perturbed interpolation moved too far");
}

}  // namespace

int main(int argc, char** argv) {
  try {
    CheckKnownBlackValues();
    CheckIvRoundTrip();
    CheckFiniteDifferenceGreeks();
    CheckQuoteFiltering();
    CheckHardRejectStatuses();
    CheckSurfaceDiagnostics();
    if (argc > 1) {
      CheckFixture(argv[1]);
      CheckVf5rFixture(argv[1]);
    }
    std::cout << "options_tests pass\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
