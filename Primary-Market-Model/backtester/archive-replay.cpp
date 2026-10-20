// Replay decompressed monthly archive streams with checked, bounded reordering.
// Input file descriptors are provided by the Python runner; this has no network API.
#include "basis-maker.h"
#include "archive-format.h"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string_view>

using btc::events::Event;
using btc::events::Millis;
namespace {
struct Item { Event event; std::uint64_t sequence; };
struct Later {
    bool operator()(const Item& a,const Item& b) const {
        return a.event.time != b.event.time ? a.event.time > b.event.time : a.sequence > b.sequence;
    }
};
struct Daily { std::uint64_t count = 0; Millis first = -1,last = -1,gap = 0; };
class Source {
    std::ifstream input_;
    std::string line_;
    std::priority_queue<Item,std::vector<Item>,Later> queue_;
    Millis maximum_ = -1,last_ = -1,start_,end_,window_;
    std::uint64_t sequence_ = 0;
    bool ended_ = false;
    bool binary_ = false,header_read_ = false;
    std::string previous_bid_,previous_ask_;
    double bid_ = 0,ask_ = 0;
    double decimal(std::string_view field) const {
        char* end = nullptr;
        const double value = std::strtod(field.data(),&end);
        if (end != field.data()+field.size() || !std::isfinite(value) || value <= 0)
            throw std::runtime_error("Invalid positive archive number");
        return value;
    }
    void read_one() {
        if (binary_) {
            if(!header_read_) {
                char header[sizeof(btc::archive::magic)-1];
                input_.read(header,sizeof(header));
                if(!input_ || std::memcmp(header,btc::archive::magic,sizeof(header)))throw std::runtime_error("Invalid sorted archive header");
                header_read_=true;
            }
            btc::archive::Record row;
            input_.read(reinterpret_cast<char*>(&row),sizeof(row));
            if(!input_) {
                if(!input_.eof() || input_.gcount())throw std::runtime_error("Truncated sorted archive record");
                ended_=true;return;
            }
            Event e=btc::archive::event(kind,row);
            if(e.time<start_ || e.time>=end_){++excluded;return;}
            maximum_=std::max(maximum_,e.time);queue_.push({e,row.sequence});return;
        }
        if (!std::getline(input_,line_)) {
            if (!input_.eof()) throw std::runtime_error("Archive stream read failed");
            ended_ = true; return;
        }
        if (line_.empty()) return;
        if (line_[0] < '0' || line_[0] > '9') {
            if (sequence_) throw std::runtime_error("Unexpected nonnumeric archive row");
            return; // Binance CSV header.
        }
        if (line_.back() == '\r') line_.pop_back();
        std::string_view fields[16];
        std::size_t begin = 0,n = 0;
        for (std::size_t i = 0;i <= line_.size();++i) if (i == line_.size() || line_[i] == ',') {
            if (n == 16) throw std::runtime_error("Too many archive fields");
            fields[n++] = std::string_view(line_).substr(begin,i-begin); begin = i+1;
        }
        if (n < 7) throw std::runtime_error("Missing archive fields");
        Event e; e.kind = kind;
        const auto parsed = std::from_chars(fields[5].data(),fields[5].data()+fields[5].size(),e.time);
        if (parsed.ec != std::errc() || parsed.ptr != fields[5].data()+fields[5].size())
            throw std::runtime_error("Invalid archive timestamp");
        if (e.time > 100000000000000LL) e.time /= 1000;
        ++sequence_;
        if (e.time < start_ || e.time >= end_) { ++excluded; return; }
        if (kind == 'Q') {
            if (fields[1] != previous_bid_) { bid_ = decimal(fields[1]); previous_bid_ = fields[1]; }
            if (fields[3] != previous_ask_) { ask_ = decimal(fields[3]); previous_ask_ = fields[3]; }
            e.bid = bid_; e.ask = ask_;
            if (e.ask < e.bid) throw std::runtime_error("Crossed archive quote");
        } else {
            e.price = decimal(fields[1]);
            if (kind == 'T') {
                e.quantity = decimal(fields[2]);
                if (fields[6] != "true" && fields[6] != "false" && fields[6] != "True" && fields[6] != "False")
                    throw std::runtime_error("Invalid archive aggressor side");
                e.buyer_maker = fields[6] == "true" || fields[6] == "True";
            }
        }
        maximum_ = std::max(maximum_,e.time); queue_.push({e,sequence_});
    }
public:
    char kind;
    Event head;
    bool available = false;
    bool initialized = false;
    std::uint64_t excluded = 0;
    std::map<Millis,Daily> daily;
    Source(char type,const std::string& path,Millis start,Millis end,Millis window,bool binary=false)
        :input_(path),start_(start),end_(end),window_(window),binary_(binary),kind(type) {
        if (!input_) throw std::runtime_error("Cannot open archive pipe: "+path);
        line_.reserve(256);
        // A future month's decompressor may still be waiting for its download.
        // Its lower-bound timestamp lets earlier months run without blocking.
        head.time = start; head.kind = kind; available = true;
    }
    void advance() {
        while (!ended_ && (queue_.empty() || (!binary_ && maximum_-queue_.top().event.time <= window_))) read_one();
        available = !queue_.empty();
        if (!available) return;
        head = queue_.top().event; queue_.pop();
        if (head.time < last_)
            throw std::runtime_error("Archive disorder exceeds reorder window; no result published. Increase --reorder-ms and retry.");
        last_ = head.time;
        auto& day = daily[head.time/86400000];
        if (day.first < 0) day.first = head.time;
        if (day.last >= 0) day.gap = std::max(day.gap,head.time-day.last);
        day.last = head.time; ++day.count;
    }
};
int priority(char kind) {
    if (kind == 'F' || kind == 'R') return 0;
    if (kind == 'P') return 1;
    if (kind == 'S') return 2;
    if (kind == 'Q') return 3;
    return 4;
}
}
int main(int argc,char** argv) {
    try {
        if (argc < 4) throw std::runtime_error("Usage: archive-replay SOURCES_TSV FUNDING_AND_PREMIUM_CSV OUTPUT_DIR [REORDER_MS] [--equilibrium-bps VALUE] [--entry-offset-bps VALUE]");
        Millis window = 1000;
        btc::basis_maker::Config config; config.retain_decisions = false; config.retain_fills = false;
        for (int i=4;i<argc;++i) {
            const std::string key = argv[i];
            if (i==4 && key.rfind("--",0)!=0) {
                std::size_t used=0; window=std::stoll(key,&used);
                if (used!=key.size()) throw std::runtime_error("Invalid reorder window");
                continue;
            }
            if (key!="--equilibrium-bps" && key!="--entry-offset-bps" && key!="--perp-maker-bps" &&
                key!="--perp-taker-bps" && key!="--spot-taker-bps" && key!="--one-order-per-entry-event" &&
                key!="--single-basis-position") throw std::runtime_error("Unknown option: "+key);
            if (++i>=argc) throw std::runtime_error("Missing value for "+key);
            const std::string value=argv[i]; std::size_t used=0; const double number=std::stod(value,&used);
            if (used!=value.size() || !std::isfinite(number)) throw std::runtime_error("Invalid value for "+key);
            if (key=="--equilibrium-bps") config.equilibrium_bps=number;
            else if (key=="--entry-offset-bps") config.entry_offset_bps=number;
            else if (key=="--perp-maker-bps") config.perp_maker_bps=number;
            else if (key=="--perp-taker-bps") config.perp_taker_bps=number;
            else if (key=="--one-order-per-entry-event") {
                if (number!=0 && number!=1) throw std::runtime_error("Entry event mode must be 0 or 1");
                config.one_order_per_entry_event=number==1;
            }
            else if (key=="--single-basis-position") {
                if (number!=0 && number!=1) throw std::runtime_error("Single basis position mode must be 0 or 1");
                config.single_basis_position=number==1;
            }
            else config.spot_taker_bps=number;
        }
        if (window < 0) throw std::runtime_error("Negative reorder window");
        std::ifstream manifest(argv[1]);
        std::vector<std::unique_ptr<Source>> sources;
        std::string line;
        Millis first = std::numeric_limits<Millis>::max(),end = -1;
        while (std::getline(manifest,line)) {
            std::istringstream row(line); char kind; Millis start,stop; std::string path,format;
            if (!(row >> kind >> start >> stop >> path) || (kind != 'Q' && kind != 'T' && kind != 'S'))
                throw std::runtime_error("Malformed archive source manifest");
            first = std::min(first,start); end = std::max(end,stop);
            row >> format;
            if(!format.empty() && format!="BINARY")throw std::runtime_error("Unexpected archive source format");
            sources.push_back(std::make_unique<Source>(kind,path,start,stop,window,format=="BINARY"));
        }
        if (sources.empty()) throw std::runtime_error("No archive sources");
        std::vector<Event> small;
        btc::events::read(argv[2],[&](const Event& event,bool) {
            if (event.kind != 'P' && event.kind != 'F' && event.kind != 'R')
                throw std::runtime_error("Expected only funding and premium events");
            if ((event.time >= first || event.kind == 'R') && event.time < end) small.push_back(event);
        });
        std::stable_sort(small.begin(),small.end(),[](const Event& a,const Event& b) {
            return a.time != b.time ? a.time < b.time : priority(a.kind) < priority(b.kind);
        });
        btc::basis_maker::Engine engine(config);
        std::size_t index = 0;
        Millis previous_time = -1,progress = -1;
        const auto started = std::chrono::steady_clock::now();
        std::uint64_t count = 0;
        while (true) {
            Source* chosen = nullptr;
            for (auto& source : sources) if (source->available && (!chosen ||
                    source->head.time < chosen->head.time || (source->head.time == chosen->head.time &&
                    priority(source->kind) < priority(chosen->kind)))) chosen = source.get();
            const bool use_small = index < small.size() && (!chosen || small[index].time < chosen->head.time ||
                (small[index].time == chosen->head.time && priority(small[index].kind) < priority(chosen->kind)));
            if (!chosen && !use_small) break;
            if (!use_small && !chosen->initialized) { chosen->initialized = true; chosen->advance(); continue; }
            const Event event = use_small ? small[index++] : chosen->head;
            if (!use_small) chosen->advance();
            if (event.time < previous_time) throw std::runtime_error("Merged archive clock moved backwards");
            if (previous_time >= first && event.time/86400000 != previous_time/86400000) engine.mark_equity();
            previous_time = event.time; ++count;
            // Retain exact settlement inventory after any exits that occurred
            // later than the first sampled mark of the blackout minute.
            if (event.kind == 'F' && !engine.equity().empty()) engine.mark_equity();
            engine.on_event(event);
            if (event.kind == 'F') engine.mark_equity();
            if (event.time/86400000 != progress) {
                progress = event.time/86400000;
                const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-started).count();
                std::cerr << "Replay UTC day=" << progress << " events=" << count << " elapsed_s=" << seconds
                          << " net=" << engine.totals().net() << '\n';
            }
        }
        engine.finish();
        // A successful archive checksum is not sufficient: require every
        // requested day in every market stream before publishing the result.
        std::map<std::pair<Millis,char>,Daily> days;
        for (const auto& source : sources) for (const auto& item : source->daily) {
            auto& day = days[{item.first,source->kind}];
            if (day.count) throw std::runtime_error("Overlapping archive sources");
            day = item.second;
        }
        for (Millis day = first/86400000;day < end/86400000;++day) for (char kind : {'S','Q','T'})
            if (!days[{day,kind}].count) throw std::runtime_error("Missing full market day in archive inputs");
        btc::basis_maker::write_results(engine,argv[3]);
        std::ofstream coverage(std::string(argv[3])+"/coverage.csv");
        coverage << "utc_day,kind,count,first_ms,last_ms,max_gap_ms\n";
        for (const auto& item : days) coverage << item.first.first << ',' << item.first.second << ',' << item.second.count
            << ',' << item.second.first << ',' << item.second.last << ',' << item.second.gap << '\n';
        std::cout << "Completed archive replay: " << count << " source events; net=" << engine.totals().net() << '\n';
    } catch (const std::exception& error) {
        std::cerr << "archive replay error: " << error.what() << '\n'; return 1;
    }
}
