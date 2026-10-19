#include "primary-model.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>

namespace btc::directional {
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void finite(double v) { require(std::isfinite(v), "Non-finite model input"); }
std::vector<std::string> split(std::string line) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::vector<std::string> fields;
    std::string field;
    std::istringstream in(line);
    while (std::getline(in,field,',')) fields.push_back(field);
    if (!line.empty() && line.back() == ',') fields.emplace_back();
    return fields;
}
class Csv {
public:
    explicit Csv(const std::string& path) : file_(path), path_(path) {
        require(static_cast<bool>(file_), ("Cannot open " + path).c_str());
        std::string header; std::getline(file_,header);
        const auto names = split(header); count_ = names.size();
        for (std::size_t i = 0; i < names.size(); ++i)
            require(columns_.emplace(names[i],i).second,"Duplicate CSV column");
    }
    bool next() {
        std::string line;
        while (std::getline(file_,line)) {
            if (line.empty() || line == "\r") continue;
            row_ = split(line); require(row_.size() == count_,"CSV field-count mismatch"); return true;
        }
        return false;
    }
    double number(const std::string& name) const {
        const auto& s = field(name); std::size_t used = 0;
        const double v = std::stod(s,&used); finite(v);
        require(used == s.size(),("Invalid CSV value in " + path_).c_str()); return v;
    }
    Timestamp timestamp() const {
        const auto& s = field("ts"); std::size_t used = 0;
        const auto v = std::stoll(s,&used);
        require(used == s.size() && v >= 0 && v % 60 == 0,"Invalid minute timestamp"); return v;
    }
private:
    const std::string& field(const std::string& name) const {
        const auto it = columns_.find(name);
        require(it != columns_.end(),("Missing column " + name).c_str()); return row_.at(it->second);
    }
    std::ifstream file_; std::string path_;
    std::map<std::string,std::size_t> columns_;
    std::vector<std::string> row_; std::size_t count_ = 0;
};
std::vector<std::pair<Timestamp,double>> prices(const std::string& path) {
    Csv csv(path); std::vector<std::pair<Timestamp,double>> result;
    while (csv.next()) {
        const auto ts = csv.timestamp(); const double p = csv.number("close");
        require(p > 0,"Price must be positive");
        require(result.empty() || ts > result.back().first,"Price timestamps must strictly increase");
        result.emplace_back(ts+60,p); // A minute close is not known at its START timestamp.
    }
    require(!result.empty(),"No prices in CSV"); return result;
}
void validate_snapshot(const Snapshot& s) {
    for (double v : {s.perp,s.spot,s.perp_return_5m_bps,s.spot_return_5m_bps,
                    s.perp_volatility_5m_bps,s.known_hourly_funding_bps,s.minutes_to_next_funding}) finite(v);
    require(s.perp > 0 && s.spot > 0 && s.perp_volatility_5m_bps >= 0 &&
            s.minutes_to_next_funding > 0 && s.minutes_to_next_funding <= 60,"Invalid market snapshot");
}
double payment_count(int horizon, double next) {
    return horizon < next ? 0 : 1+std::floor((horizon-next)/60+1e-12);
}
} // namespace

