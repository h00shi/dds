#include "./compile_exec.hpp"

#include <dds/build/file_deps.hpp>
#include <dds/error/errors.hpp>
#include <dds/proc.hpp>
#include <dds/util/string.hpp>
#include <dds/util/time.hpp>

#include <range/v3/view/filter.hpp>
#include <range/v3/view/transform.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <thread>

using namespace dds;
using namespace ranges;

namespace {

template <typename Range, typename Fn>
bool parallel_run(Range&& rng, int n_jobs, Fn&& fn) {
    // We don't bother with a nice thread pool, as the overhead of most build
    // tasks dwarf the cost of interlocking.
    std::mutex mut;

    auto       iter = rng.begin();
    const auto stop = rng.end();

    std::vector<std::exception_ptr> exceptions;

    auto run_one = [&]() mutable {
        while (true) {
            std::unique_lock lk{mut};
            if (!exceptions.empty()) {
                break;
            }
            if (iter == stop) {
                break;
            }
            auto&& item = *iter;
            ++iter;
            lk.unlock();
            try {
                fn(item);
            } catch (...) {
                lk.lock();
                exceptions.push_back(std::current_exception());
                break;
            }
        }
    };

    std::unique_lock         lk{mut};
    std::vector<std::thread> threads;
    if (n_jobs < 1) {
        n_jobs = std::thread::hardware_concurrency() + 2;
    }
    std::generate_n(std::back_inserter(threads), n_jobs, [&] { return std::thread(run_one); });
    lk.unlock();
    for (auto& t : threads) {
        t.join();
    }
    for (auto eptr : exceptions) {
        try {
            std::rethrow_exception(eptr);
        } catch (const std::exception& e) {
            spdlog::error(e.what());
        }
    }
    return exceptions.empty();
}

/// The actual "real" information that we need to perform a compilation.
struct compile_file_full {
    const compile_file_plan& plan;
    fs::path                 object_file_path;
    compile_command_info     cmd_info;
};

/// Simple aggregate that stores a counter for keeping track of compile progress
struct compile_counter {
    std::atomic_size_t n;
    const std::size_t  max;
    const std::size_t  max_digits;
};

/**
 * Actually performs a compilation and collects deps information from that compilation
 *
 * @param cf The compilation to execute
 * @param env The build environment
 * @param counter A thread-safe counter for display progress to the user
 */
std::optional<file_deps_info>
do_compile(const compile_file_full& cf, build_env_ref env, compile_counter& counter) {
    // Create the parent directory
    fs::create_directories(cf.object_file_path.parent_path());

    // Generate a log message to display to the user
    auto source_path = cf.plan.source_path();
    auto msg         = fmt::format("[{}] Compile: {}",
                           cf.plan.qualifier(),
                           fs::relative(source_path, cf.plan.source().basis_path).string());

    // Do it!
    spdlog::info(msg);
    auto&& [dur_ms, proc_res]
        = timed<std::chrono::milliseconds>([&] { return run_proc(cf.cmd_info.command); });
    auto nth = counter.n.fetch_add(1);
    spdlog::info("{:60} - {:>7n}ms [{:{}}/{}]",
                 msg,
                 dur_ms.count(),
                 nth,
                 counter.max_digits,
                 counter.max);

    const bool  compiled_okay   = proc_res.okay();
    const auto  compile_retc    = proc_res.retc;
    const auto  compile_signal  = proc_res.signal;
    std::string compiler_output = std::move(proc_res.output);

    // Build dependency information, if applicable to the toolchain
    std::optional<file_deps_info> ret_deps_info;

    if (env.toolchain.deps_mode() == file_deps_mode::gnu) {
        // GNU-style deps using Makefile generation
        assert(cf.cmd_info.gnu_depfile_path.has_value());
        auto& df_path = *cf.cmd_info.gnu_depfile_path;
        if (!fs::is_regular_file(df_path)) {
            spdlog::critical(
                "The expected Makefile deps were not generated on disk. This is a bug! "
                "(Expected file to exist: [{}])",
                df_path.string());
        } else {
            auto dep_info = dds::parse_mkfile_deps_file(df_path);
            assert(dep_info.output == cf.object_file_path);
            dep_info.command        = quote_command(cf.cmd_info.command);
            dep_info.command_output = compiler_output;
            ret_deps_info           = std::move(dep_info);
        }
    } else if (env.toolchain.deps_mode() == file_deps_mode::msvc) {
        // Uglier deps generation by parsing the output from cl.exe
        /// TODO: Handle different #include Note: prefixes, since those are localized
        auto msvc_deps = parse_msvc_output_for_deps(compiler_output, "Note: including file:");
        // parse_msvc_output_for_deps will return the compile output without the /showIncludes notes
        compiler_output = std::move(msvc_deps.cleaned_output);
        // Only update deps if we actually parsed anything, other wise we can't be sure that we
        // successfully parsed anything, and we don't want to store garbage deps info and possibly
        // cause a miscompile
        if (!msvc_deps.deps_info.inputs.empty()) {
            // Add the main source file as an input, since it is not listed by /showIncludes
            msvc_deps.deps_info.inputs.push_back(cf.plan.source_path());
            msvc_deps.deps_info.output         = cf.object_file_path;
            msvc_deps.deps_info.command        = quote_command(cf.cmd_info.command);
            msvc_deps.deps_info.command_output = compiler_output;
            ret_deps_info                      = std::move(msvc_deps.deps_info);
        }
    }

    // MSVC prints the filename of the source file. Remove it from the output.
    if (compiler_output.find(source_path.filename().string()) == 0) {
        compiler_output.erase(0, source_path.filename().string().length());
        if (starts_with(compiler_output, "\r")) {
            compiler_output.erase(0, 1);
        }
        if (starts_with(compiler_output, "\n")) {
            compiler_output.erase(0, 1);
        }
    }

    // Log a compiler failure
    if (!compiled_okay) {
        spdlog::error("Compilation failed: {}", source_path.string());
        spdlog::error("Subcommand FAILED [Exitted {}]: {}\n{}",
                      compile_retc,
                      quote_command(cf.cmd_info.command),
                      compiler_output);
        if (compile_signal) {
            spdlog::error("Process exited via signal {}", compile_signal);
        }
        throw_user_error<errc::compile_failure>("Compilation failed [{}]", source_path.string());
    }

    // Print any compiler output, sans whitespace
    if (!dds::trim_view(compiler_output).empty()) {
        spdlog::warn("While compiling file {} [{}]:\n{}",
                     source_path.string(),
                     quote_command(cf.cmd_info.command),
                     compiler_output);
    }

    // We'll only get here if the compilation was successful, otherwise we throw
    assert(compiled_okay);
    return ret_deps_info;
}

/// Generate the full compile command information from an abstract plan
compile_file_full realize_plan(const compile_file_plan& plan, build_env_ref env) {
    auto cmd_info = plan.generate_compile_command(env);
    return compile_file_full{plan, plan.calc_object_file_path(env), cmd_info};
}

/**
 * Determine if the given compile command should actually be executed based on
 * the dependency information we have recorded in the database.
 */
bool should_compile(const compile_file_full& comp, const database& db) {
    if (!fs::exists(comp.object_file_path)) {
        // The output file simply doesn't exist. We have to recompile, of course.
        return true;
    }
    auto rb_info = get_rebuild_info(db, comp.object_file_path);
    if (rb_info.previous_command.empty()) {
        // We have no previous compile command for this file. Assume it is new.
        return true;
    }
    if (!rb_info.newer_inputs.empty()) {
        // Inputs to this file have changed from a prior execution.
        return true;
    }
    auto cur_cmd_str = quote_command(comp.cmd_info.command);
    if (cur_cmd_str != rb_info.previous_command) {
        // The command used to generate the output is new
        return true;
    }
    // Nope. This file is up-to-date.
    return false;
}

}  // namespace

