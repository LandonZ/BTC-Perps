// Live paper-trading loop for the naive basis model (model/kalshi-model.hpp).
//
// Once a minute it snapshots the Kalshi KXBTCPERP top of book and the Coinbase
// BTC-USD bid/ask, feeds the quote to NaiveBasisModel::on_quote, and tracks a
// paper position of one unit. It never places an order: every action is only
// printed and logged to CSV.
//
// The rolling-mean centre is warmed up from the last `lookback` 1m candles of
// both venues at startup (Kalshi bid/ask close, Coinbase close), so signals
// start on the first live bar instead of a day later.
//
// Paper PnL is the basis captured, in bps of spot: perp leg at the touch, spot
// hedge at mid, no fees or funding.
#include "../model/kalshi-model.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

const std::string KALSHI = "https://external-api.kalshi.com/trade-api/v2/margin";
const std::string TICKER = "KXBTCPERP";
const std::string CB_ADV = "https://api.coinbase.com/api/v3/brokerage/market/products/BTC-USD";
const std::string CB_EXCHANGE = "https://api.exchange.coinbase.com/products/BTC-USD";
constexpr double CONTRACT_BTC = 0.0001;  // Kalshi prices are USD per contract of 0.0001 BTC

volatile std::sig_atomic_t stop_requested = 0;

// ---------------------------------------------------------------- minimal JSON

struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    double number = 0;
    bool boolean = false;
    std::string string;
    std::vector<Json> array;
    std::vector<std::pair<std::string, Json>> object;

    const Json* find(const std::string& key) const {
        for (const auto& [k, v] : object)
            if (k == key) return &v;
        return nullptr;
    }
    const Json& operator[](const std::string& key) const {
        const Json* v = find(key);
        if (!v) throw std::runtime_error("JSON key missing: " + key);
        return *v;
    }
    // Both venues send prices as strings; accept either form.
    double as_double() const {
        if (type == Type::Number) return number;
        if (type == Type::String && !string.empty()) return std::stod(string);
        throw std::runtime_error("JSON value is not a number");
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : s_(text) {}
    Json parse() {
        Json v = value();
        skip_ws();
        if (i_ != s_.size()) fail("trailing characters");
        return v;
    }

private:
    const std::string& s_;
    std::size_t i_ = 0;

    [[noreturn]] void fail(const char* what) { throw std::runtime_error(std::string("JSON parse error: ") + what); }
    void skip_ws() { while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_; }
    char peek() { skip_ws(); if (i_ >= s_.size()) fail("unexpected end"); return s_[i_]; }
    void expect(char c) { if (peek() != c) fail("unexpected character"); ++i_; }

    Json value() {
        Json v;
        const char c = peek();
        if (c == '{') {
            v.type = Json::Type::Object; ++i_;
            if (peek() == '}') { ++i_; return v; }
            for (;;) {
                std::string key = str();
                expect(':');
                v.object.emplace_back(std::move(key), value());
                if (peek() == ',') { ++i_; continue; }
                expect('}');
                return v;
            }
        }
        if (c == '[') {
            v.type = Json::Type::Array; ++i_;
            if (peek() == ']') { ++i_; return v; }
            for (;;) {
                v.array.push_back(value());
                if (peek() == ',') { ++i_; continue; }
                expect(']');
                return v;
            }
        }
        if (c == '"') { v.type = Json::Type::String; v.string = str(); return v; }
        if (s_.compare(i_, 4, "true") == 0) { v.type = Json::Type::Bool; v.boolean = true; i_ += 4; return v; }
        if (s_.compare(i_, 5, "false") == 0) { v.type = Json::Type::Bool; i_ += 5; return v; }
        if (s_.compare(i_, 4, "null") == 0) { i_ += 4; return v; }
        std::size_t used = 0;
        v.type = Json::Type::Number;
        v.number = std::stod(s_.substr(i_, 64), &used);
        i_ += used;
        return v;
    }

    std::string str() {
        expect('"');
        std::string out;
        while (i_ < s_.size() && s_[i_] != '"') {
            char c = s_[i_++];
            if (c == '\\' && i_ < s_.size()) {
                c = s_[i_++];
                if (c == 'n') c = '\n';
                else if (c == 't') c = '\t';
                else if (c == 'u') { i_ += 4; c = '?'; }  // non-ASCII never matters here
            }
            out += c;
        }
        expect('"');
        return out;
    }
};

