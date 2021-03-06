#include "./manifest.hpp"

#include <dds/dym.hpp>
#include <dds/error/errors.hpp>
#include <dds/util/string.hpp>
#include <libman/parse.hpp>

#include <range/v3/view/split.hpp>
#include <range/v3/view/split_when.hpp>
#include <range/v3/view/transform.hpp>
#include <semester/decomp.hpp>
#include <spdlog/spdlog.h>

#include <json5/parse_data.hpp>

using namespace dds;

package_manifest package_manifest::load_from_dds_file(const fs::path& fpath) {
    spdlog::warn(
        "Using deprecated package.dds parsing (on file {}). This will be removed soon. Migrate!",
        fpath.string());
    auto                       kvs = lm::parse_file(fpath);
    package_manifest           ret;
    std::string                version_str;
    std::vector<std::string>   depends_strs;
    std::optional<std::string> opt_test_driver;
    lm::read(fmt::format("Reading package manifest '{}'", fpath.string()),
             kvs,
             lm::read_required("Name", ret.pkg_id.name),
             lm::read_opt("Namespace", ret.namespace_),
             lm::read_required("Version", version_str),
             lm::read_accumulate("Depends", depends_strs),
             lm::read_opt("Test-Driver", opt_test_driver),
             lm_reject_dym{{"Name", "Namespace", "Version", "Depends", "Test-Driver"}});

    if (ret.pkg_id.name.empty()) {
        throw_user_error<errc::invalid_pkg_name>("'Name' field in [{}] may not be an empty string",
                                                 fpath.string());
    }
    if (version_str.empty()) {
        throw_user_error<
            errc::invalid_version_string>("'Version' field in [{}] may not be an empty string",
                                          fpath.string());
    }
    if (opt_test_driver) {
        auto& test_driver_str = *opt_test_driver;
        if (test_driver_str == "Catch-Main") {
            ret.test_driver = test_lib::catch_main;
        } else if (test_driver_str == "Catch") {
            ret.test_driver = test_lib::catch_;
        } else {
            auto dym = *did_you_mean(test_driver_str, {"Catch-Main", "Catch"});
            throw_user_error<
                errc::unknown_test_driver>("Unknown 'test_driver' '{}' (Did you mean '{}'?)",
                                           test_driver_str,
                                           dym);
        }
    }

    if (ret.namespace_.empty()) {
        ret.namespace_ = ret.pkg_id.name;
    }

    ret.pkg_id.version = semver::version::parse(version_str);

    ret.dependencies = depends_strs                                   //
        | ranges::views::transform(dependency::parse_depends_string)  //
        | ranges::to_vector;

    return ret;
}

package_manifest package_manifest::load_from_file(const fs::path& fpath) {
    auto content = slurp_file(fpath);
    auto data    = json5::parse_data(content);

    if (!data.is_object()) {
        throw_user_error<errc::invalid_pkg_manifest>("Root value must be an object");
    }

    package_manifest ret;
    using namespace semester::decompose_ops;
    auto res = semester::decompose(  //
        data,
        try_seq{
            require_type<json5::data::mapping_type>{
                "The root of a package manifest must be an object (mapping)"},
            mapping{
                if_key{"$schema", just_accept},
                if_key{
                    "name",
                    require_type<std::string>{"`name` must be a string"},
                    put_into{ret.pkg_id.name},
                },
                if_key{
                    "namespace",
                    require_type<std::string>{"`namespace` must be a string"},
                    put_into{ret.namespace_},
                },
                if_key{
                    "version",
                    require_type<std::string>{"`version` must be a string"},
                    [&](auto&& version_str_) {
                        auto& version      = version_str_.as_string();
                        ret.pkg_id.version = semver::version::parse(version);
                        return semester::dc_accept;
                    },
                },
                if_key{
                    "depends",
                    require_type<json5::data::mapping_type>{
                        "`depends` must be a mapping between package names and version ranges"},
                    mapping{[&](auto pkg_name, auto&& range_str_) {
                        if (!range_str_.is_string()) {
                            throw_user_error<errc::invalid_pkg_manifest>(
                                "Dependency for '{}' must be a range string", pkg_name);
                        }
                        try {
                            auto rng = semver::range::parse_restricted(range_str_.as_string());
                            dependency dep{std::string(pkg_name), {rng.low(), rng.high()}};
                            ret.dependencies.push_back(std::move(dep));
                        } catch (const semver::invalid_range&) {
                            throw_user_error<errc::invalid_version_range_string>(
                                "Invalid version range string '{}' in dependency declaration for "
                                "'{}'",
                                range_str_.as_string(),
                                pkg_name);
                        }
                        return semester::dc_accept;
                    }},
                },
                if_key{"test_driver",
                       require_type<std::string>{"`test_driver` must be a string"},
                       [&](auto&& test_driver_str_) {
                           auto& test_driver = test_driver_str_.as_string();
                           if (test_driver == "Catch-Main") {
                               ret.test_driver = test_lib::catch_main;
                           } else if (test_driver == "Catch") {
                               ret.test_driver = test_lib::catch_;
                           } else {
                               auto dym = *did_you_mean(test_driver, {"Catch-Main", "Catch"});
                               throw_user_error<errc::unknown_test_driver>(
                                   "Unknown 'test_driver' '{}' (Did you mean '{}'?)",
                                   test_driver,
                                   dym);
                           }
                           return semester::dc_accept;
                       }},
                [&](auto key, auto&&) {
                    return semester::dc_reject_t{
                        fmt::format("Unknown key `{}` in package manifest", key)};
                }}});
    auto rej = std::get_if<semester::dc_reject_t>(&res);
    if (rej) {
        throw_user_error<errc::invalid_pkg_manifest>(rej->message);
    }

    if (ret.pkg_id.name.empty()) {
        throw_user_error<errc::invalid_pkg_manifest>("The 'name' field is required.");
    }

    if (ret.namespace_.empty()) {
        throw_user_error<errc::invalid_pkg_manifest>("The 'namespace'` field is required.");
    }

    return ret;
}

std::optional<fs::path> package_manifest::find_in_directory(path_ref dirpath) {
    auto cands = {
        "package.json5",
        "package.jsonc",
        "package.json",
    };
    for (auto c : cands) {
        auto cand = dirpath / c;
        if (fs::is_regular_file(cand)) {
            return cand;
        }
    }

    auto dds_fname = dirpath / "package.dds";
    if (fs::is_regular_file(dds_fname)) {
        return dds_fname;
    }
    return std::nullopt;
}

std::optional<package_manifest> package_manifest::load_from_directory(path_ref dirpath) {
    auto found = find_in_directory(dirpath);
    if (!found.has_value()) {
        return std::nullopt;
    }
    if (found->extension() == ".dds") {
        return load_from_dds_file(*found);
    } else {
        return load_from_file(*found);
    }
}