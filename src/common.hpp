#pragma once

#if defined(_MSC_VER)
#define AVC_LIKELY(x)   (x)
#define AVC_UNLIKELY(x) (x)
#else
#define AVC_LIKELY(x)   __builtin_expect(!!(x), 1)
#define AVC_UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif
