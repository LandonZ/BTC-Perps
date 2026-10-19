#include "events.h"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>

namespace btc::events {
namespace {
double number(const std::string& s) {
    std::size_t used = 0;
    const double value = std::stod(s,&used);
    if (used != s.size() || !std::isfinite(value)) throw std::runtime_error("Invalid event number: "+s);
    return value;
}
std::vector<std::string> cells(const std::string& line) {
    std::vector<std::string> out(1);
    for (char ch : line) { if (ch == ',') out.emplace_back(); else if (ch != '\r') out.back() += ch; }
    return out;
}
}
// Gzipped replays (*.gz) stream through `gzip -dc`, so committed recordings need no unpacking.
class GzipStream {
public:
    explicit GzipStream(const std::string& path) : pipe_(popen(("gzip -dc '"+path+"'").c_str(),"r"),pclose) {
        if (!pipe_) throw std::runtime_error("Cannot run gzip for "+path);
    }
    bool getline(std::string& line) {
        line.clear();
        for (int ch; (ch = std::fgetc(pipe_.get())) != EOF;) {
            if (ch == '\n') return true;
            line += static_cast<char>(ch);
        }
        return !line.empty();
    }
private:
    std::unique_ptr<FILE,int(*)(FILE*)> pipe_;
};
void read(const std::string& path, const std::function<void(const Event&,bool)>& feed) {
    const bool gz = path.size() > 3 && path.compare(path.size()-3,3,".gz") == 0;
    std::ifstream file;
    std::optional<GzipStream> zipped;
    if (gz) zipped.emplace(path); else file.open(path);
    if (!gz && !file) throw std::runtime_error("Cannot read events: "+path+"; run tools/download_binance_maker.py first");
    auto next_line = [&](std::string& out) { return gz ? zipped->getline(out) : static_cast<bool>(std::getline(file,out)); };
    std::string line;
    next_line(line);
    if (line.find("time_ms,kind,price,quantity,bid,ask,buyer_maker,funding_rate,interval_hours") != 0)
        throw std::runtime_error("Unexpected Binance replay header");
    std::optional<Event> previous;
    while (next_line(line)) {
        if (line.empty()) continue;
        const auto c = cells(line);
        if (c.size() != 9 || c[1].size() != 1) throw std::runtime_error("Malformed replay row");
        Event e;
        std::size_t used = 0;
        e.time = std::stoll(c[0],&used);
        if (used != c[0].size()) throw std::runtime_error("Invalid replay timestamp");
        e.kind = c[1][0];
        if (!c[2].empty()) e.price = number(c[2]);
        if (!c[3].empty()) e.quantity = number(c[3]);
        if (!c[4].empty()) e.bid = number(c[4]);
        if (!c[5].empty()) e.ask = number(c[5]);
        if (e.kind == 'T' && c[6] != "true" && c[6] != "false") throw std::runtime_error("Trade must supply buyer_maker");
        e.buyer_maker = c[6] == "true";
        if (!c[7].empty()) e.funding_rate = number(c[7]);
        if (!c[8].empty()) e.interval_hours = number(c[8]);
        if ((e.kind == 'F' || e.kind == 'R') && (c[7].empty() || c[8].empty()))
            throw std::runtime_error("Funding event must supply a rate and interval");
        if (previous) feed(*previous,e.time != previous->time);
        previous = e;
    }
    if (!previous) throw std::runtime_error("Replay contains no events");
    feed(*previous,true);
}
} // namespace btc::events
