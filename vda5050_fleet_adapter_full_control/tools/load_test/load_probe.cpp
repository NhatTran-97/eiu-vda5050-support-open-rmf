// Runs the real Connector with N registered robots and a 10 Hz update loop; prints one "SAMPLE <json>" line per period
// with its metrics, CPU time and memory.
#include <sys/resource.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <rclcpp/rclcpp.hpp>

#include "vda5050_fleet_adapter_full_control/rmf/connector.hpp"
#include "vda5050_fleet_adapter_full_control/util/metrics.hpp"

namespace vfa = vda5050_fleet_adapter_full_control;
using Clock = std::chrono::steady_clock;

namespace {

// The number after `key` in /proc/self/status, or -1.
long status_value(const char *key)
{
    std::ifstream in("/proc/self/status");
    std::string line;
    const std::string prefix = std::string(key) + ":";
    while (std::getline(in, line))
    {
        if (line.compare(0, prefix.size(), prefix) == 0)
        {
            return std::atol(line.c_str() + prefix.size());
        }
    }
    return -1;
}

}  // namespace

// Usage: load_probe <robots> <seconds> <broker url> <sample period in seconds>
int main(int argc, char **argv)
{
    rclcpp::init(argc, argv);
    const int robots = argc > 1 ? std::atoi(argv[1]) : 10;
    const int seconds = argc > 2 ? std::atoi(argv[2]) : 60;
    const std::string broker = argc > 3 ? argv[3] : "tcp://127.0.0.1:18830";
    const int sample_s = argc > 4 ? std::atoi(argv[4]) : 5;

    vfa::rmf::Connector connector(rclcpp::get_logger("load"), broker, "AMR");
    std::vector<std::string> names;
    for (int i = 0; i < robots; ++i)
    {
        char name[16];
        char serial[16];
        std::snprintf(name, sizeof(name), "r%04d", i);
        std::snprintf(serial, sizeof(serial), "%04d", i);
        names.push_back(name);
        connector.add_robot(name, "LOAD", serial, vfa::rmf::Transform());
    }
    connector.start();

    vfa::util::Histogram loop_pass;
    vfa::util::Counter overruns;
    std::atomic<bool> running{true};
    std::thread update([&] {
        const auto period = std::chrono::milliseconds(100);
        auto slot = Clock::now();
        while (running)
        {
            const auto started = Clock::now();
            for (const auto &name : names)
            {
                connector.poll(name);
                (void)connector.get_data(name);
                (void)connector.is_online(name);
                (void)connector.is_command_completed(name);
                (void)connector.is_order_stuck(name);
            }
            const auto ended = Clock::now();
            loop_pass.record(ended - started);
            slot += period;
            if (ended > slot)
            {
                overruns.add();
                while (slot <= ended)
                {
                    slot += period;
                }
            }
            std::this_thread::sleep_until(slot);
        }
    });

    const auto t0 = Clock::now();
    for (int elapsed = sample_s; elapsed <= seconds; elapsed += sample_s)
    {
        std::this_thread::sleep_until(t0 + std::chrono::seconds(elapsed));
        nlohmann::json report = connector.metrics();
        rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        report["wall_s"] = std::chrono::duration<double>(Clock::now() - t0).count();
        report["cpu_s"] = static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
                          static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
        report["rss_mb"] = static_cast<double>(status_value("VmRSS")) / 1024.0;
        report["threads"] = status_value("Threads");
        report["update_loop"] = {{"pass_us", vfa::util::to_json(loop_pass.take())}, {"overruns", overruns.value()}};
        std::printf("SAMPLE %s\n", report.dump().c_str());
        std::fflush(stdout);
    }
    running = false;
    update.join();
    connector.shutdown();
    std::fflush(stdout);
    std::_Exit(0);
}
