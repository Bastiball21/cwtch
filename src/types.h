#ifndef TYPES_H
#define TYPES_H

#include <stdint.h>
#include <time.h>

// BUILD is injected by the makefile from VERSION (-DBUILD=...).

#ifndef NET_H1_SIZE
#define NET_H1_SIZE 1536
#endif
#define NET_I_SIZE 768   // inputs per king bucket
#ifndef NET_I_BUCKETS
#define NET_I_BUCKETS 16  // king buckets per perspective, layout in net.c
#endif
#define NET_MIRRORED 1   // mirror board horizontally when king on files e-h
#define NET_O_BUCKETS 8  // output buckets by piece count
#define NET_QA 255
#define NET_QB 64
#define NET_QAB (NET_QA * NET_QB)
#define NET_SCALE 400

_Static_assert(NET_H1_SIZE > 0 && (NET_H1_SIZE % 32) == 0 && NET_H1_SIZE <= 4096,
               "NET_H1_SIZE must be a multiple of 32 and <= 4096");

// Path to the weights file embedded via INCBIN in net.c. Hardwired; to build
// against a different net, edit this line.
#define NET_WEIGHTS_PATH "./quantised.bin"

#define NOT_A_FILE 0xfefefefefefefefeULL
#define NOT_H_FILE 0x7f7f7f7f7f7f7f7fULL

enum {WHITE, BLACK};
enum {PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING};
enum {WPAWN, WKNIGHT, WBISHOP, WROOK, WQUEEN, WKING, BPAWN, BKNIGHT, BBISHOP, BROOK, BQUEEN, BKING};

enum {
  A1, B1, C1, D1, E1, F1, G1, H1,
  A2, B2, C2, D2, E2, F2, G2, H2,
  A3, B3, C3, D3, E3, F3, G3, H3,
  A4, B4, C4, D4, E4, F4, G4, H4,
  A5, B5, C5, D5, E5, F5, G5, H5,
  A6, B6, C6, D6, E6, F6, G6, H6,
  A7, B7, C7, D7, E7, F7, G7, H7,
  A8, B8, C8, D8, E8, F8, G8, H8
};

#define INF 32001
#define MATE 32000
#define MATEISH 31000

static inline int piece_index(const int piece, const int colour) {
  return piece + colour * 6;
}

static inline int colour_index(const int colour) {
  return colour * 6;
}

static inline int piece_colour(const int index) {
  return index / 6;
}

static inline int piece_type(const int index) {
  return index % 6;
}

#ifdef _WIN32
  #include <windows.h>
  static inline uint64_t time_ms(void) {
    return (uint64_t)GetTickCount64();
  }
#else
  static inline uint64_t time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
  }
#endif

#endif