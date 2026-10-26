#pragma once
#include <cstdint>

namespace longerdays
{
    // Measures the game's vanilla clock speed (in-game seconds per real
    // second) instead of trusting a hard-coded number, so [RealTime] targets
    // stay accurate across patches.
    //
    // Fed with two running totals: the clock-tick time the game was actually
    // given (after our slow-down), and the in-game clock in seconds. Their
    // ratio over a window is the vanilla rate. Sleeping, waiting, loading a
    // save or another mod moving the clock shows up as a jump and simply
    // restarts the window.
    class RateMeter
    {
    public:
        explicit RateMeter(float assumedRate) : m_rate(assumedRate) {}

        void Feed(double appliedSeconds, int64_t clockSeconds);

        float Rate() const       { return m_rate; }
        bool  Calibrated() const { return m_calibrated; }

        // In-game seconds a window must span before it yields a measurement.
        // Long enough that the clock's 1-second resolution is noise (<0.2%).
        static constexpr int64_t kWindowGameSeconds = 600;

    private:
        void Restart(double applied, int64_t clock);

        static constexpr double kMaxRate = 200.0;

        float   m_rate;
        bool    m_calibrated = false;
        bool    m_started    = false;
        double  m_startApplied = 0, m_lastApplied = 0;
        int64_t m_startClock   = 0, m_lastClock   = 0;
    };
}
