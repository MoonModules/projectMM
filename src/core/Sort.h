#pragma once

#include <cstddef>

namespace mm {

// In-place insertion sort over a fixed array — the generic sort primitive for the
// small, bounded collections the system holds (a device list, a handful of rows),
// kept in core so every module sorts the same way and supplies only its comparator.
//
// Why insertion sort, not std::sort: the collections are tiny and bounded (≤ a few
// dozen), already nearly-ordered between updates, and live in fixed arrays with no
// allocation — insertion sort is O(n) on nearly-sorted input, allocation-free, stable,
// and ~10 lines a contributor reads at a glance. std::sort's introsort machinery buys
// nothing at this scale and pulls in <algorithm>. (Reach for std::sort when a genuinely
// large or performance-critical sort appears; this is the right tool for bounded UI/state
// lists.) Off the hot path — call it when the collection changes, not per render tick.
//
// `less(a, b)` returns true when a should sort before b. Stable: equal elements keep
// their relative order. T must be move/copy-assignable (our row structs are plain data).
template <typename T, typename Less>
inline void insertionSort(T* arr, size_t n, Less less) {
    for (size_t i = 1; i < n; i++) {
        T key = arr[i];
        size_t j = i;
        while (j > 0 && less(key, arr[j - 1])) {
            arr[j] = arr[j - 1];
            j--;
        }
        arr[j] = key;
    }
}

}  // namespace mm
