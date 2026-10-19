#ifndef PR_FRAMEWORK_WORKER_THREAD_POOL_H
#define PR_FRAMEWORK_WORKER_THREAD_POOL_H
// Selection belongs to the composition root; consumers use ThreadPoolBase.
#if PROTORELAY_WORKER_BOOST
#include "framework/thread_pool/boost_thread_pool.h"
namespace pr { using WorkerThreadPool = BoostThreadPool; }
#else
#include "framework/thread_pool/std_thread_pool.h"
namespace pr { using WorkerThreadPool = StdThreadPool; }
#endif
#endif
