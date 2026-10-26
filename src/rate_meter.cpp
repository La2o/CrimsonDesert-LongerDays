#include "rate_meter.h"

namespace longerdays
{
    void RateMeter::Restart(double applied, int64_t clock)
    {
        m_startApplied = m_lastApplied = applied;
        m_startClock   = m_lastClock   = clock;
        m_started = true;
    }

    void RateMeter::Feed(double applied, int64_t clock)
    {
        if (!m_started)
        {
            Restart(applied, clock);
            return;
        }

        const double  dApplied = applied - m_lastApplied;
        const int64_t dClock   = clock - m_lastClock;
        // More clock movement than any plausible speed explains (+2 s for the
        // clock's whole-second resolution) is a skip, not time passing.
        if (dApplied < 0.0 || dClock < 0 || static_cast<double>(dClock) > dApplied * kMaxRate + 2.0)
        {
            Restart(applied, clock);
            return;
        }
        m_lastApplied = applied;
        m_lastClock   = clock;

        const int64_t winClock   = clock - m_startClock;
        const double  winApplied = applied - m_startApplied;
        if (winClock < kWindowGameSeconds || winApplied <= 0.0) return;

        const double rate = static_cast<double>(winClock) / winApplied;
        if (rate >= 0.5 && rate <= kMaxRate)
        {
            // Average successive windows so one odd stretch can't swing it.
            m_rate = m_calibrated ? static_cast<float>((m_rate + rate) * 0.5) : static_cast<float>(rate);
            m_calibrated = true;
        }
        Restart(applied, clock);
    }
}
