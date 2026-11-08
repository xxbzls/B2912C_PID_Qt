#pragma once
#include <array>
#include <cmath>
#include <cstddef>

// Shared by the displayed curves and 1 Hz files. A timestamp exactly at n+1
// belongs to the next window. No empty windows or unfinished tail are emitted.
template<std::size_t Columns> class SecondAverage {
public:
    struct Point {double end=0;std::array<double,Columns> values{};};
    bool append(double seconds,const std::array<double,Columns>& values,Point& completed) {
        const double second=std::floor(seconds);
        const bool ready=count>0 && second!=window;
        if(ready) {
            completed.end=window+1;
            for(std::size_t i=0;i<Columns;++i)completed.values[i]=sums[i]/count;
            sums.fill(0);count=0;
        }
        window=second;
        for(std::size_t i=0;i<Columns;++i)sums[i]+=values[i];
        ++count;
        return ready;
    }
private:
    double window=0;
    std::size_t count=0;
    std::array<double,Columns> sums{};
};
