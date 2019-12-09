#include "./archive.hpp"

#include <dds/proc.hpp>
#include <dds/util/time.hpp>

#include <range/v3/view/transform.hpp>
#include <spdlog/spdlog.h>

using namespace dds;

fs::path create_archive_plan::calc_archive_file_path(const build_env& env) const noexcept {
    return env.output_root / _subdir
        / fmt::format("{}{}{}", "lib", _name, env.toolchain.archive_suffix());
}

void create_archive_plan::archive(const build_env& env) const {
    // Convert the file compilation plans into the paths to their respective object files.
    const auto objects =  //
        _compile_files    //
        | ranges::views::transform([&](auto&& cf) { return cf.calc_object_file_path(env); })
        | ranges::to_vector  //
        ;
    // Build up the archive command
    archive_spec ar;
    ar.input_files = std::move(objects);
    ar.out_path    = calc_archive_file_path(env);
    auto ar_cmd    = env.toolchain.create_archive_command(ar);

    // `out_relpath` is purely for the benefit of the user to have a short name
    // in the logs
    auto out_relpath = fs::relative(ar.out_path, env.output_root).string();

    // Different archiving tools behave differently between platforms depending on whether the
    // archive file exists. Make it uniform by simply removing the prior copy.
    if (fs::exists(ar.out_path)) {
        fs::remove(ar.out_path);
    }

    // Ensure the parent directory exists
    fs::create_directories(ar.out_path.parent_path());

    // Do it!
    spdlog::info("[{}] Archive: {}", _name, out_relpath);
    auto&& [dur_ms, ar_res] = timed<std::chrono::milliseconds>([&] { return run_proc(ar_cmd); });
    spdlog::info("[{}] Archive: {} - {:n}ms", _name, out_relpath, dur_ms.count());

    // Check, log, and throw
    if (!ar_res.okay()) {
        spdlog::error("Creating static library archive failed: {}", out_relpath);
        spdlog::error("Subcommand FAILED: {}\n{}", quote_command(ar_cmd), ar_res.output);
        throw std::runtime_error(
            fmt::format("Creating archive [{}] failed for '{}'", out_relpath, _name));
    }
}