// ---------------------------------------------------------------- HTTP

std::size_t append_body(char* data, std::size_t size, std::size_t n, void* out) {
    static_cast<std::string*>(out)->append(data, size * n);
    return size * n;
}

Json http_get_json(const std::string& url, int retries = 4) {
    for (int attempt = 0;; ++attempt) {
        CURL* curl = curl_easy_init();
        if (!curl) throw std::runtime_error("curl_easy_init failed");
        std::string body;
        long status = 0;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "ucf-btc-perps/1.0");
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_body);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        const CURLcode rc = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_cleanup(curl);

        if (rc == CURLE_OK && status == 200) return JsonParser(body).parse();
        const bool retryable = rc != CURLE_OK || status == 429 || status >= 500;
        if (!retryable || attempt == retries - 1) {
            std::ostringstream msg;
            msg << "GET " << url << " -> " << (rc != CURLE_OK ? curl_easy_strerror(rc) : std::to_string(status))
                << ' ' << body.substr(0, 200);
            throw std::runtime_error(msg.str());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500 << attempt));
    }
}

// ---------------------------------------------------------------- market data

int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string utc(int64_t ts) {
    const std::time_t t = static_cast<std::time_t>(ts);
    std::tm tm{};
    gmtime_r(&t, &tm);
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return out.str();
}

// Kalshi fills empty candle fields with a huge sentinel; treat those as missing.
std::optional<double> kalshi_price(const Json& v) {
    if (v.type == Json::Type::Null || (v.type == Json::Type::String && v.string.empty())) return std::nullopt;
    const double p = v.as_double();
    if (p <= 0 || p > 1e6) return std::nullopt;
    return p / CONTRACT_BTC;
}

// One live bar: Kalshi top of book and Coinbase mid, both USD per BTC.
std::optional<kalshi::Quote> live_quote() {
    const Json book = http_get_json(KALSHI + "/markets/" + TICKER + "/orderbook?depth=1")["orderbook"];
    const Json* bids = book.find("bids");
    const Json* asks = book.find("asks");
    if (!bids || !asks || bids->array.empty() || asks->array.empty()) return std::nullopt;
    const Json ticker = http_get_json(CB_EXCHANGE + "/ticker");

    kalshi::Quote q;
    q.ts = now_seconds();
    q.bid = bids->array[0].array.at(0).as_double() / CONTRACT_BTC;
    q.ask = asks->array[0].array.at(0).as_double() / CONTRACT_BTC;
    q.spot = (ticker["bid"].as_double() + ticker["ask"].as_double()) / 2;
    if (q.bid <= 0 || q.ask <= q.bid || q.spot <= 0) return std::nullopt;
    return q;
}

