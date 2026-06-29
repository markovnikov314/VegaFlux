#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace vf_options {

struct MarketAssumptions {
  double spot{};
  double rate{};
  double dividend_yield{};
};

struct IvConfig {
  double min_vol{0.0001};
  double max_vol{5.0};
  int max_iterations{100};
  double price_tolerance{1e-10};
};

struct SurfaceConfig {
  std::string component_id{"options"};
  std::string surface_id{"clean_smile_v1"};
  std::string corrupted_surface_id{"corrupt_smile_v1"};
  std::string surface_model_id{"quadratic_total_variance_v1"};
  std::string algorithm_version{"vegaflux_options_cpp_v1"};
  std::int64_t evaluation_ts_ns{};
  std::int64_t max_quote_age_ns{};
  MarketAssumptions assumptions;
  IvConfig iv;
};

struct OptionQuote {
  std::string surface_id;
  std::string quote_id;
  std::string symbol;
  double expiry_years{};
  double strike{};
  bool is_call{true};
  double bid{};
  double ask{};
  std::int64_t quote_ts_ns{};
  int source_line{};
  std::string raw_csv;
};

struct FilteredQuote {
  OptionQuote quote;
  std::string status;
  std::string reason;
  double mid{};
  std::optional<double> implied_vol;
  int source_line{};
  std::string raw_csv;
};

struct GreekResult {
  double delta{};
  double gamma{};
  double vega{};
  double theta{};
};

struct StaticArbitrageDiagnostics {
  int monotonic_violations{};
  int butterfly_violations{};
  int calendar_violations{};
};

struct ForwardAssumption {
  double expiry_years{};
  double forward{};
};

struct ForwardCurvePoint {
  double expiry_years{};
  double forward{};
  std::string status;
  std::string source;
};

struct QuoteLoadResult {
  std::vector<OptionQuote> quotes;
  std::vector<FilteredQuote> rejected_rows;
};

struct SurfaceInterpolationRequest {
  std::string surface_id;
  double expiry_years{};
  double strike{};
  bool is_call{true};
};

struct SurfaceInterpolationResult {
  SurfaceInterpolationRequest request;
  std::string status;
  std::string reason;
  std::optional<double> implied_vol;
  std::optional<double> fair_value;
};

struct SurfaceFitResult {
  std::string surface_id;
  std::string surface_model_id;
  std::string algorithm_version;
  std::string status;
  std::string reason;
  std::string quote_checksum;
  std::string input_checksum;
  MarketAssumptions assumptions;
  std::size_t accepted_quote_count{};
  StaticArbitrageDiagnostics arbitrage;
  std::vector<double> coefficients;
  std::vector<ForwardAssumption> forward_assumptions;
  std::optional<double> rmse_iv;
  std::optional<SurfaceInterpolationResult> interpolation;
};

double NormalCdf(double x);
double NormalPdf(double x);
double BlackPrice(double forward, double strike, double discount, double expiry_years, double volatility, bool is_call);
double BlackScholesPrice(double spot,
                         double strike,
                         double expiry_years,
                         double rate,
                         double dividend_yield,
                         double volatility,
                         bool is_call);
std::optional<double> ImpliedVol(double target_price,
                                 double spot,
                                 double strike,
                                 double expiry_years,
                                 double rate,
                                 double dividend_yield,
                                 bool is_call,
                                 const IvConfig& config);
GreekResult BlackScholesGreeks(double spot,
                               double strike,
                               double expiry_years,
                               double rate,
                               double dividend_yield,
                               double volatility,
                               bool is_call);

std::vector<OptionQuote> LoadOptionQuotes(const std::filesystem::path& path);
QuoteLoadResult LoadOptionQuotesWithRejects(const std::filesystem::path& path);
std::vector<FilteredQuote> FilterQuotes(const std::vector<OptionQuote>& quotes, const SurfaceConfig& config);
SurfaceFitResult FitSurface(const std::vector<FilteredQuote>& quotes,
                            const SurfaceConfig& config,
                            const std::string& surface_id,
                            const std::string& input_checksum);
SurfaceInterpolationResult InterpolateSurfaceFairValue(const SurfaceFitResult& fit,
                                                       const SurfaceConfig& config,
                                                       const SurfaceInterpolationRequest& request);
std::vector<ForwardCurvePoint> ExtractForwardsFromPutCallParity(const std::vector<FilteredQuote>& quotes,
                                                                const std::string& surface_id);
std::string Fnv64File(const std::filesystem::path& path);
std::string FilteredQuoteJson(const FilteredQuote& quote);
std::string SurfaceFitJson(const SurfaceFitResult& result);
std::string SurfaceDiagnosticsJson(const SurfaceConfig& config,
                                   const std::filesystem::path& input,
                                   const std::string& input_checksum,
                                   const std::vector<FilteredQuote>& quotes,
                                   const SurfaceFitResult& clean,
                                   const SurfaceFitResult& corrupt);
void WriteFilteredQuotesJsonl(const std::filesystem::path& path, const std::vector<FilteredQuote>& quotes);
void WriteSurfaceDiagnostics(const std::filesystem::path& path,
                             const SurfaceConfig& config,
                             const std::filesystem::path& input,
                             const std::string& input_checksum,
                             const std::vector<FilteredQuote>& quotes,
                             const SurfaceFitResult& clean,
                             const SurfaceFitResult& corrupt);
void WriteSourceTableCsv(const std::filesystem::path& path, const std::vector<FilteredQuote>& quotes);
void WriteIvResidualTableCsv(const std::filesystem::path& path,
                             const std::vector<FilteredQuote>& quotes,
                             const SurfaceFitResult& fit,
                             const SurfaceConfig& config);
void WriteStaticArbitrageTableCsv(const std::filesystem::path& path,
                                  const std::vector<SurfaceFitResult>& fits);
void WriteGreekFiniteDifferenceTableCsv(const std::filesystem::path& path,
                                        const MarketAssumptions& assumptions,
                                        double expiry_years,
                                        double strike,
                                        double volatility);

}  // namespace vf_options
