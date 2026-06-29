#include "vf_options/options.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace vf_options {
namespace {

constexpr double kSqrt2 = 1.4142135623730950488;
constexpr double kInvSqrt2Pi = 0.39894228040143267794;

std::string Escape(const std::string& value) {
  std::string out;
  for (const char ch : value) {
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
        out.push_back(ch);
    }
  }
  return out;
}

std::string JsonString(const std::string& value) {
  return "\"" + Escape(value) + "\"";
}

std::string CsvString(const std::string& value) {
  return "\"" + Escape(value) + "\"";
}

std::string JsonDouble(double value) {
  if (!std::isfinite(value)) {
    return "null";
  }
  std::ostringstream out;
  out << std::setprecision(15) << value;
  return out.str();
}

std::string JsonOptionalDouble(std::optional<double> value) {
  return value.has_value() ? JsonDouble(*value) : "null";
}

void AddJsonField(std::ostringstream& out, const std::string& name, const std::string& value, bool& first) {
  if (!first) {
    out << ',';
  }
  first = false;
  out << '"' << name << "\":" << value;
}

std::vector<std::string> SplitCsv(const std::string& line) {
  std::vector<std::string> out;
  std::string field;
  std::istringstream in(line);
  while (std::getline(in, field, ',')) {
    out.push_back(field);
  }
  return out;
}

bool ParseBool(const std::string& value) {
  return value == "1" || value == "true" || value == "TRUE" || value == "CALL" || value == "call";
}

std::optional<bool> ParseBoolStrict(const std::string& value) {
  if (value == "1" || value == "true" || value == "TRUE" || value == "CALL" || value == "call") {
    return true;
  }
  if (value == "0" || value == "false" || value == "FALSE" || value == "PUT" || value == "put") {
    return false;
  }
  return std::nullopt;
}