// The last `bars` completed minutes where both venues have a candle, oldest first.
std::vector<kalshi::Quote> warmup_quotes(int bars) {
    const int64_t current_minute = now_seconds() / 60 * 60;
    // The window counts bars, not minutes, and Kalshi has multi-hour outages, so look back twice as far.
    const int64_t start = current_minute - 2 * static_cast<int64_t>(bars) * 60;

    std::map<int64_t, std::pair<double, double>> perp;  // candle start -> (bid, ask)
    for (int64_t t = start; t < current_minute; t += 4800 * 60) {
        const int64_t end = std::min<int64_t>(t + 4800 * 60, current_minute);
        const Json d = http_get_json(KALSHI + "/markets/" + TICKER + "/candlesticks?start_ts=" + std::to_string(t) +
                                     "&end_ts=" + std::to_string(end) + "&period_interval=1");
        for (const Json& c : d["candlesticks"].array) {
            const auto bid = kalshi_price(c["bid"]["close"]);
            const auto ask = kalshi_price(c["ask"]["close"]);
            // end_period_ts closes the minute; Coinbase keys candles by their start.
            if (bid && ask && *ask > *bid) perp[static_cast<int64_t>(c["end_period_ts"].as_double()) - 60] = {*bid, *ask};
        }
    }

    std::map<int64_t, double> spot;  // candle start -> close
    for (int64_t t = start; t < current_minute; t += 350 * 60) {
        const int64_t end = std::min<int64_t>(t + 349 * 60, current_minute - 60);
        const Json d = http_get_json(CB_ADV + "/candles?granularity=ONE_MINUTE&start=" + std::to_string(t) +
                                     "&end=" + std::to_string(end));
        for (const Json& c : d["candles"].array)
            spot[static_cast<int64_t>(c["start"].as_double())] = c["close"].as_double();
        std::this_thread::sleep_for(std::chrono::milliseconds(110));  // public limit is 10 req/s
    }

    std::vector<kalshi::Quote> out;
    for (const auto& [ts, ba] : perp) {
        const auto s = spot.find(ts);
        if (ts >= current_minute || s == spot.end()) continue;
        out.push_back({ts + 60, ba.first, ba.second, s->second});  // quote as of the candle close
    }
    if ((int)out.size() > bars) out.erase(out.begin(), out.end() - bars);
    return out;
}

// ---------------------------------------------------------------- loop

struct Settings {
    kalshi::ModelParams model;
    std::string log_dir = "live/logs";
    int64_t run_minutes = 0;  // 0 = until Ctrl-C
};

const char* action_name(kalshi::Action a) {
    return a == kalshi::Action::Buy ? "BUY" : a == kalshi::Action::Sell ? "SELL" : "hold";
}

