#pragma once
#include "events.h"
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <string_view>

namespace btc::archive {
struct Record { events::Millis time; double first,second; std::uint64_t sequence; };
static_assert(sizeof(Record)==32,"Unexpected local archive record layout");
inline constexpr char magic[]="BASIS-ARCHIVE-1\n";
class Parser {
    char kind_;
    std::uint64_t sequence_ = 0;
    std::string bid_text_,ask_text_;
    double bid_ = 0,ask_ = 0;
    static double decimal(std::string_view field) {
        char* end=nullptr; const double value=std::strtod(field.data(),&end);
        if (end!=field.data()+field.size() || !std::isfinite(value) || value<=0)
            throw std::runtime_error("Invalid positive archive number");
        return value;
    }
public:
    explicit Parser(char kind):kind_(kind) {}
    bool parse(std::string& line,Record& record) {
        if (line.empty()) return false;
        if (line[0]<'0' || line[0]>'9') {
            if (sequence_) throw std::runtime_error("Unexpected nonnumeric archive row");
            return false;
        }
        if (line.back()=='\r') line.pop_back();
        std::string_view fields[16]; std::size_t begin=0,n=0;
        for (std::size_t i=0;i<=line.size();++i) if (i==line.size() || line[i]==',') {
            if (n==16) throw std::runtime_error("Too many archive fields");
            fields[n++]=std::string_view(line).substr(begin,i-begin);begin=i+1;
        }
        if (n<7) throw std::runtime_error("Missing archive fields");
        const auto result=std::from_chars(fields[5].data(),fields[5].data()+fields[5].size(),record.time);
        if (result.ec!=std::errc() || result.ptr!=fields[5].data()+fields[5].size())
            throw std::runtime_error("Invalid archive timestamp");
        if (record.time>100000000000000LL) record.time/=1000;
        record.sequence=sequence_++;record.second=0;
        if (kind_=='Q') {
            if (fields[1]!=bid_text_) { bid_=decimal(fields[1]);bid_text_=fields[1]; }
            if (fields[3]!=ask_text_) { ask_=decimal(fields[3]);ask_text_=fields[3]; }
            record.first=bid_;record.second=ask_;
            if (ask_<bid_) throw std::runtime_error("Crossed archive quote");
        } else {
            record.first=decimal(fields[1]);
            if (kind_=='T') {
                if (fields[6]!="true" && fields[6]!="false" && fields[6]!="True" && fields[6]!="False")
                    throw std::runtime_error("Invalid archive aggressor side");
                record.second=decimal(fields[2])*(fields[6]=="true" || fields[6]=="True" ? 1 : -1);
            }
        }
        return true;
    }
};
inline events::Event event(char kind,const Record& row) {
    events::Event result;result.kind=kind;result.time=row.time;
    if (kind=='Q') { result.bid=row.first;result.ask=row.second; }
    else { result.price=row.first;if(kind=='T'){result.quantity=std::abs(row.second);result.buyer_maker=row.second>0;} }
    return result;
}
inline bool before(const Record& a,const Record& b) {
    return a.time!=b.time ? a.time<b.time : a.sequence<b.sequence;
}
}
