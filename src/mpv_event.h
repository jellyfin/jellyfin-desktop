#pragma once

enum class MpvEventType {
    NONE,       // sentinel -- unhandled, don't publish
    SHUTDOWN,
    FILE_LOADED,
    END_FILE_EOF,
    END_FILE_ERROR,
    END_FILE_CANCEL,
    PAUSE,
    TIME_POS,
    DURATION,
    FULLSCREEN,
    OSD_DIMS,
    SPEED,
    SEEKING,
};

struct MpvEvent {
    MpvEventType type;
    bool flag;              // PAUSE, FULLSCREEN, SEEKING
    double dbl;             // TIME_POS, DURATION, SPEED
    int pw, ph, lw, lh;    // OSD_DIMS
};
