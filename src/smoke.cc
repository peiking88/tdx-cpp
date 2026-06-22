// Task 1 骨架验证：确认 helio（base + fibers2）链接通过，io_uring ProactorPool 能启动并调度 fiber。
// 这是整个协议层（tdx::proto）的运行时地基——后续 Connection/Heartbeat/ServerPool 都跑在该 pool 上。
//
// 镜像 helio/examples/echo_server.cc:621-663 的 pool 启动范式：
//   fb2::Pool::IOUring(ring_depth) → pp->Run() → GetNextProactor()->Await([&]{...}) → pp->Stop()
#include <chrono>
#include <iostream>
#include <memory>

#include "base/init.h"
#include "util/fibers/fibers.h"
#include "util/fibers/pool.h"
#include "util/fibers/proactor_base.h"
#include "util/fibers/synchronization.h"

using namespace util;

int main(int argc, char* argv[]) {
  // helio 初始化：MainInitGuard 必须传 &argc, &argv（指针的指针），且必须赋给具名变量
  // （base/init.h:62 有 static_assert 陷阱：裸 MainInitGuard(argc,argv) 会编译失败）。
  MainInitGuard guard(&argc, &argv);

  // 启动 io_uring ProactorPool（Linux 内核 ≥5.11；当前 7.0-aws 完全支持）。
  // 注意：ProactorPool 在 util 命名空间，fb2::Pool 在 util::fb2（pool.h:12）。
  std::unique_ptr<ProactorPool> pp(fb2::Pool::IOUring(/*ring_depth=*/64));
  pp->Run();

  // 在 pool 的某个 proactor 上同步跑一个 fiber，验证 fiber 调度 + ThisFiber::SleepFor。
  // Await 把 lambda 投递到 proactor 线程并阻塞当前（main）线程直到完成。
  ProactorBase* pb = pp->GetNextProactor();
  int fiber_result = pb->Await([&] {
    ThisFiber::SleepFor(std::chrono::milliseconds(5));
    return 42;
  });

  std::cout << "tdx-cpp smoke: helio OK, pool_size=" << pp->size()
            << ", fiber_result=" << fiber_result << "\n";

  pp->Stop();
  return fiber_result == 42 ? 0 : 1;
}