std::optional<double> ParseDouble(const std::string& value) {
  try {
    std::size_t used = 0;
    const double parsed = std::stod(value, &used);
    if (used != value.size()) {
      return std::nullopt;
    }
    return parsed;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::optional<std::int64_t> ParseInt64(const std::string& value) {
  try {
    std::size_t used = 0;
    const auto parsed = std::stoll(value, &used);
    if (used != value.size()) {
      return std::nullopt;
    }
    return parsed;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

FilteredQuote RejectedCsvRow(int source_line,
                             const std::string& raw_csv,
                             const std::vector<std::string>& fields,
                             const std::string& reason) {
  FilteredQuote row;
  if (!fields.empty()) {
    row.quote.surface_id = fields[0];
  }
  row.quote.quote_id = fields.size() > 1 ? fields[1] : "line_" + std::to_string(source_line);
  if (fields.size() > 2) {
    row.quote.symbol = fields[2];
  }
  row.status = "MALFORMED_ROW";
  row.reason = reason;
  row.mid = std::numeric_limits<double>::quiet_NaN();
  row.source_line = source_line;
  row.raw_csv = raw_csv;
  row.quote.source_line = source_line;
  row.quote.raw_csv = raw_csv;
  return row;
}

double Forward(const MarketAssumptions& assumptions, double expiry_years) {
  return assumptions.spot * std::exp((assumptions.rate - assumptions.dividend_yield) * expiry_years);
}

double Discount(const MarketAssumptions& assumptions, double expiry_years) {
  return std::exp(-assumptions.rate * expiry_years);
}

std::uint64_t Fnv64Bytes(const std::string& text, std::uint64_t hash = 14695981039346656037ull) {
  for (const unsigned char ch : text) {
    hash ^= ch;
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string Fnv64Hex(std::uint64_t hash) {
  std::ostringstream out;
  out << "fnv64:" << std::hex << std::setw(16) << std::setfill('0') << hash;
  return out.str();
}

std::string QuoteChecksum(const std::vector<FilteredQuote>& quotes, const std::string& surface_id) {
  std::uint64_t hash = 14695981039346656037ull;
  for (const auto& quote : quotes) {
    if (quote.quote.surface_id != surface_id || quote.status != "ACCEPTED") {
      continue;
    }
    std::ostringstream row;
    row << quote.quote.quote_id << '|' << quote.quote.expiry_years << '|' << quote.quote.strike << '|'
        << quote.quote.is_call << '|' << quote.mid << '|' << *quote.implied_vol << '\n';
    hash = Fnv64Bytes(row.str(), hash);
  }
  return Fnv64Hex(hash);
}

StaticArbitrageDiagnostics CheckStaticArbitrage(const std::vector<const FilteredQuote*>& quotes) {
  struct Point {
    double expiry{};
    double strike{};
    double price{};
    bool is_call{};
  };
  std::vector<Point> points;
  points.reserve(quotes.size());
  for (const auto* quote : quotes) {
    points.push_back({quote->quote.expiry_years, quote->quote.strike, quote->mid, quote->quote.is_call});
  }

  StaticArbitrageDiagnostics diagnostics;
  std::sort(points.begin(), points.end(), [](const Point& left, const Point& right) {
    if (left.is_call != right.is_call) {
      return left.is_call > right.is_call;
    }
    if (left.expiry != right.expiry) {
      return left.expiry < right.expiry;
    }
    return left.strike < right.strike;
  });

  constexpr double tol = 1e-8;
  for (std::size_t begin = 0; begin < points.size();) {
    std::size_t end = begin + 1;
    while (end < points.size() && points[end].is_call == points[begin].is_call &&
           std::abs(points[end].expiry - points[begin].expiry) < 1e-12) {
      ++end;
    }
    for (std::size_t i = begin + 1; i < end; ++i) {
      const auto& prev = points[i - 1];
      const auto& cur = points[i];
      if (cur.is_call && cur.price > prev.price + tol) {
        ++diagnostics.monotonic_violations;
      }
      if (!cur.is_call && cur.price + tol < prev.price) {
        ++diagnostics.monotonic_violations;
      }
    }
    for (std::size_t i = begin + 2; i < end; ++i) {
      const auto& left = points[i - 2];
      const auto& mid = points[i - 1];
      const auto& right = points[i];
      const double left_slope = (mid.price - left.price) / (mid.strike - left.strike);
      const double right_slope = (right.price - mid.price) / (right.strike - mid.strike);
      if (right_slope + tol < left_slope) {
        ++diagnostics.butterfly_violations;
      }
    }
    begin = end;
  }

  std::sort(points.begin(), points.end(), [](const Point& left, const Point& right) {
    if (left.is_call != right.is_call) {
      return left.is_call > right.is_call;
    }
    if (left.strike != right.strike) {
      return left.strike < right.strike;
    }
    return left.expiry < right.expiry;
  });
  for (std::size_t i = 1; i < points.size(); ++i) {
    const auto& prev = points[i - 1];
    const auto& cur = points[i];
    if (cur.is_call == prev.is_call && std::abs(cur.strike - prev.strike) < 1e-12 &&
        cur.expiry > prev.expiry + 1e-12 && cur.price + tol < prev.price) {
      ++diagnostics.calendar_violations;
    }
  }
  return diagnostics;
}

bool HasArbitrage(const StaticArbitrageDiagnostics& diagnostics) {
  return diagnostics.monotonic_violations > 0 || diagnostics.butterfly_violations > 0 ||
         diagnostics.calendar_violations > 0;
}

bool Solve3x3(std::array<std::array<double, 4>, 3> matrix, std::array<double, 3>& solution) {
  for (int col = 0; col < 3; ++col) {
    int pivot = col;
    for (int row = col + 1; row < 3; ++row) {
      if (std::abs(matrix[row][col]) > std::abs(matrix[pivot][col])) {
        pivot = row;
      }
    }
    if (std::abs(matrix[pivot][col]) < 1e-14) {
      return false;
    }
    if (pivot != col) {
      std::swap(matrix[pivot], matrix[col]);
    }
    for (int row = 0; row < 3; ++row) {
      if (row == col) {
        continue;
      }
      const double scale = matrix[row][col] / matrix[col][col];
      for (int item = col; item < 4; ++item) {
        matrix[row][item] -= scale * matrix[col][item];
      }
    }
  }
  for (int row = 0; row < 3; ++row) {
    solution[row] = matrix[row][3] / matrix[row][row];
  }
  return true;
}

std::vector<const FilteredQuote*> AcceptedSurfaceQuotes(const std::vector<FilteredQuote>& quotes,
                                                        const std::string& surface_id) {
  std::vector<const FilteredQuote*> accepted;
  for (const auto& quote : quotes) {
    if (quote.quote.surface_id == surface_id && quote.status == "ACCEPTED" && quote.implied_vol.has_value()) {
      accepted.push_back(&quote);
    }
  }
  return accepted;
}

std::string FitReason(const StaticArbitrageDiagnostics& diagnostics) {
  std::ostringstream out;
  out << "monotonic=" << diagnostics.monotonic_violations << ",butterfly=" << diagnostics.butterfly_violations
      << ",calendar=" << diagnostics.calendar_violations;
  return out.str();
}

}  // namespace

double NormalCdf(double x) {
  return 0.5 * std::erfc(-x / kSqrt2);
}

double NormalPdf(double x) {
  return kInvSqrt2Pi * std::exp(-0.5 * x * x);
}

double BlackPrice(double forward, double strike, double discount, double expiry_years, double volatility, bool is_call) {
  if (!std::isfinite(forward) || !std::isfinite(strike) || !std::isfinite(discount) || forward <= 0.0 ||
      strike <= 0.0 || discount <= 0.0) {
    throw std::runtime_error("Black inputs must be positive");
  }
  if (expiry_years <= 0.0 || volatility <= 0.0) {
    const double intrinsic = is_call ? std::max(forward - strike, 0.0) : std::max(strike - forward, 0.0);
    return discount * intrinsic;
  }
  const double stddev = volatility * std::sqrt(expiry_years);
  const double d1 = (std::log(forward / strike) + 0.5 * stddev * stddev) / stddev;
  const double d2 = d1 - stddev;
  if (is_call) {
    return discount * (forward * NormalCdf(d1) - strike * NormalCdf(d2));
  }
  return discount * (strike * NormalCdf(-d2) - forward * NormalCdf(-d1));
}

double BlackScholesPrice(double spot,
                         double strike,
                         double expiry_years,
                         double rate,
                         double dividend_yield,
                         double volatility,
                         bool is_call) {
  MarketAssumptions assumptions{spot, rate, dividend_yield};
  return BlackPrice(Forward(assumptions, expiry_years), strike, Discount(assumptions, expiry_years), expiry_years,
                    volatility, is_call);
}

std::optional<double> ImpliedVol(double target_price,
                                 double spot,
                                 double strike,
                                 double expiry_years,
                                 double rate,
                                 double dividend_yield,
                                 bool is_call,
                                 const IvConfig& config) {
  if (!std::isfinite(target_price) || target_price <= 0.0 || config.min_vol <= 0.0 ||
      config.max_vol <= config.min_vol || config.max_iterations <= 0 || !std::isfinite(spot) ||
      !std::isfinite(strike) || !std::isfinite(expiry_years) || !std::isfinite(rate) ||
      !std::isfinite(dividend_yield) || spot <= 0.0 || strike <= 0.0 || expiry_years <= 0.0) {
    return std::nullopt;
  }
  const double low_price =
      BlackScholesPrice(spot, strike, expiry_years, rate, dividend_yield, config.min_vol, is_call);
  const double high_price =
      BlackScholesPrice(spot, strike, expiry_years, rate, dividend_yield, config.max_vol, is_call);
  if (target_price < low_price - config.price_tolerance || target_price > high_price + config.price_tolerance) {
    return std::nullopt;
  }
  if (std::abs(target_price - low_price) <= config.price_tolerance) {
    return config.min_vol;
  }
  if (std::abs(target_price - high_price) <= config.price_tolerance) {
    return config.max_vol;
  }

  double low = config.min_vol;
  double high = config.max_vol;
  for (int i = 0; i < config.max_iterations; ++i) {
    const double mid = 0.5 * (low + high);
    const double price = BlackScholesPrice(spot, strike, expiry_years, rate, dividend_yield, mid, is_call);
    if (std::abs(price - target_price) <= config.price_tolerance) {
      return mid;
    }
    if (price < target_price) {
      low = mid;
    } else {
      high = mid;
    }
  }
  return 0.5 * (low + high);
}

GreekResult BlackScholesGreeks(double spot,
                               double strike,
                               double expiry_years,
                               double rate,
                               double dividend_yield,
                               double volatility,
                               bool is_call) {
  if (!std::isfinite(spot) || !std::isfinite(strike) || !std::isfinite(expiry_years) ||
      !std::isfinite(rate) || !std::isfinite(dividend_yield) || !std::isfinite(volatility) || spot <= 0.0 ||
      strike <= 0.0 || expiry_years <= 0.0 || volatility <= 0.0) {
    throw std::runtime_error("Greek inputs must be positive");
  }
  const double sqrt_t = std::sqrt(expiry_years);
  const double d1 =
      (std::log(spot / strike) + (rate - dividend_yield + 0.5 * volatility * volatility) * expiry_years) /
      (volatility * sqrt_t);
  const double d2 = d1 - volatility * sqrt_t;
  const double discount_r = std::exp(-rate * expiry_years);
  const double discount_q = std::exp(-dividend_yield * expiry_years);

  GreekResult result;
  result.delta = is_call ? discount_q * NormalCdf(d1) : discount_q * (NormalCdf(d1) - 1.0);
  result.gamma = discount_q * NormalPdf(d1) / (spot * volatility * sqrt_t);
  result.vega = spot * discount_q * NormalPdf(d1) * sqrt_t;
  const double decay = -(spot * discount_q * NormalPdf(d1) * volatility) / (2.0 * sqrt_t);
  if (is_call) {
    result.theta = decay - rate * strike * discount_r * NormalCdf(d2) +
                   dividend_yield * spot * discount_q * NormalCdf(d1);
  } else {
    result.theta = decay + rate * strike * discount_r * NormalCdf(-d2) -
                   dividend_yield * spot * discount_q * NormalCdf(-d1);
  }
  return result;
}

std::vector<OptionQuote> LoadOptionQuotes(const std::filesystem::path& path) {
  const auto result = LoadOptionQuotesWithRejects(path);
  if (!result.rejected_rows.empty()) {
    throw std::runtime_error("malformed option quote CSV: " + path.string());
  }
  return result.quotes;
}

QuoteLoadResult LoadOptionQuotesWithRejects(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("failed to read option quotes: " + path.string());
  }
  QuoteLoadResult result;
  std::string line;
  int source_line = 0;
  while (std::getline(in, line)) {
    ++source_line;
    if (line.empty() || line.rfind("surface_id,", 0) == 0) {
      continue;
    }
    const auto fields = SplitCsv(line);
    if (fields.size() != 9) {
      result.rejected_rows.push_back(RejectedCsvRow(source_line, line, fields, "expected_9_csv_fields"));
      continue;
    }
    const auto expiry = ParseDouble(fields[3]);
    const auto strike = ParseDouble(fields[4]);
    const auto is_call = ParseBoolStrict(fields[5]);
    const auto bid = ParseDouble(fields[6]);
    const auto ask = ParseDouble(fields[7]);
    const auto quote_ts = ParseInt64(fields[8]);
    if (!expiry.has_value() || !strike.has_value() || !is_call.has_value() || !bid.has_value() ||
        !ask.has_value() || !quote_ts.has_value()) {
      result.rejected_rows.push_back(RejectedCsvRow(source_line, line, fields, "malformed_numeric_or_bool_field"));
      continue;
    }
    OptionQuote quote;
    quote.surface_id = fields[0];
    quote.quote_id = fields[1];
    quote.symbol = fields[2];
    quote.expiry_years = *expiry;
    quote.strike = *strike;
    quote.is_call = *is_call;
    quote.bid = *bid;
    quote.ask = *ask;
    quote.quote_ts_ns = *quote_ts;
    quote.source_line = source_line;
    quote.raw_csv = line;
    result.quotes.push_back(quote);
  }
  return result;
}

std::vector<FilteredQuote> FilterQuotes(const std::vector<OptionQuote>& quotes, const SurfaceConfig& config) {
  std::vector<FilteredQuote> filtered;
  filtered.reserve(quotes.size());
  for (const auto& quote : quotes) {
    FilteredQuote row;
    row.quote = quote;
    row.mid = 0.5 * (quote.bid + quote.ask);
    row.source_line = quote.source_line;
    row.raw_csv = quote.raw_csv;
    if (!std::isfinite(config.assumptions.spot) || config.assumptions.spot <= 0.0) {
      row.status = "INVALID_UNDERLYING";
      row.reason = "spot_not_positive_or_finite";
    } else if (!std::isfinite(quote.strike) || quote.strike <= 0.0) {
      row.status = "INVALID_STRIKE";
      row.reason = "strike_not_positive_or_finite";
    } else if (!std::isfinite(quote.expiry_years) || quote.expiry_years <= 0.0) {
      row.status = "INVALID_EXPIRY";
      row.reason = "expiry_not_positive_or_finite";
    } else if (!std::isfinite(quote.bid) || !std::isfinite(quote.ask) ||
               !std::isfinite(config.assumptions.rate) || !std::isfinite(config.assumptions.dividend_yield)) {
      row.status = "NAN_NUMERIC";
      row.reason = "numeric_field_not_finite";
    } else if (quote.bid <= 0.0 || quote.ask <= 0.0) {
      row.status = "NON_POSITIVE";
      row.reason = "bid_or_ask_not_positive";
    } else if (quote.bid > quote.ask) {
      row.status = "CROSSED";
      row.reason = "bid_above_ask";
    } else if (quote.quote_ts_ns > config.evaluation_ts_ns) {
      row.status = "FUTURE_TIMESTAMP";
      row.reason = "quote_ts_after_evaluation_ts";
    } else if (config.max_quote_age_ns >= 0 && config.evaluation_ts_ns - quote.quote_ts_ns > config.max_quote_age_ns) {
      row.status = "STALE";
      row.reason = "quote_age_exceeds_max_quote_age_ns";
    } else {
      row.implied_vol = ImpliedVol(row.mid, config.assumptions.spot, quote.strike, quote.expiry_years,
                                   config.assumptions.rate, config.assumptions.dividend_yield, quote.is_call,
                                   config.iv);
      if (!row.implied_vol.has_value()) {
        row.status = "UNINVERTIBLE";
        row.reason = "mid_price_outside_iv_bracket";
      } else {
        row.status = "ACCEPTED";
        row.reason = "ok";
      }
    }
    filtered.push_back(row);
  }
  return filtered;
}

SurfaceFitResult FitSurface(const std::vector<FilteredQuote>& quotes,
                            const SurfaceConfig& config,
                            const std::string& surface_id,
                            const std::string& input_checksum) {
  SurfaceFitResult result;
  result.surface_id = surface_id;
  result.surface_model_id = config.surface_model_id;
  result.algorithm_version = config.algorithm_version;
  result.input_checksum = input_checksum;
  result.quote_checksum = QuoteChecksum(quotes, surface_id);
  result.assumptions = config.assumptions;

  const auto accepted = AcceptedSurfaceQuotes(quotes, surface_id);
  result.accepted_quote_count = accepted.size();
  std::set<double> expiries;
  for (const auto* quote : accepted) {
    expiries.insert(quote->quote.expiry_years);
  }
  for (const double expiry : expiries) {
    result.forward_assumptions.push_back({expiry, Forward(config.assumptions, expiry)});
  }
  if (accepted.size() < 3) {
    result.status = "INSUFFICIENT_QUOTES";
    result.reason = "need_at_least_three_accepted_quotes";
    return result;
  }

  result.arbitrage = CheckStaticArbitrage(accepted);
  if (HasArbitrage(result.arbitrage)) {
    result.status = "STATIC_ARBITRAGE_VIOLATION";
    result.reason = FitReason(result.arbitrage);
    return result;
  }

  std::array<std::array<double, 4>, 3> normal{};
  for (const auto* quote : accepted) {
    const double forward = Forward(config.assumptions, quote->quote.expiry_years);
    const double x = std::log(quote->quote.strike / forward);
    const double y = (*quote->implied_vol) * (*quote->implied_vol) * quote->quote.expiry_years;
    const double spread = std::max(quote->quote.ask - quote->quote.bid, 1e-6);
    const double weight = 1.0 / spread;
    const std::array<double, 3> basis{1.0, x, x * x};
    for (int row = 0; row < 3; ++row) {
      for (int col = 0; col < 3; ++col) {
        normal[row][col] += weight * basis[row] * basis[col];
      }
      normal[row][3] += weight * basis[row] * y;
    }
  }

  std::array<double, 3> coeff{};
  if (!Solve3x3(normal, coeff)) {
    result.status = "FIT_SINGULAR";
    result.reason = "normal_equations_singular";
    return result;
  }

  double squared_error = 0.0;
  for (const auto* quote : accepted) {
    const double forward = Forward(config.assumptions, quote->quote.expiry_years);
    const double x = std::log(quote->quote.strike / forward);
    const double total_variance = coeff[0] + coeff[1] * x + coeff[2] * x * x;
    if (total_variance <= 0.0 || !std::isfinite(total_variance)) {
      result.status = "UNSAFE_FIT_NEGATIVE_VARIANCE";
      result.reason = "fitted_total_variance_not_positive";
      return result;
    }
    const double fitted_iv = std::sqrt(total_variance / quote->quote.expiry_years);
    const double error = fitted_iv - *quote->implied_vol;
    squared_error += error * error;
  }

  result.coefficients = {coeff[0], coeff[1], coeff[2]};
  result.rmse_iv = std::sqrt(squared_error / static_cast<double>(accepted.size()));
  result.status = "PASS";
  result.reason = "fit_ok";
  return result;
}

SurfaceInterpolationResult InterpolateSurfaceFairValue(const SurfaceFitResult& fit,
                                                       const SurfaceConfig& config,
                                                       const SurfaceInterpolationRequest& request) {
  SurfaceInterpolationResult result;
  result.request = request;
  if (fit.status != "PASS") {
    result.status = "SURFACE_NOT_PASS";
    result.reason = "fit_status_" + fit.status;
    return result;
  }
  if (fit.coefficients.size() != 3) {
    result.status = "MISSING_COEFFICIENTS";
    result.reason = "need_quadratic_total_variance_coefficients";
    return result;
  }
  if (!std::isfinite(config.assumptions.spot) || !std::isfinite(config.assumptions.rate) ||
      !std::isfinite(config.assumptions.dividend_yield) || config.assumptions.spot <= 0.0 ||
      !std::isfinite(request.strike) || request.strike <= 0.0 ||
      !std::isfinite(request.expiry_years) || request.expiry_years <= 0.0) {
    result.status = "INVALID_INPUT";
    result.reason = "spot_strike_expiry_must_be_positive_finite";
    return result;
  }
  const double forward = Forward(config.assumptions, request.expiry_years);
  const double x = std::log(request.strike / forward);
  const double total_variance = fit.coefficients[0] + fit.coefficients[1] * x + fit.coefficients[2] * x * x;
  if (!std::isfinite(total_variance) || total_variance <= 0.0) {
    result.status = "UNSAFE_FIT_NEGATIVE_VARIANCE";
    result.reason = "interpolated_total_variance_not_positive";
    return result;
  }
  result.implied_vol = std::sqrt(total_variance / request.expiry_years);
  result.fair_value = BlackScholesPrice(config.assumptions.spot, request.strike, request.expiry_years,
                                        config.assumptions.rate, config.assumptions.dividend_yield,
                                        *result.implied_vol, request.is_call);
  result.status = "PASS";
  result.reason = "interpolation_ok";
  return result;
}

std::vector<ForwardCurvePoint> ExtractForwardsFromPutCallParity(const std::vector<FilteredQuote>&,
                                                                const std::string&) {
  // ponytail: parity extraction needs chain grouping and quality policy; keep manifest forwards until implemented.
  return {};
}

std::string Fnv64File(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("failed to hash file: " + path.string());
  }
  std::uint64_t hash = 14695981039346656037ull;
  for (const char ch : std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>())) {
    hash ^= static_cast<unsigned char>(ch);
    hash *= 1099511628211ull;
  }
  return Fnv64Hex(hash);
}

std::string FilteredQuoteJson(const FilteredQuote& quote) {
  std::ostringstream out;
  bool first = true;
  out << '{';
  AddJsonField(out, "surface_id", JsonString(quote.quote.surface_id), first);
  AddJsonField(out, "quote_id", JsonString(quote.quote.quote_id), first);
  AddJsonField(out, "symbol", JsonString(quote.quote.symbol), first);
  AddJsonField(out, "expiry_years", JsonDouble(quote.quote.expiry_years), first);
  AddJsonField(out, "strike", JsonDouble(quote.quote.strike), first);
  AddJsonField(out, "is_call", quote.quote.is_call ? "true" : "false", first);
  AddJsonField(out, "bid", JsonDouble(quote.quote.bid), first);
  AddJsonField(out, "ask", JsonDouble(quote.quote.ask), first);
  AddJsonField(out, "mid", JsonDouble(quote.mid), first);
  AddJsonField(out, "quote_ts_ns", std::to_string(quote.quote.quote_ts_ns), first);
  AddJsonField(out, "source_line", std::to_string(quote.source_line), first);
  AddJsonField(out, "status", JsonString(quote.status), first);
  AddJsonField(out, "reason", JsonString(quote.reason), first);
  AddJsonField(out, "implied_vol", JsonOptionalDouble(quote.implied_vol), first);
  AddJsonField(out, "raw_csv", JsonString(quote.raw_csv), first);
  out << '}';
  return out.str();
}

std::string SurfaceFitJson(const SurfaceFitResult& result) {
  std::ostringstream out;
  bool first = true;
  out << '{';
  AddJsonField(out, "surface_id", JsonString(result.surface_id), first);
  AddJsonField(out, "surface_model_id", JsonString(result.surface_model_id), first);
  AddJsonField(out, "algorithm_version", JsonString(result.algorithm_version), first);
  AddJsonField(out, "status", JsonString(result.status), first);
  AddJsonField(out, "reason", JsonString(result.reason), first);
  AddJsonField(out, "accepted_quote_count", std::to_string(result.accepted_quote_count), first);
  AddJsonField(out, "quote_checksum", JsonString(result.quote_checksum), first);
  AddJsonField(out, "input_checksum", JsonString(result.input_checksum), first);
  AddJsonField(out, "rmse_iv", JsonOptionalDouble(result.rmse_iv), first);
  std::ostringstream coeffs;
  coeffs << '[';
  for (std::size_t i = 0; i < result.coefficients.size(); ++i) {
    if (i != 0) {
      coeffs << ',';
    }
    coeffs << JsonDouble(result.coefficients[i]);
  }
  coeffs << ']';
  AddJsonField(out, "coefficients", coeffs.str(), first);
  std::ostringstream arb;
  arb << "{\"monotonic_violations\":" << result.arbitrage.monotonic_violations
      << ",\"butterfly_violations\":" << result.arbitrage.butterfly_violations
      << ",\"calendar_violations\":" << result.arbitrage.calendar_violations << '}';
  AddJsonField(out, "static_arbitrage", arb.str(), first);
  std::ostringstream assumptions;
  assumptions << "{\"spot\":" << JsonDouble(result.assumptions.spot) << ",\"rate\":"
              << JsonDouble(result.assumptions.rate) << ",\"dividend_yield\":"
              << JsonDouble(result.assumptions.dividend_yield) << ",\"forwards_by_expiry\":[";
  for (std::size_t i = 0; i < result.forward_assumptions.size(); ++i) {
    if (i != 0) {
      assumptions << ',';
    }
    assumptions << "{\"expiry_years\":" << JsonDouble(result.forward_assumptions[i].expiry_years)
                << ",\"forward\":" << JsonDouble(result.forward_assumptions[i].forward) << '}';
  }
  assumptions << "]}";
  AddJsonField(out, "forward_rate_dividend_assumptions", assumptions.str(), first);
  if (result.interpolation.has_value()) {
    const auto& interp = *result.interpolation;
    std::ostringstream interpolation;
    interpolation << "{\"surface_id\":" << JsonString(interp.request.surface_id)
                  << ",\"expiry_years\":" << JsonDouble(interp.request.expiry_years)
                  << ",\"strike\":" << JsonDouble(interp.request.strike)
                  << ",\"is_call\":" << (interp.request.is_call ? "true" : "false")
                  << ",\"status\":" << JsonString(interp.status)
                  << ",\"reason\":" << JsonString(interp.reason)
                  << ",\"implied_vol\":" << JsonOptionalDouble(interp.implied_vol)
                  << ",\"fair_value\":" << JsonOptionalDouble(interp.fair_value) << '}';
    AddJsonField(out, "interpolation", interpolation.str(), first);
  }
  out << '}';
  return out.str();
}

std::string SurfaceDiagnosticsJson(const SurfaceConfig& config,
                                   const std::filesystem::path& input,
                                   const std::string& input_checksum,
                                   const std::vector<FilteredQuote>& quotes,
                                   const SurfaceFitResult& clean,
                                   const SurfaceFitResult& corrupt) {
  std::map<std::string, int> counts;
  for (const auto& quote : quotes) {
    ++counts[quote.status];
  }
  const bool expected_corruption_caught = config.corrupted_surface_id.empty() ||
                                          corrupt.status == "STATIC_ARBITRAGE_VIOLATION";
  const std::string overall = clean.status == "PASS" && expected_corruption_caught ? "pass" : "fail";

  std::ostringstream out;
  out << "{\n";
  out << "  \"schema_version\": \"vegaflux.canonical_market.v0.1\",\n";
  out << "  \"component_id\": " << JsonString(config.component_id) << ",\n";
  out << "  \"status\": " << JsonString(overall) << ",\n";
  out << "  \"surface_model_id\": " << JsonString(config.surface_model_id) << ",\n";
  out << "  \"algorithm_version\": " << JsonString(config.algorithm_version) << ",\n";
  out << "  \"input\": " << JsonString(input.generic_string()) << ",\n";
  out << "  \"input_checksum\": " << JsonString(input_checksum) << ",\n";
  out << "  \"evaluation_ts_ns\": " << config.evaluation_ts_ns << ",\n";
  out << "  \"max_quote_age_ns\": " << config.max_quote_age_ns << ",\n";
  out << "  \"assumptions\": {\"spot\": " << JsonDouble(config.assumptions.spot)
      << ", \"rate\": " << JsonDouble(config.assumptions.rate)
      << ", \"dividend_yield\": " << JsonDouble(config.assumptions.dividend_yield) << "},\n";
  out << "  \"quote_filter_counts\": {";
  bool first_count = true;
  for (const auto& [status, count] : counts) {
    if (!first_count) {
      out << ',';
    }
    first_count = false;
    out << JsonString(status) << ':' << count;
  }
  out << "},\n";
  out << "  \"fits\": [\n";
  out << "    " << SurfaceFitJson(clean) << ",\n";
  out << "    " << SurfaceFitJson(corrupt) << "\n";
  out << "  ],\n";
  out << "  \"forward_extraction_status\": \"TODO_put_call_parity_fixture_ready_interface_no_fake_forward\",\n";
  out << "  \"note\": \"synthetic diagnostic baseline; no strategy or performance claim\"\n";
  out << "}\n";
  return out.str();
}

void WriteFilteredQuotesJsonl(const std::filesystem::path& path, const std::vector<FilteredQuote>& quotes) {
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw std::runtime_error("failed to write filtered quotes: " + path.string());
  }
  for (const auto& quote : quotes) {
    out << FilteredQuoteJson(quote) << '\n';
  }
}

void WriteSurfaceDiagnostics(const std::filesystem::path& path,
                             const SurfaceConfig& config,
                             const std::filesystem::path& input,
                             const std::string& input_checksum,
                             const std::vector<FilteredQuote>& quotes,
                             const SurfaceFitResult& clean,
                             const SurfaceFitResult& corrupt) {
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw std::runtime_error("failed to write surface diagnostics: " + path.string());
  }
  out << SurfaceDiagnosticsJson(config, input, input_checksum, quotes, clean, corrupt);
}