void run(const Settings& s) {
    kalshi::NaiveBasisModel model(s.model);

    if (s.model.lookback > 0) {
        std::cerr << "Warming up the centre from the last " << s.model.lookback << " 1m candles...\n";
        const auto history = warmup_quotes(s.model.lookback);
        for (const auto& q : history) model.on_quote(q, 0);
        std::cerr << "  " << history.size() << " bars loaded";
        if (!model.ready()) std::cerr << " (gaps; " << s.model.lookback - (int)history.size() << " more live bars needed)";
        std::cerr << ", centre " << std::fixed << std::setprecision(2) << model.centre_bps() << " bps\n";
    }

    std::filesystem::create_directories(s.log_dir);
    const std::string path = s.log_dir + "/live_" + std::to_string(now_seconds()) + ".csv";
    std::ofstream log(path);
    log << "ts,utc,perp_bid,perp_ask,spot,basis_bid_bps,basis_ask_bps,basis_mid_bps,centre_bps,"
           "action,position,trade_pnl_bps,total_pnl_bps\n";
    std::cerr << "Logging to " << path << " (paper only, no orders). Ctrl-C to stop.\n";

    int position = 0;
    double entry_bps = 0, total_pnl = 0;
    int trades = 0;
    const int64_t stop_at = s.run_minutes > 0 ? now_seconds() + s.run_minutes * 60 : 0;

    while (!stop_requested && (stop_at == 0 || now_seconds() < stop_at)) {
        // Sleep to the next minute boundary so live bars line up with the 1m warm-up bars.
        const int64_t next = now_seconds() / 60 * 60 + 60;
        while (!stop_requested && now_seconds() < next) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (stop_requested) break;

        std::optional<kalshi::Quote> q;
        try {
            q = live_quote();
        } catch (const std::exception& e) {
            std::cerr << utc(now_seconds()) << "  fetch failed, skipping bar: " << e.what() << '\n';
            continue;
        }
        if (!q) {
            std::cerr << utc(now_seconds()) << "  empty book, skipping bar\n";
            continue;
        }

        const double centre = model.centre_bps();  // the centre on_quote decides against
        const kalshi::Action action = model.on_quote(*q, position);
        std::optional<double> trade_pnl;
        if (action == kalshi::Action::Buy) {
            if (position == 0) { position = 1; entry_bps = q->basis_ask(); }
            else { trade_pnl = entry_bps - q->basis_ask(); position = 0; }
        } else if (action == kalshi::Action::Sell) {
            if (position == 0) { position = -1; entry_bps = q->basis_bid(); }
            else { trade_pnl = q->basis_bid() - entry_bps; position = 0; }
        }
        if (trade_pnl) { total_pnl += *trade_pnl; ++trades; }

        std::ostringstream pnl;
        if (trade_pnl) pnl << std::fixed << std::setprecision(2) << *trade_pnl;
        log << std::fixed << std::setprecision(2) << q->ts << ',' << utc(q->ts) << ',' << q->bid << ',' << q->ask << ','
            << q->spot << ',' << std::setprecision(3) << q->basis_bid() << ',' << q->basis_ask() << ','
            << q->basis_mid() << ',' << centre << ',' << action_name(action) << ',' << position << ',' << pnl.str()
            << ',' << total_pnl << '\n'
            << std::flush;

        std::cout << utc(q->ts) << std::fixed << std::setprecision(2) << "  spot " << q->spot << "  perp "
                  << q->bid << '/' << q->ask << "  basis " << q->basis_bid() << '/' << q->basis_ask()
                  << "  centre " << centre << "  " << (model.ready() ? action_name(action) : "warming") << "  pos "
                  << position;
        if (trade_pnl) std::cout << "  closed " << *trade_pnl << " bps";
        std::cout << "  total " << total_pnl << " bps (" << trades << " trades)\n" << std::flush;
    }
    std::cerr << "\nStopped. " << trades << " closed paper trades, " << std::fixed << std::setprecision(2)
              << total_pnl << " bps; final position " << position << ". Log: " << path << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    Settings s;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--help" || option == "-h") {
                std::cout << "Usage: kalshi-live [options]   (paper trading only; places no orders)\n"
                             "  --lookback N      bars (minutes) in the rolling-mean centre, 0 = fixed centre (default 1440)\n"
                             "  --centre-bps X    fixed centre when --lookback 0 (default 0)\n"
                             "  --edge-bps X      enter when the hittable basis is X bps through the centre (default 3)\n"
                             "  --exit-bps X      exit when the basis is back within X bps of the centre (default 0)\n"
                             "  --minutes N       stop after N minutes (default: run until Ctrl-C)\n"
                             "  --log-dir DIR     where the CSV log goes (default live/logs)\n";
                return 0;
            }
            if (i + 1 >= argc) throw std::runtime_error("Missing value for " + option);
            const std::string value = argv[++i];
            if (option == "--lookback") s.model.lookback = std::stoi(value);
            else if (option == "--centre-bps") s.model.centre_bps = std::stod(value);
            else if (option == "--edge-bps") s.model.edge_bps = std::stod(value);
            else if (option == "--exit-bps") s.model.exit_bps = std::stod(value);
            else if (option == "--minutes") s.run_minutes = std::stoll(value);
            else if (option == "--log-dir") s.log_dir = value;
            else throw std::runtime_error("Unknown option " + option + " (see --help)");
        }
        if (s.model.lookback < 0 || s.model.edge_bps < 0 || s.model.exit_bps < 0)
            throw std::runtime_error("--lookback, --edge-bps and --exit-bps must be non-negative");

        std::signal(SIGINT, [](int) { stop_requested = 1; });
        curl_global_init(CURL_GLOBAL_DEFAULT);
        run(s);
        curl_global_cleanup();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }
}
