#include "dsp.hpp"
#include <array>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <new>
#include <pthread.h>

static thread_local bool in_audio = false;
static size_t allocations = 0, locks = 0;

void* operator new(std::size_t size) {
    if (in_audio) ++allocations;
    if (void* p = std::malloc(size == 0 ? 1 : size)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

extern "C" int __real_pthread_mutex_lock(pthread_mutex_t* mutex);
extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex) {
    if (in_audio) ++locks;
    return __real_pthread_mutex_lock(mutex);
}

int main() {
    crossfeed::CrossfeedDSP dsp;
    std::array<float, 512> input{}, output{};
    input.fill(.2f);
    for (int block = 0; block < 1000; ++block) {
        if (block % 20 == 0) dsp.set_delay_us(block % 40 ? 800.f : 0.f);
        if (block % 100 == 0) dsp.set_enabled(block % 200 == 0);
        if (block % 10 == 1) dsp.service_sample_rate();
        in_audio = true;
        if (block % 100 == 0) dsp.request_sample_rate(block % 200 ? 96000 : 48000);
        if (block % 3 == 0) dsp.reset();
        if (block % 2) dsp.process_interleaved(input.data(), output.data(), 256);
        else dsp.process_planar(input.data(), input.data() + 256, output.data(), output.data() + 256, 256);
        in_audio = false;
    }
    assert(allocations == 0 && locks == 0);
    std::cout << "PASS: 1000 audio blocks, including updates/rate fallback/reset, used zero C++ allocations and zero pthread mutex locks.\n";
}