void WriteSourceTableCsv(const std::filesystem::path& path, const std::vector<FilteredQuote>& quotes) {
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw std::runtime_error("failed to write source table: " + path.string());
  }
  out << "source_line,surface_id,quote_id,symbol,expiry_years,strike,is_call,bid,ask,quote_ts_ns,status,reason\n";
  for (const auto& quote : quotes) {
    out << quote.source_line << ',' << CsvString(quote.quote.surface_id) << ',' << CsvString(quote.quote.quote_id)
        << ',' << CsvString(quote.quote.symbol) << ',' << JsonDouble(quote.quote.expiry_years) << ','
        << JsonDouble(quote.quote.strike) << ',' << (quote.quote.is_call ? 1 : 0) << ',' << JsonDouble(quote.quote.bid)
        << ',' << JsonDouble(quote.quote.ask) << ',' << quote.quote.quote_ts_ns << ',' << CsvString(quote.status)
        << ',' << CsvString(quote.reason) << '\n';
  }
}

void WriteIvResidualTableCsv(const std::filesystem::path& path,
                             const std::vector<FilteredQuote>& quotes,
                             const SurfaceFitResult& fit,
                             const SurfaceConfig& config) {
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw std::runtime_error("failed to write IV residual table: " + path.string());
  }
  out << "surface_id,quote_id,expiry_years,strike,is_call,market_iv,fitted_iv,residual_iv,market_mid,fitted_price,residual_price,status\n";
  if (fit.status != "PASS" || fit.coefficients.size() != 3) {
    return;
  }
  for (const auto& quote : quotes) {
    if (quote.quote.surface_id != fit.surface_id || quote.status != "ACCEPTED" || !quote.implied_vol.has_value()) {
      continue;
    }
    const double forward = Forward(config.assumptions, quote.quote.expiry_years);
    const double x = std::log(quote.quote.strike / forward);
    const double total_variance = fit.coefficients[0] + fit.coefficients[1] * x + fit.coefficients[2] * x * x;
    if (!std::isfinite(total_variance) || total_variance <= 0.0) {
      out << CsvString(fit.surface_id) << ',' << CsvString(quote.quote.quote_id) << ','
          << JsonDouble(quote.quote.expiry_years) << ',' << JsonDouble(quote.quote.strike) << ','
          << (quote.quote.is_call ? 1 : 0) << ',' << JsonDouble(*quote.implied_vol)
          << ",,,,," << CsvString("UNSAFE_FIT_NEGATIVE_VARIANCE") << '\n';
      continue;
    }
    const double fitted_iv = std::sqrt(total_variance / quote.quote.expiry_years);
    const double fitted_price = BlackScholesPrice(config.assumptions.spot, quote.quote.strike, quote.quote.expiry_years,
                                                 config.assumptions.rate, config.assumptions.dividend_yield, fitted_iv,
                                                 quote.quote.is_call);
    out << CsvString(fit.surface_id) << ',' << CsvString(quote.quote.quote_id) << ','
        << JsonDouble(quote.quote.expiry_years) << ',' << JsonDouble(quote.quote.strike) << ','
        << (quote.quote.is_call ? 1 : 0) << ',' << JsonDouble(*quote.implied_vol) << ',' << JsonDouble(fitted_iv)
        << ',' << JsonDouble(fitted_iv - *quote.implied_vol) << ',' << JsonDouble(quote.mid) << ','
        << JsonDouble(fitted_price) << ',' << JsonDouble(fitted_price - quote.mid) << ',' << CsvString("PASS")
        << '\n';
  }
}

