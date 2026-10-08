#pragma once

#include <cmath>
#include <cstdint>

using namespace std;

struct Range {
    uint64_t first;
    uint64_t last;  // first > last means the range is empty
};

// splits first..last into `parts` pieces and returns piece number `index` (starts at 0)
// leftover numbers go to the first pieces
Range splitRange(uint64_t first, uint64_t last, uint64_t parts, uint64_t index) {
    Range piece;
    piece.first = 1;
    piece.last = 0;
    if (first > last) return piece;

    uint64_t total = last - first + 1;
    uint64_t size = total / parts;
    uint64_t extra = total % parts;

    uint64_t pieceSize = size;
    if (index < extra) pieceSize++;
    if (pieceSize == 0) return piece;

    piece.first = first + index * size;
    if (index < extra) {
        piece.first = piece.first + index;
    } else {
        piece.first = piece.first + extra;
    }
    piece.last = piece.first + pieceSize - 1;
    return piece;
}

uint64_t intSqrt(uint64_t n) {
    uint64_t root = (uint64_t)sqrt((double)n);
    if (root > 4294967295ULL) root = 4294967295ULL;
    while (root * root > n) root--;
    while (root < 4294967295ULL && (root + 1) * (root + 1) <= n) root++;
    return root;
}

// only needs to check divisors up to sqrt(n)
bool isPrime(uint64_t n) {
    if (n < 2) return false;
    uint64_t root = intSqrt(n);
    for (uint64_t d = 2; d <= root; d++) {
        if (n % d == 0) return false;
    }
    return true;
}
