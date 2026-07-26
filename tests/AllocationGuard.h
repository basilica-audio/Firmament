#pragma once

#include <cstdlib>
#include <new>

// A thread-scoped heap-allocation counter for the audio thread's
// real-time-safety contract ("no allocation once prepare() has completed" -
// docs/architecture.md, CLAUDE.md). Overrides the global operator new/delete
// to count allocations, so a test can assert that a processBlock() call (or
// any other audio-thread path) performs zero heap operations.
//
// Ported from the suite's AllocationGuard pattern (sibling
// basilica-audio/miserere, tests/AllocationGuard.h) per the v0.3.0 binding
// brief, section 6.11 - firmament had no allocation guard before v0.3.0.
//
// Usage:
//
//   AllocationGuard::reset();
//   engine.process (block);
//   CHECK (AllocationGuard::allocationCount() == 0);
//
// Scope discipline (brief 6.11): the guard is scoped to the render calls -
// message-thread work (parameter changes, kernel recomputes via
// Convolution::loadImpulseResponse) is expected to allocate and must happen
// OUTSIDE the reset()/allocationCount() bracket.
//
// Thread scoping (brief 6.11: "zero heap allocations on the audio thread;
// message-thread kernel loads ... exempt (guard scoped to the render
// call)"): the counter is thread_local, so reset()/allocationCount() observe
// exactly the allocations made by the calling thread - in these tests, the
// Catch2 thread that plays the audio-thread role by invoking
// processBlock()/process() synchronously. Allocations by other threads
// (juce::Timer's callback poster, juce::dsp::Convolution's background IR
// loader - both legitimate, non-audio-thread work that runs concurrently by
// design) are deliberately NOT counted: they land on their own threads'
// counters, which no test reads. This is what makes the guard deterministic
// - a process-wide counter would intermittently pick up the background
// loader finishing an exempt, message-thread-initiated IR build.
namespace AllocationGuard
{
    inline thread_local std::size_t threadLocalCount = 0;

    inline void reset() noexcept { threadLocalCount = 0; }
    inline std::size_t allocationCount() noexcept { return threadLocalCount; }
}

// NOTE: global replacement operator new/delete must not be declared `inline`
// (the standard forbids it for replacement functions) - safe here because
// this header is deliberately included by exactly one translation unit
// (AllocationGuardTests.cpp), so there is no ODR concern despite the
// definitions living in a header.
void* operator new (std::size_t size)
{
    ++AllocationGuard::threadLocalCount;
    if (auto* ptr = std::malloc (size == 0 ? 1 : size))
        return ptr;
    throw std::bad_alloc();
}

void operator delete (void* ptr) noexcept { std::free (ptr); }
void operator delete (void* ptr, std::size_t) noexcept { std::free (ptr); }

void* operator new[] (std::size_t size)
{
    ++AllocationGuard::threadLocalCount;
    if (auto* ptr = std::malloc (size == 0 ? 1 : size))
        return ptr;
    throw std::bad_alloc();
}

void operator delete[] (void* ptr) noexcept { std::free (ptr); }
void operator delete[] (void* ptr, std::size_t) noexcept { std::free (ptr); }