Features features(const Snapshot& s, int horizon) {
    validate_snapshot(s); require(horizon > 0 && horizon <= 1440,"Horizon must be 1..1440 minutes");
    return {{(s.perp/s.spot-1)*bps,s.perp_return_5m_bps,s.spot_return_5m_bps,
             s.perp_volatility_5m_bps,s.known_hourly_funding_bps,payment_count(horizon,s.minutes_to_next_funding)}};
}
double forecast_funding_per_btc(const Snapshot& s, double predicted, int horizon, std::optional<double> paid_bps) {
    validate_snapshot(s); finite(predicted); require(predicted > 0 && horizon > 0,"Invalid funding forecast horizon/price");
    if (paid_bps) { finite(*paid_bps); return s.perp * *paid_bps / bps; }
    // Legacy INTX only: persist the known hourly rate, with projected average mark.
    return payment_count(horizon,s.minutes_to_next_funding) * s.known_hourly_funding_bps / bps * (s.perp+predicted)/2;
}
ProfitEstimate profits(const Snapshot& s, double predicted, double funding, double quantity, int horizon, const Costs& c) {
    validate_snapshot(s);
    for (double v : {predicted,funding,quantity,c.entry_fee_bps,c.exit_fee_bps,c.half_spread_bps,
                    c.slippage_bps_per_fill,c.financing_apr,c.margin_fraction,c.min_net_edge_bps}) finite(v);
    require(predicted > 0 && quantity > 0 && horizon > 0 && c.half_spread_bps >= 0 &&
        c.slippage_bps_per_fill >= 0 && c.financing_apr >= 0 && c.margin_fraction > 0 && c.margin_fraction <= 1 &&
        c.min_net_edge_bps >= 0 && std::abs(c.entry_fee_bps) < bps && std::abs(c.exit_fee_bps) < bps,
        "Invalid costs, quantity or forecast");
    const double adverse = (c.half_spread_bps+c.slippage_bps_per_fill)/bps;
    require(adverse < 1,"Execution cost exceeds price");
    ProfitEstimate p;
    p.predicted_perp = predicted; p.funding_paid_per_btc = funding;
    p.long_entry = s.perp*(1+adverse); p.long_exit = predicted*(1-adverse);
    p.short_entry = s.perp*(1-adverse); p.short_exit = predicted*(1+adverse);
    p.long_fees = quantity*(p.long_entry*c.entry_fee_bps+p.long_exit*c.exit_fee_bps)/bps;
    p.short_fees = quantity*(p.short_entry*c.entry_fee_bps+p.short_exit*c.exit_fee_bps)/bps;
    p.financing = quantity*s.perp*c.margin_fraction*c.financing_apr*horizon/(365.0*24*60);
    p.long_pnl = quantity*(p.long_exit-p.long_entry-funding)-p.long_fees-p.financing;
    p.short_pnl = quantity*(p.short_entry-p.short_exit+funding)-p.short_fees-p.financing;
    p.long_edge_bps = p.long_pnl/(quantity*s.perp)*bps;
    p.short_edge_bps = p.short_pnl/(quantity*s.perp)*bps;
    p.decision = "NEITHER";
    if (p.long_edge_bps > c.min_net_edge_bps && p.long_pnl > p.short_pnl) p.decision = "LONG";
    else if (p.short_edge_bps > c.min_net_edge_bps && p.short_pnl > p.long_pnl) p.decision = "SHORT";
    for (double v : {p.long_pnl,p.short_pnl,p.long_edge_bps,p.short_edge_bps}) finite(v);
    return p;
}

