// syncer 送达复现器：1 个 hub(模拟 GCS syncer 单线程) + N 个真实 gRPC 客户端(模拟 raylet)。
// 注入 M 条来自 M 个"虚拟源节点"的快照(模拟 PG commit 风暴的 fan-in 终点)，量:
//   (a) hub 排空墙钟  (b) 每目的地收齐 M 条的延迟分布 p50/p90/p99/max
//   (c) hub io_context event stats(写次数/巨批形态)
// 用法: syncer_delivery_bench [N=2000] [M=N] [payload=1024] [K_client_threads=16]
#include <grpcpp/create_channel.h>
#include <grpcpp/server_builder.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ray/asio/instrumented_io_context.h"
#include "ray/asio/periodical_runner.h"
#include "ray/common/id.h"
#include "ray/ray_syncer/ray_syncer.h"
#include "ray/util/network_util.h"

using ray::NodeID;
using ray::rpc::syncer::MessageType;
using ray::rpc::syncer::RaySyncMessage;
using Clock = std::chrono::steady_clock;

namespace {

class NullReporter : public ray::syncer::ReporterInterface {
 public:
  std::optional<RaySyncMessage> CreateSyncMessage(int64_t, MessageType) const override {
    return std::nullopt;
  }
};

// 每客户端接收器：数到 M 条记完成时刻。
class CountingReceiver : public ray::syncer::ReceiverInterface {
 public:
  CountingReceiver(size_t expect,
                   Clock::time_point *t0,
                   std::atomic<int> *done,
                   double *out_ms)
      : expect_(expect), t0_(t0), done_(done), out_ms_(out_ms) {}
  void ConsumeSyncMessage(std::shared_ptr<const RaySyncMessage>) override {
    if (++got_ == expect_) {
      *out_ms_ = std::chrono::duration<double, std::milli>(Clock::now() - *t0_).count();
      done_->fetch_add(1);
    }
  }
  size_t got_ = 0;
  size_t expect_;
  Clock::time_point *t0_;
  std::atomic<int> *done_;
  double *out_ms_;
};

RaySyncMessage MakeMsg(const NodeID &from, int64_t ver, size_t payload) {
  RaySyncMessage m;
  m.set_version(ver);
  m.set_message_type(MessageType::RESOURCE_VIEW);
  m.set_node_id(from.Binary());
  m.set_sync_message(std::string(payload, 'x'));
  return m;
}

}  // namespace