bool dds::detail::compile_all(const ref_vector<const compile_file_plan>& compiles,
                              build_env_ref                              env,
                              int                                        njobs) {
    auto each_realized =  //
        compiles
        // Convert each _plan_ into a concrete object for compiler invocation.
        | views::transform([&](auto&& plan) { return realize_plan(plan, env); })
        // Filter out compile jobs that we don't need to run. This drops compilations where the
        // output is "up-to-date" based on its inputs.
        | views::filter([&](auto&& real) { return should_compile(real, env.db); })
        // Convert to to a real vector so we can ask its size.
        | ranges::to_vector;

    // Keep a counter to display progress to the user.
    const auto      total      = each_realized.size();
    const auto      max_digits = fmt::format("{}", total).size();
    compile_counter counter{{1}, total, max_digits};

    // Ass we execute, accumulate new dependency information from successful compilations
    std::vector<file_deps_info> all_new_deps;
    std::mutex                  mut;
    // Do it!
    auto okay = parallel_run(each_realized, njobs, [&](const compile_file_full& full) {
        auto new_dep = do_compile(full, env, counter);
        if (new_dep) {
            std::unique_lock lk{mut};
            all_new_deps.push_back(std::move(*new_dep));
        }
    });

    // Update compile dependency information
    auto tr = env.db.transaction();
    for (auto& info : all_new_deps) {
        update_deps_info(neo::into(env.db), info);
    }

    // Return whether or not there were any failures.
    return okay;
}