Dataset load_dataset(const std::string& spot_path, const std::string& perp_path, const std::string& funding_path) {
    const auto spot = prices(spot_path), perp = prices(perp_path);
    Dataset d; Csv csv(funding_path);
    while (csv.next()) {
        Funding f{csv.timestamp(),csv.number("funding_rate"),csv.number("mark_price")};
        require(f.ts % 3600 == 0 && std::abs(f.rate) < 1 && f.mark > 0,"Invalid legacy hourly funding record");
        require(d.funding.empty() || f.ts-d.funding.back().ts == 3600,"Missing/duplicate hourly funding; cannot treat gaps as zero");
        d.funding.push_back(f);
    }
    require(!d.funding.empty(),"No funding history");
    std::size_t i = 0,j = 0;
    while (i < spot.size() && j < perp.size()) {
        if (spot[i].first < perp[j].first) ++i;
        else if (perp[j].first < spot[i].first) ++j;
        else {
            if (spot[i].first >= d.funding.front().ts && spot[i].first <= d.funding.back().ts)
                d.points.push_back({spot[i].first,spot[i].second,perp[j].second});
            ++i; ++j;
        }
    }
    require(d.points.size() >= 20,"Too few synchronized price observations"); return d;
}
Snapshot snapshot_at(const Dataset& d, std::size_t i) {
    require(i >= 5 && i < d.points.size(),"Snapshot needs five prior minutes");
    const auto& p = d.points[i];
    for (std::size_t j = i-4; j <= i; ++j)
        require(d.points[j].ts-d.points[j-1].ts == 60,"Five-minute feature window crosses a data gap");
    const auto f = std::upper_bound(d.funding.begin(),d.funding.end(),p.ts,
        [](Timestamp ts,const Funding& row) { return ts < row.ts; });
    require(f != d.funding.begin(),"No already known funding rate");
    double sum = 0,squares = 0;
    for (std::size_t j = i-4; j <= i; ++j) {
        const double r = (d.points[j].perp/d.points[j-1].perp-1)*bps;
        sum += r; squares += r*r;
    }
    Snapshot s;
    s.ts = p.ts; s.spot = p.spot; s.perp = p.perp;
    s.perp_return_5m_bps = (p.perp/d.points[i-5].perp-1)*bps;
    s.spot_return_5m_bps = (p.spot/d.points[i-5].spot-1)*bps;
    s.perp_volatility_5m_bps = std::sqrt(std::max(0.0,squares/5-(sum/5)*(sum/5)));
    s.known_hourly_funding_bps = (f-1)->rate*bps;
    s.minutes_to_next_funding = static_cast<double>(3600-p.ts%3600)/60;
    return s;
}
std::vector<Example> make_examples(const Dataset& d, int horizon) {
    require(horizon > 0 && horizon <= 1440,"Horizon must be 1..1440 minutes");
    std::vector<Example> examples;
    std::vector<std::size_t> gaps(d.points.size(),0);
    for (std::size_t i = 1; i < d.points.size(); ++i)
        gaps[i] = gaps[i-1]+(d.points[i].ts-d.points[i-1].ts != 60);
    for (std::size_t i = 5; i+static_cast<std::size_t>(horizon) < d.points.size(); ++i) {
        const auto j = i+static_cast<std::size_t>(horizon);
        if (gaps[j] != gaps[i-5]) continue;
        const auto s = snapshot_at(d,i);
        examples.push_back({s.ts,d.points[j].ts,features(s,horizon),(d.points[j].perp/s.perp-1)*bps});
    }
    require(!examples.empty(),"No complete training windows"); return examples;
}

