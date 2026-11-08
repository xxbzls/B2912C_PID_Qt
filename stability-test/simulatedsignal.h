#pragma once
#include <random>
#include <algorithm>
#include <cmath>

// Synthetic open-loop noise for rendering/storage load tests, never a plant/PID model.
class SimulatedSignal {
    std::mt19937 generator{20261028};
    std::normal_distribution<double> normal{0,1};
    long long lastSecond=-1;
    double secondNoise=0;
public:
    struct Values { double probeV,outputMA,measuredMA; };
    Values next(double seconds,double targetV) {
        const auto second=static_cast<long long>(seconds);
        if(second!=lastSecond) {
            lastSecond=second;
            secondNoise=.0006*std::clamp(normal(generator),-3.0,3.0);
        }
        // The correlated one-second component remains visible after actual one-second averaging.
        const double voltage=targetV+secondNoise+.00025*normal(generator)
            +.00010*std::sin(seconds/1500.0)+.0012*std::exp(-seconds/5.0);
        const double current=.24-.015*(1-std::exp(-seconds/12000.0))+.00015*normal(generator);
        return {voltage,current,current+.00004*normal(generator)};
    }
};