void WriteStaticArbitrageTableCsv(const std::filesystem::path& path,
                                  const std::vector<SurfaceFitResult>& fits) {
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw std::runtime_error("failed to write static-arbitrage table: " + path.string());
  }
  out << "surface_id,status,reason,accepted_quote_count,monotonic_violations,butterfly_violations,calendar_violations\n";
  for (const auto& fit : fits) {
    out << CsvString(fit.surface_id) << ',' << CsvString(fit.status) << ',' << CsvString(fit.reason) << ','
        << fit.accepted_quote_count << ',' << fit.arbitrage.monotonic_violations << ','
        << fit.arbitrage.butterfly_violations << ',' << fit.arbitrage.calendar_violations << '\n';
  }
}

void WriteGreekFiniteDifferenceTableCsv(const std::filesystem::path& path,
                                        const MarketAssumptions& assumptions,
                                        double expiry_years,
                                        double strike,
                                        double volatility) {
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw std::runtime_error("failed to write Greek finite-difference table: " + path.string());
  }
  out << "option_type,greek,analytic,finite_difference,abs_error\n";
  constexpr double ds = 1e-3;
  constexpr double dv = 1e-4;
  constexpr double dt = 1e-5;
  for (const bool is_call : {true, false}) {
    const auto price = [&](double s, double t, double v) {
      return BlackScholesPrice(s, strike, t, assumptions.rate, assumptions.dividend_yield, v, is_call);
    };
    const auto greeks =
        BlackScholesGreeks(assumptions.spot, strike, expiry_years, assumptions.rate, assumptions.dividend_yield,
                           volatility, is_call);
    const double base = price(assumptions.spot, expiry_years, volatility);
    const double delta_fd = (price(assumptions.spot + ds, expiry_years, volatility) -
                             price(assumptions.spot - ds, expiry_years, volatility)) /
                            (2.0 * ds);
    const double gamma_fd = (price(assumptions.spot + ds, expiry_years, volatility) - 2.0 * base +
                             price(assumptions.spot - ds, expiry_years, volatility)) /
                            (ds * ds);
    const double vega_fd = (price(assumptions.spot, expiry_years, volatility + dv) -
                            price(assumptions.spot, expiry_years, volatility - dv)) /
                           (2.0 * dv);
    const double theta_fd = (price(assumptions.spot, expiry_years - dt, volatility) -
                             price(assumptions.spot, expiry_years + dt, volatility)) /
                            (2.0 * dt);
    const std::string type = is_call ? "call" : "put";
    out << type << ",delta," << JsonDouble(greeks.delta) << ',' << JsonDouble(delta_fd) << ','
        << JsonDouble(std::abs(greeks.delta - delta_fd)) << '\n';
    out << type << ",gamma," << JsonDouble(greeks.gamma) << ',' << JsonDouble(gamma_fd) << ','
        << JsonDouble(std::abs(greeks.gamma - gamma_fd)) << '\n';
    out << type << ",vega," << JsonDouble(greeks.vega) << ',' << JsonDouble(vega_fd) << ','
        << JsonDouble(std::abs(greeks.vega - vega_fd)) << '\n';
    out << type << ",theta," << JsonDouble(greeks.theta) << ',' << JsonDouble(theta_fd) << ','
        << JsonDouble(std::abs(greeks.theta - theta_fd)) << '\n';
  }
}

}  // namespace vf_options
