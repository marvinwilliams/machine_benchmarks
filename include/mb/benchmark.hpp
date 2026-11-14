#pragma once

#include <cxxopts.hpp>

#include <cstdlib>
#include <exception>
#include <iostream>
#include <utility>

namespace mb {

// Parses the command line into the settings and runs the benchmark, like benchmark::run of the MultiQueue
// experiments. Settings provide register_cmd_options(cxxopts::Options&) and resolve(), which converts and checks the
// parsed values (throwing on invalid ones). Errors are only reported up to this point; the benchmark itself has no
// error handling.
template <typename Settings, typename Benchmark>
int run(int argc, char* argv[], char const* description, Benchmark&& benchmark) {
    cxxopts::Options cmd(argv[0], description);
    cmd.add_options()("h,help", "Print this help");
    Settings settings{};
    settings.register_cmd_options(cmd);
    try {
        if (cmd.parse(argc, argv).count("help") > 0) {
            std::clog << cmd.help() << '\n';
            return EXIT_SUCCESS;
        }
        settings.resolve();
    } catch (std::exception const& e) {
        std::cerr << "Error parsing command line: " << e.what() << '\n';
        std::cerr << "Use --help for usage information\n";
        return EXIT_FAILURE;
    }
    std::forward<Benchmark>(benchmark)(settings);
    return EXIT_SUCCESS;
}

}  // namespace mb
