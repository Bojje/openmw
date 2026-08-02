// The single translation unit that instantiates VulkanMemoryAllocator.
//
// vk_mem_alloc.h is a ~700 KB header-only library; defining VMA_IMPLEMENTATION here keeps the compile
// cost in one object file instead of in every file that touches a buffer. Everywhere else includes the
// header for its declarations only.
//
// VMA is vendored in extern/vma rather than pulled from the dependency bundle: vcpkg-deps ships only
// installed/ and cannot build packages, so adding it there is not an option.

#define VMA_IMPLEMENTATION

#ifdef _MSC_VER
// Third-party code built with OpenMW's warning level. Not our warnings to fix, and patching a vendored
// header would have to be redone on every update.
#pragma warning(push)
#pragma warning(disable : 4100 4127 4189 4324 4505)
#endif

#include <vk_mem_alloc.h>

#ifdef _MSC_VER
#pragma warning(pop)
#endif