int main(int argc, char **argv) {
  size_t N = argc > 1 ? std::stoul(argv[1]) : 2000;
  size_t M = argc > 2 ? std::stoul(argv[2]) : N;
  size_t payload = argc > 3 ? std::stoul(argv[3]) : 1024;
  size_t K = argc > 4 ? std::stoul(argv[4]) : 16;

  // ---- hub(GCS 形态: 专属单线程, batch=1/delay=0, reporter=nullptr) ----
  instrumented_io_context server_io;
  auto server_node = NodeID::FromRandom();
  NullReporter null_reporter;
  const bool raw_wire = std::getenv("SYNCER_RAW") != nullptr;  // fix-①: 字节共享扇出
  auto server_syncer = std::make_unique<ray::syncer::RaySyncer>(
      server_io, ray::PeriodicalRunner::Create(server_io), server_node.Binary(), 1, 0,
      /*on_rpc_completion=*/nullptr, /*serialize_frames=*/raw_wire);
  // hub 侧不需要 receiver 参与计时，但 Register 需要非空之一; 仿 GCS: reporter=nullptr。
  static CountingReceiver hub_recv(SIZE_MAX, nullptr, nullptr, nullptr);
  server_syncer->Register(MessageType::RESOURCE_VIEW, nullptr, &hub_recv);
  auto service_typed = std::make_unique<ray::syncer::RaySyncerService>(*server_syncer);
  auto service_raw = std::make_unique<ray::syncer::RaySyncerServiceRaw>(*server_syncer);
  grpc::Service *service =
      raw_wire ? static_cast<grpc::Service *>(service_raw.get())
               : static_cast<grpc::Service *>(service_typed.get());
  printf("wire mode: %s\n", raw_wire ? "RAW(serialize-once)" : "typed(upstream)");
  grpc::ServerBuilder builder;
  int port = 0;
  builder.SetMaxReceiveMessageSize(512 * 1024 * 1024);
  builder.SetMaxSendMessageSize(512 * 1024 * 1024);
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(service);
  auto server = builder.BuildAndStart();
  std::thread server_thread([&server_io] {
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> g(
        server_io.get_executor());
    server_io.run();
  });

  // ---- N 个客户端, K 根共享 io 线程 ----
  std::vector<std::unique_ptr<instrumented_io_context>> client_ios;
  std::vector<std::thread> client_threads;
  for (size_t k = 0; k < K; k++) {
    client_ios.push_back(std::make_unique<instrumented_io_context>());
    auto *io = client_ios.back().get();
    client_threads.emplace_back([io] {
      boost::asio::executor_work_guard<boost::asio::io_context::executor_type> g(
          io->get_executor());
      io->run();
    });
  }
  Clock::time_point t0;  // 风暴起点(注入前设置)
  std::atomic<int> done{0};
  std::vector<double> lat_ms(N, -1.0);
  std::vector<std::unique_ptr<ray::syncer::RaySyncer>> clients;
  std::vector<std::unique_ptr<CountingReceiver>> recvs;
  clients.reserve(N);
  recvs.reserve(N);
  std::string addr = ray::BuildAddress("127.0.0.1", std::to_string(port));
  for (size_t i = 0; i < N; i++) {
    auto &io = *client_ios[i % K];
    auto nid = NodeID::FromRandom();
    recvs.push_back(std::make_unique<CountingReceiver>(M, &t0, &done, &lat_ms[i]));
    auto sy = std::make_unique<ray::syncer::RaySyncer>(
        io, ray::PeriodicalRunner::Create(io), nid.Binary(), 1, 0);
    sy->Register(MessageType::RESOURCE_VIEW, &null_reporter, recvs.back().get());
    grpc::ChannelArguments args;
    args.SetInt("bench.client.id", static_cast<int>(i));  // 防 subchannel 合并
    // 镜像生产 channel 参数:巨批可到几十 MB,默认 4MB 上限会拒收→断流→重连风暴
    args.SetMaxReceiveMessageSize(512 * 1024 * 1024);
    args.SetMaxSendMessageSize(512 * 1024 * 1024);
    auto ch = grpc::CreateCustomChannel(addr, grpc::InsecureChannelCredentials(), args);
    sy->Connect(server_node.Binary(), ch);
    clients.push_back(std::move(sy));
    if (i % 500 == 0) {
      printf("  connected %zu/%zu\n", i, N);
      fflush(stdout);
    }
  }
  // 等全部注册到 hub
  while (true) {
    auto n = server_syncer->GetAllConnectedNodeIDs().size();
    if (n >= N) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  printf("all %zu connected; injecting storm M=%zu payload=%zu\n", N, M, payload);
  fflush(stdout);

  // ---- 风暴注入: M 条来自互不相同的"虚拟源"(不与任何客户端同 id, 不被 origin-skip) ----
  t0 = Clock::now();
  for (size_t j = 0; j < M; j++) {
    auto msg =
        std::make_shared<RaySyncMessage>(MakeMsg(NodeID::FromRandom(), 1, payload));
    server_syncer->BroadcastMessage(std::move(msg));
  }
  auto t_inject = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();

  // ---- 等全体收齐 ----
  while (done.load() < static_cast<int>(N)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    static int tick = 0;
    if (++tick % 25 == 0) {
      printf("  done %d/%zu @ %.1fs\n", done.load(), N,
             std::chrono::duration<double>(Clock::now() - t0).count());
      fflush(stdout);
    }
  }
  std::vector<double> s = lat_ms;
  std::sort(s.begin(), s.end());
  printf("\nN=%zu M=%zu payload=%zuB | inject=%.1fms | drain(all)=%.1fms\n",
         N, M, payload, t_inject, s.back());
  printf("per-dest latency ms: p50=%.1f p90=%.1f p99=%.1f max=%.1f min=%.1f\n",
         s[s.size() / 2], s[s.size() * 9 / 10], s[s.size() * 99 / 100], s.back(), s[0]);
  {
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    while (f && fgets(line, sizeof(line), f)) {
      if (strncmp(line, "VmHWM", 5) == 0) printf("%s", line);
    }
    if (f) fclose(f);
  }
  printf("\n==== hub io_context event stats ====\n%s\n",
         server_io.stats()->StatsString().c_str());
  fflush(stdout);
  _exit(0);  // 跳过 10k 对象的析构长尾
}
