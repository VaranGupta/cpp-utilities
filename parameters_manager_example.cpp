#include <iostream>
#include <thread>

#include "parameters_manager/parameters_manager.hpp"


namespace
{
void demo_other_place()
{
    auto name = Parameters::get<std::string>("app.name");
    auto port = Parameters::get<long long>("db.primary.port");
    std::cout<< std::format("[other.cpp] app.name={}, db.port={}", name, port);
}
} // namespace

int main()
{

    std::cout<< std::format("Hello World");

    try
    {
        Parameters::init("config/config.yaml");
    } catch (const Parameters::Error& e)
    {
        std::cout<< std::format("Init failed: {}", e.what());
        return 1;
    }

    // Basic reads
    std::cout<< std::format("servers[0].port = {}", Parameters::get<long long>("servers[0].port"));
    std::cout<< std::format("servers[0].port = {}", Parameters::get<std::string>("servers[1][1].host"));
    std::cout<< std::format("app.name = {}", Parameters::get<std::string>("app.name"));
    std::cout<< std::format("testScalar = {}", Parameters::get<float>("testScalar"));
    std::cout<< std::format("list = {}", Parameters::get<std::string>("list[1]"));
    std::cout<< std::format("db.primary.host = {}", Parameters::get<std::string>("db.primary.host"));
    std::cout<< std::format("pool.timeout (ms) = {}", Parameters::get<std::chrono::milliseconds>("db.pool.timeout").count());
    std::cout<< std::format("features.enableCoolThing? {}", Parameters::get<bool>("features.enableCoolThing") ? "yes" : "no");

    demo_other_place();

    // Concurrent reader thread looping
    std::atomic<bool> run {true};
    std::thread worker([&] {
        while (run.load())
        {
            try
            {
                auto lvl = Parameters::get<std::string>("app.logLevel");
                auto maxc = Parameters::get<long long>("db.pool.maxConnections");
                (void)lvl;
                (void)maxc; // pretend to use
            } catch (...)
            {
                // ignore for demo
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });

    std::cout<< std::format("\n--- Reload demo ---\n"
         "Edit config/config.yaml (or change env vars), then press ENTER to reload.");
    std::cin.get();
    try
    {
        Parameters::reload();
        std::cout<< std::format("Reloaded.");
        std::cout<< std::format("New app.logLevel = {}", Parameters::get<std::string>("app.logLevel"));
    } catch (const Parameters::Error& e)
    {
        std::cout<< std::format("Reload failed: {}", e.what());
    }

    run.store(false);
    worker.join();
    return 1;
}