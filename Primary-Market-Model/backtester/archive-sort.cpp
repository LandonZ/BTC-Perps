// Stable external sort of public archive rows, retaining only 32 bytes per row.
#include "archive-format.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <queue>
#include <thread>
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

using btc::archive::Record;
namespace {
class OutputBuffer:public std::streambuf {
    char data_[1024*1024];
public:
    OutputBuffer(){setp(data_,data_+sizeof(data_));}
    int sync()override {
        auto remaining=pptr()-pbase();char* cursor=pbase();
        while(remaining) {
            const auto written=write(STDOUT_FILENO,cursor,static_cast<std::size_t>(remaining));
            if(written<0 && errno==EINTR)continue;
            if(written<=0)return -1;
            remaining-=written;cursor+=written;
        }
        setp(data_,data_+sizeof(data_));return 0;
    }
    int_type overflow(int_type value)override {
        if(sync()!=0)return traits_type::eof();
        if(!traits_type::eq_int_type(value,traits_type::eof())){*pptr()=traits_type::to_char_type(value);pbump(1);}
        return traits_type::not_eof(value);
    }
};
void merge(const std::vector<std::string>& paths,std::ostream& output) {
    struct Head{Record row;std::size_t source;};
    struct Later{bool operator()(const Head& a,const Head& b)const{return btc::archive::before(b.row,a.row);}};
    std::vector<std::ifstream> inputs;
    std::priority_queue<Head,std::vector<Head>,Later> queue;
    for(std::size_t i=0;i<paths.size();++i) {
        inputs.emplace_back(paths[i],std::ios::binary);Record row;
        if(inputs.back().read(reinterpret_cast<char*>(&row),sizeof(row)))queue.push({row,i});
        else throw std::runtime_error("Cannot read archive sort run");
    }
    while(!queue.empty()) {
        auto head=queue.top();queue.pop();
        output.write(reinterpret_cast<const char*>(&head.row),sizeof(head.row));
        if(!output)throw std::runtime_error("Sorted archive output failed");
        if(inputs[head.source].read(reinterpret_cast<char*>(&head.row),sizeof(head.row)))queue.push(head);
        else if(!inputs[head.source].eof() || inputs[head.source].gcount())throw std::runtime_error("Truncated archive sort run");
    }
}
}
int main(int argc,char** argv) {
    int slot=-1;
    try {
        if ((argc!=4 && argc!=5) || std::string(argv[1]).size()!=1) throw std::runtime_error("Usage: archive-sort KIND RUN_DIRECTORY SLOT_DIRECTORY [CHUNK_ROWS]");
        const std::size_t chunk_rows=argc==5?std::stoull(argv[4]):4000000;
        if(!chunk_rows)throw std::runtime_error("Archive sort chunk size must be positive");
        const char kind=argv[1][0];
        if (kind!='Q' && kind!='S' && kind!='T') throw std::runtime_error("Invalid archive kind");
        std::filesystem::create_directories(argv[2]);
        while (slot<0) {
            for (int i=0;i<2 && slot<0;++i) {
                const auto path=std::string(argv[3])+"/sort-slot-"+std::to_string(i);
                int file=open(path.c_str(),O_CREAT|O_RDWR,0600);
                if(file<0) throw std::runtime_error("Cannot open archive sort slot");
                if(flock(file,LOCK_EX|LOCK_NB)==0) slot=file;else close(file);
            }
            if(slot<0)std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::ios::sync_with_stdio(false);std::cin.tie(nullptr);
        std::ifstream input("/dev/fd/0",std::ios::binary);
        OutputBuffer output_buffer;std::ostream output(&output_buffer);
        if(!input || !output)throw std::runtime_error("Cannot open archive sort streams");
        btc::archive::Parser parser(kind);
        std::vector<Record> buffer;buffer.reserve(chunk_rows);
        std::vector<std::string> paths;
        std::string line;line.reserve(256);
        std::uint64_t count=0;bool ordered=true;
        auto flush=[&] {
            if(buffer.empty())return;
            if(!ordered)std::sort(buffer.begin(),buffer.end(),btc::archive::before);
            const auto path=std::string(argv[2])+"/run-"+std::to_string(paths.size())+".bin";
            std::ofstream file(path,std::ios::binary);
            file.write(reinterpret_cast<const char*>(buffer.data()),static_cast<std::streamsize>(buffer.size()*sizeof(Record)));
            if(!file)throw std::runtime_error("Archive sort temporary write failed");
            paths.push_back(path);buffer.clear();ordered=true;
            if(paths.size()%8==0)std::cerr<<"Archive sort "<<argv[2]<<" rows="<<count<<'\n';
        };
        while(std::getline(input,line)) {
            Record row;if(!parser.parse(line,row))continue;
            if(!buffer.empty() && btc::archive::before(row,buffer.back()))ordered=false;
            buffer.push_back(row);++count;
            if(buffer.size()==chunk_rows)flush();
        }
        if(!input.eof())throw std::runtime_error("Archive decompression stream failed");
        flush();
        // Release the sorting slot BEFORE output can block on a future month.
        flock(slot,LOCK_UN);close(slot);slot=-1;
        buffer.clear();buffer.shrink_to_fit();
        std::size_t generation=0;
        while(paths.size()>64) {
            std::vector<std::string> next;
            for(std::size_t i=0;i<paths.size();i+=64) {
                std::vector<std::string> group(paths.begin()+i,paths.begin()+std::min(i+64,paths.size()));
                const auto path=std::string(argv[2])+"/merge-"+std::to_string(generation)+"-"+std::to_string(i)+".bin";
                {std::ofstream output(path,std::ios::binary);merge(group,output);output.close();if(!output)throw std::runtime_error("Archive merge write failed");}
                next.push_back(path);
                for(const auto& old:group)std::filesystem::remove(old);
            }
            paths=std::move(next);++generation;
        }
        output.write(btc::archive::magic,sizeof(btc::archive::magic)-1);
        merge(paths,output);
        output.flush();if(!output)throw std::runtime_error("Sorted archive output failed");
        for(const auto& path:paths)std::filesystem::remove(path);
        std::cerr<<"Archive sort completed "<<argv[2]<<" rows="<<count<<'\n';
    } catch(const std::exception& error) {
        if(slot>=0){flock(slot,LOCK_UN);close(slot);}
        std::cerr<<"archive sort error: "<<error.what()<<'\n';return 1;
    }
}