double ForecastModel::predict_return_bps(const Features& x) const {
    double prediction = coefficients[0];
    for (std::size_t j = 0; j < feature_count; ++j) {
        finite(x[j]); require(scales[j] > 0,"Invalid fitted feature scale");
        prediction += coefficients[j+1]*(x[j]-means[j])/scales[j];
    }
    finite(prediction); return prediction;
}
Metrics evaluate(const ForecastModel& model, const std::vector<Example>& examples) {
    require(!examples.empty(),"Empty evaluation period"); Metrics m; m.observations = examples.size();
    for (const auto& e : examples) {
        const double error = model.predict_return_bps(e.x)-e.target_return_bps;
        m.mae_bps += std::abs(error); m.rmse_bps += error*error;
        m.no_change_mae_bps += std::abs(e.target_return_bps);
        m.no_change_rmse_bps += e.target_return_bps*e.target_return_bps;
    }
    const double n = static_cast<double>(examples.size());
    m.mae_bps /= n; m.rmse_bps = std::sqrt(m.rmse_bps/n);
    m.no_change_mae_bps /= n; m.no_change_rmse_bps = std::sqrt(m.no_change_rmse_bps/n); return m;
}
ForecastModel fit(const std::vector<Example>& examples,int horizon,double ridge) {
    finite(ridge); require(ridge > 0,"Ridge strength must be positive");
    require(horizon > 0 && horizon <= 1440 && examples.size() > feature_count+1,"Insufficient regression samples or invalid horizon");
    ForecastModel m; m.horizon_minutes = horizon; m.ridge = ridge;
    const double n = static_cast<double>(examples.size());
    for (const auto& e : examples) {
        finite(e.target_return_bps);
        for (std::size_t j = 0; j < feature_count; ++j) { finite(e.x[j]); m.means[j] += e.x[j]/n; }
    }
    for (const auto& e : examples)
        for (std::size_t j = 0; j < feature_count; ++j) m.scales[j] += (e.x[j]-m.means[j])*(e.x[j]-m.means[j])/n;
    for (double& scale : m.scales) { scale = std::sqrt(scale); if (scale < 1e-12) scale = 1; }
    constexpr std::size_t k = feature_count+1;
    std::array<std::array<double,k+1>,k> matrix{};
    for (const auto& e : examples) {
        std::array<double,k> x{}; x[0] = 1;
        for (std::size_t j = 0; j < feature_count; ++j) x[j+1] = (e.x[j]-m.means[j])/m.scales[j];
        for (std::size_t a = 0; a < k; ++a) {
            for (std::size_t b = 0; b < k; ++b) matrix[a][b] += x[a]*x[b]/n;
            matrix[a][k] += x[a]*e.target_return_bps/n;
        }
    }
    for (std::size_t j = 1; j < k; ++j) matrix[j][j] += ridge; // Do not penalize the intercept.
    for (std::size_t col = 0; col < k; ++col) {
        auto pivot = col;
        for (std::size_t row = col+1; row < k; ++row)
            if (std::abs(matrix[row][col]) > std::abs(matrix[pivot][col])) pivot = row;
        require(std::abs(matrix[pivot][col]) > 1e-12,"Singular regression system");
        std::swap(matrix[col],matrix[pivot]); const double scale = matrix[col][col];
        for (std::size_t j = col; j <= k; ++j) matrix[col][j] /= scale;
        for (std::size_t row = 0; row < k; ++row) if (row != col) {
            const double factor = matrix[row][col];
            for (std::size_t j = col; j <= k; ++j) matrix[row][j] -= factor*matrix[col][j];
        }
    }
    for (std::size_t j = 0; j < k; ++j) { m.coefficients[j] = matrix[j][k]; finite(m.coefficients[j]); }
    m.training_rmse_bps = evaluate(m,examples).rmse_bps; return m;
}
void ForecastModel::save(const std::string& path) const {
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    std::ofstream out(path); require(static_cast<bool>(out),"Cannot save fitted model");
    out.exceptions(std::ios::badbit|std::ios::failbit); out << std::setprecision(17);
    out << "BTC_DIRECTIONAL_V1\n" << horizon_minutes << ' ' << ridge << ' ' << training_rmse_bps << '\n';
    for (const auto& values : {means,scales}) { for (double v : values) out << v << ' '; out << '\n'; }
    for (double v : coefficients) out << v << ' ';
    out << '\n' << validation.observations << ' ' << validation.mae_bps << ' ' << validation.rmse_bps
        << ' ' << validation.no_change_mae_bps << ' ' << validation.no_change_rmse_bps << '\n';
}
ForecastModel ForecastModel::load(const std::string& path) {
    std::ifstream in(path); require(static_cast<bool>(in),"Cannot read fitted model; train it first");
    std::string version; in >> version; require(version == "BTC_DIRECTIONAL_V1","Unsupported fitted model format");
    ForecastModel m; in >> m.horizon_minutes >> m.ridge >> m.training_rmse_bps;
    for (double& v : m.means) in >> v;
    for (double& v : m.scales) in >> v;
    for (double& v : m.coefficients) in >> v;
    in >> m.validation.observations >> m.validation.mae_bps >> m.validation.rmse_bps
       >> m.validation.no_change_mae_bps >> m.validation.no_change_rmse_bps;
    require(static_cast<bool>(in),"Malformed fitted model");
    require(m.horizon_minutes > 0 && m.horizon_minutes <= 1440 && m.ridge > 0,"Invalid fitted model parameters");
    for (double v : m.means) finite(v);
    for (double v : m.scales) { finite(v); require(v > 0,"Invalid fitted scale"); }
    for (double v : m.coefficients) finite(v);
    finite(m.training_rmse_bps); finite(m.validation.mae_bps); finite(m.validation.rmse_bps);
    finite(m.validation.no_change_mae_bps); finite(m.validation.no_change_rmse_bps);
    return m;
}
} // namespace btc::directional
