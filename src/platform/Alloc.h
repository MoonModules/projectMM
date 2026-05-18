#pragma once

#include <cstddef>

namespace mm::platform {

void* alloc(size_t bytes);
void  free(void* ptr);

} // namespace mm::platform
