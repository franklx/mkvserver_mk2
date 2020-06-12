#ifndef DEBUG_H
#define DEBUG_H

#define DEBUG(fmt, ...) \
            do { if (0) printf(fmt, ##__VA_ARGS__); } while (0)

#endif
