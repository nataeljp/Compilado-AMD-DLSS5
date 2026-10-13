#pragma once
#include "SysUtils.h"

class FrameLimit
{
    static int timer_sleep(int64_t hundred_ns);
    static int busywait_sleep(int64_t ns);
    static int combined_sleep(int64_t ns);

  public:
    static void sleep(bool fgActive);
    // [lmxxf2] usados pelo pacer do núcleo neural (InterleavePacing.h)
    static uint64_t now_ns();
    static void sleep_ns(int64_t ns);
};
