#include "lru.hh"
#include <chrono>
#include <iostream>

using namespace std;
using namespace std::chrono;

int main() {
    auto always_true = [](auto) { return true; };
    auto now = std::chrono::steady_clock::now;

    lru_cache<uintptr_t, uintptr_t, always_true> cache(128);
    
    auto start = now(); 
    auto limits = 1u << 26;
    for (int i = 0; i < limits; ++i) {
        auto p = uintptr_t(i << 6);
        cache.insert(p, p, true);
        cache.get(p);
    };

    auto d = duration_cast<microseconds>(now() - start).count(); 
    cout << limits * 1000ul / d  << " kops" << endl;
    return 0;
}
