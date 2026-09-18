#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <thread>
#include "uvhid.hpp"

inline void precise_wait(std::chrono::nanoseconds duration)
{
    const auto deadline = std::chrono::steady_clock::now() + duration;

    while (std::chrono::steady_clock::now() < deadline) {
    }
}

int main()
{
    try {
        for (int seconds = 3; seconds > 0; --seconds) {
            std::cout << "Starting in " << seconds << "...\n" << std::flush;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        uvhid::Controller mouse;

        const auto finish =
            std::chrono::steady_clock::now() + std::chrono::seconds(3);

        constexpr auto movement_delay = std::chrono::microseconds(50);
        std::uint64_t cycles = 0;

        while (std::chrono::steady_clock::now() < finish) {
            mouse.move_mouse(3, 3);
            precise_wait(movement_delay);

            mouse.move_mouse(-3, -3);
            precise_wait(movement_delay);

            ++cycles;
        }

        std::cout << "Stopped after " << cycles
            << " movement cycles.\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}