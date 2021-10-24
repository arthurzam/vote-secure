#include <cppcoro/net/socket.hpp>
#include <cppcoro/io_service.hpp>
#include <cppcoro/task.hpp>
#include <cppcoro/sync_wait.hpp>
#include <cppcoro/when_all.hpp>
#include <cppcoro/cancellation_source.hpp>
#include <cppcoro/async_scope.hpp>
#include <cppcoro/on_scope_exit.hpp>

#include <memory>
#include <iostream>

#include "talliers_network.h"
#include "mpc_service.h"

#include "utils.h"

int main(int argc, const char *argv[]) {
    std::cout << std::unitbuf; // Always flush when writing
    std::cerr << std::unitbuf; // Always flush when writing

    cppcoro::io_service ioSvc(16384);
    talliers_network net(ioSvc, argc < 2 ? 0 : atoi(argv[1]));

    (void) cppcoro::sync_wait(cppcoro::when_all(
            [&]() -> cppcoro::task<> {
                // Shutdown the event loop once finished.
                auto stopOnExit = cppcoro::on_scope_exit([&] { ioSvc.stop(); });
                co_await net.build_collect();
                std::cout << "service" << std::endl;
                mpc_service service(net);

                auto super_task = [&](uint16_t msg_id) -> cppcoro::task<> {
                    std::vector<cppcoro::task<utils::share>> rnd_t;
                    rnd_t.reserve(32);
                    for (int i = 0; i < 32; i++)
                        rnd_t.push_back(service.random_bit(msg_id + i));
                    auto rnd = co_await cppcoro::when_all(std::move(rnd_t));

                    std::vector<cppcoro::task<utils::share>> tasks;
                    tasks.reserve(33);
                    for (int i = 0; i < 32; i++)
                        tasks.push_back(service.resolve(msg_id + i, rnd[i]));
                    tasks.push_back(service.fan_in_or(msg_id + 32, {rnd.data(), 32}));
                    auto res = co_await cppcoro::when_all(std::move(tasks));

                    std::string out = "{";
                    for (int i = 0; i < 32; i++)
                        out += std::to_string(res[i]) + " ";
                    out += "} -> ";
                    out += std::to_string(co_await service.resolve(msg_id, res[32]));
                    out += "\n";
                    std::cout << out;
                };

                auto func = [&](int i) -> cppcoro::task<utils::share> {
                    auto rnd = co_await service.random_bit(i);
                    auto mul = co_await service.multiply(i, rnd, rnd);
                    auto res = co_await service.resolve(i, mul);
                    co_return res;
                };

                auto rnd_num = [&](unsigned i) -> cppcoro::task<> {
                    std::cout << co_await service.resolve(i, co_await service.random_bit(i)) << std::endl;
                };

                for (int j = 0; j < 10; j++) {
                    std::vector<cppcoro::task<>> tasks;
                    tasks.reserve(100);
                    for (unsigned i = 0; i < 50; i++)
                        tasks.push_back(super_task(80 * i));
                    co_await cppcoro::when_all(std::move(tasks));
                }

                std::cout << "Done!" << std::endl;
                co_await ioSvc.schedule_after(std::chrono::seconds(1));
                co_await net.close();
            }(),
            [&]() -> cppcoro::task<> {
                ioSvc.process_events();
                co_return;
            }()));

    return 0;
}