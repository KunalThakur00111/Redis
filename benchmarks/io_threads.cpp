// Compare one and four I/O threads with the same pipelined command workloads.
#include "../tests/test_support.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <netinet/tcp.h>
#include <numeric>
#include <sstream>

namespace {
constexpr int clients = 16;
constexpr int pipeline = 32;
constexpr int batches = 2000;
constexpr int trials = 3;

enum class Workload { ping, get, set, incr };

const char* workload_name(Workload workload) {
    switch(workload){
        case Workload::ping: return "PING";
        case Workload::get: return "GET";
        case Workload::set: return "SET";
        case Workload::incr: return "INCR";
    }
    return "unknown";
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    std::size_t middle = values.size() / 2;
    if(values.size() % 2 == 0) return (values[middle - 1] + values[middle]) / 2;
    return values[middle];
}

double main_cpu_seconds(pid_t pid) {
    std::ifstream stat("/proc/" + std::to_string(pid) + "/task/" +
                       std::to_string(pid) + "/stat");
    std::string line;
    TEST_CHECK(static_cast<bool>(std::getline(stat, line)));
    const std::size_t name_end = line.rfind(')');
    TEST_CHECK(name_end != std::string::npos);
    std::istringstream fields(line.substr(name_end + 2));
    std::uint64_t user_ticks = 0;
    std::uint64_t system_ticks = 0;
    std::string value;
    for(int field = 3; field <= 15; field++){
        TEST_CHECK(static_cast<bool>(fields >> value));
        if(field == 14) user_ticks = std::stoull(value);
        if(field == 15) system_ticks = std::stoull(value);
    }
    long ticks_per_second = sysconf(_SC_CLK_TCK);
    TEST_CHECK(ticks_per_second > 0);
    return static_cast<double>(user_ticks + system_ticks) / ticks_per_second;
}

std::vector<double> run_client(std::uint16_t port, Workload workload,
                               int client_index, int value_size) {
    test::Fd connection = test::connect_to(port, 10);
    int no_delay = 1;
    TEST_CHECK(setsockopt(connection.get(), IPPROTO_TCP, TCP_NODELAY,
                          &no_delay, sizeof(no_delay)) == 0);
    const std::string key = "benchmark:" + std::to_string(client_index);
    const std::string value(static_cast<std::size_t>(value_size), 'x');
    if(workload != Workload::ping){
        test::send_all(connection.get(), test::request({"SET", key,
            workload == Workload::incr ? std::string_view("0") : std::string_view(value)}));
        TEST_CHECK(test::read_exact(connection.get(), 5) == "+OK\r\n");
    }
    std::string payload;
    std::string expected;
    for(int i = 0; i < pipeline; i++){
        if(workload == Workload::ping){
            payload += test::request({"PING"});
            expected += "+PONG\r\n";
        } else if(workload == Workload::get){
            payload += test::request({"GET", key});
            expected += "$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";
        } else if(workload == Workload::set){
            payload += test::request({"SET", key, value});
            expected += "+OK\r\n";
        } else {
            payload += test::request({"INCR", key});
        }
    }
    std::vector<std::string> increment_replies;
    if(workload == Workload::incr){
        increment_replies.reserve(batches);
        for(int i = 0; i < batches; i++){
            std::string replies;
            for(int j = 0; j < pipeline; j++)
                replies += ":" + std::to_string(i * pipeline + j + 1) + "\r\n";
            increment_replies.push_back(std::move(replies));
        }
    }
    std::vector<double> round_trips;
    round_trips.reserve(batches);
    for(int i = 0; i < batches; i++){
        const std::string& reply = workload == Workload::incr ? increment_replies[i] : expected;
        auto start = std::chrono::steady_clock::now();
        test::send_all(connection.get(), payload);
        TEST_CHECK(test::read_exact(connection.get(), reply.size()) == reply);
        auto elapsed = std::chrono::steady_clock::now() - start;
        round_trips.push_back(std::chrono::duration<double, std::milli>(elapsed).count());
    }
    return round_trips;
}

struct Result {
    double throughput;
    double median_rtt;
    double p95_rtt;
    double main_cpu_percent;
};

Result measure(const std::string& binary, int io_threads, Workload workload,
               int value_size) {
    test::Server server(binary, {"--io-threads", std::to_string(io_threads)});
    double cpu_start = main_cpu_seconds(server.pid());
    auto start = std::chrono::steady_clock::now();
    std::vector<std::future<std::vector<double>>> jobs;
    for(int i = 0; i < clients; i++)
        jobs.push_back(std::async(std::launch::async, run_client,
                                  server.port(), workload, i, value_size));
    std::vector<double> round_trips;
    for(auto& job : jobs){
        auto client = job.get();
        round_trips.insert(round_trips.end(), client.begin(), client.end());
    }
    auto elapsed = std::chrono::steady_clock::now() - start;
    double cpu_elapsed = main_cpu_seconds(server.pid()) - cpu_start;
    std::sort(round_trips.begin(), round_trips.end());
    return {clients * pipeline * batches /
                std::chrono::duration<double>(elapsed).count(),
            median(round_trips), round_trips[static_cast<std::size_t>(round_trips.size() * 0.95)],
            100.0 * cpu_elapsed / std::chrono::duration<double>(elapsed).count()};
}
}

int main(int argc, char* argv[]) {
    try {
        TEST_CHECK(argc >= 2 && argc <= 4);
        const int value_size = argc >= 3 ? std::stoi(argv[2]) : 128;
        TEST_CHECK(value_size >= 1 && value_size <= 1024);
        const std::string selected = argc == 4 ? argv[3] : "";
        TEST_CHECK(selected.empty() || selected == "PING" || selected == "GET" ||
                   selected == "SET" || selected == "INCR");
        std::cout << "Workload: " << clients << " persistent clients, " << pipeline
                  << " commands per pipeline, " << batches << " pipelines per client, "
                  << trials << " trials, " << value_size << "-byte values\n"
                  << std::fixed << std::setprecision(3);
        for(Workload workload : {Workload::ping, Workload::get,
                                 Workload::set, Workload::incr}){
            if(!selected.empty() && selected != workload_name(workload)) continue;
            std::cout << workload_name(workload) << " (AOF off)\n";
            for(int io_threads : {1, 4}){
                std::vector<double> throughputs, medians, p95s, main_cpu;
                for(int trial = 0; trial < trials; trial++){
                    Result result = measure(argv[1], io_threads, workload, value_size);
                    throughputs.push_back(result.throughput);
                    medians.push_back(result.median_rtt);
                    p95s.push_back(result.p95_rtt);
                    main_cpu.push_back(result.main_cpu_percent);
                }
                std::cout << "  io-threads=" << io_threads
                          << ": throughput median=" << std::setprecision(0)
                          << median(throughputs) << " ops/s; pipeline RTT median="
                          << std::setprecision(3) << median(medians)
                          << " ms; pipeline RTT p95 median=" << median(p95s)
                          << " ms; main CPU=" << std::setprecision(1)
                          << median(main_cpu) << "%; throughput trials=[";
                for(std::size_t i = 0; i < throughputs.size(); i++){
                    if(i) std::cout << ", ";
                    std::cout << std::setprecision(0) << throughputs[i];
                }
                std::cout << "]\n" << std::setprecision(3);
            }
        }
        return 0;
    } catch(const std::exception& error){
        std::cerr << error.what() << '\n';
        return 1;
    }
}
