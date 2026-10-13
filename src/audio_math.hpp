#pragma once

#include <cstdint>

#if defined(__SSE2__)
#include <xmmintrin.h>
#endif

namespace crossfeed {

// PipeWire owns its callback thread. Restore its FP environment on return rather
// than depending on the application's linker flags or changing other clients.
class ScopedDenormals {
public:
    ScopedDenormals() noexcept {
#if defined(__SSE2__)
        previous_ = _mm_getcsr();
        _mm_setcsr(previous_ | (1u << 15) | (1u << 6)); // FTZ and DAZ
#elif defined(__aarch64__)
        __asm__ __volatile__("mrs %0, fpcr" : "=r"(previous_));
        uint64_t current = previous_ | (uint64_t{1} << 24);
        __asm__ __volatile__("msr fpcr, %0" : : "r"(current));
#endif
    }
    ~ScopedDenormals() noexcept {
#if defined(__SSE2__)
        _mm_setcsr(previous_);
#elif defined(__aarch64__)
        __asm__ __volatile__("msr fpcr, %0" : : "r"(previous_));
#endif
    }
    ScopedDenormals(const ScopedDenormals&) = delete;
    ScopedDenormals& operator=(const ScopedDenormals&) = delete;
private:
#if defined(__SSE2__)
    unsigned previous_;
#elif defined(__aarch64__)
    uint64_t previous_;
#endif
};

} // namespace crossfeed
