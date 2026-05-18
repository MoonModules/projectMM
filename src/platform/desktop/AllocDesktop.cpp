#include "platform/Alloc.h"
#include <cstdlib>

namespace mm::platform {

void* alloc(size_t bytes) { return std::malloc(bytes); }
void  free(void* ptr)     { std::free(ptr); }

} // namespace mm::platform
