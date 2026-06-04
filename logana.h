#ifndef LOGANA_H
#define LOGANA_H

#include "logana/config.h"
#include "logana/engine.h"
#include "logana/queue.h"
#include "logana/render.h"
#include "logana/server.h"
#include "logana/types.h"
#include "simd_wrapper.h"

#ifndef SAFE_FREE
#define SAFE_FREE(ptr) do { free(ptr); ptr = NULL; } while(0)
#endif

#endif
