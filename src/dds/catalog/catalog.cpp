#include "./catalog.hpp"

#include <dds/dym.hpp>
#include <dds/error/errors.hpp>
#include <dds/solve/solve.hpp>

#include <neo/sqlite3/exec.hpp>
#include <neo/sqlite3/iter_tuples.hpp>
#include <neo/sqlite3/single.hpp>
#include <nlohmann/json.hpp>
#include <range/v3/range/conversion.hpp>
#include <range/v3/view/join.hpp>
#include <range/v3/view/transform.hpp>

#include <spdlog/spdlog.h>

using namespace dds;

namespace sqlite3 = neo::sqlite3;
using namespace sqlite3::literals;

namespace {

void migrate_repodb_1(sqlite3::database& db) {
    db.exec(R"(
        CREATE TABLE dds_cat_pkgs (
            pkg_id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL,
            version TEXT NOT NULL,
            git_url TEXT,
            git_ref TEXT,
            lm_name TEXT,
            lm_namespace TEXT,
            description TEXT NOT NULL,
            UNIQUE(name, version),
            CONSTRAINT has_source_info CHECK(
                (
                    git_url NOT NULL
                    AND git_ref NOT NULL
                )
                = 1
            ),
            CONSTRAINT valid_lm_info CHECK(
                (
                    lm_name NOT NULL
                    AND lm_namespace NOT NULL
                )
                +
                (
                    lm_name ISNULL
                    AND lm_namespace ISNULL
                )
                = 1
            )
        );

        CREATE TABLE dds_cat_pkg_deps (
            dep_id INTEGER PRIMARY KEY AUTOINCREMENT,
            pkg_id INTEGER NOT NULL REFERENCES dds_cat_pkgs(pkg_id),
            dep_name TEXT NOT NULL,
            low TEXT NOT NULL,
            high TEXT NOT NULL,
            UNIQUE(pkg_id, dep_name)
        );
    )");
}

void ensure_migrated(sqlite3::database& db) {
    sqlite3::transaction_guard tr{db};
    db.exec(R"(
        PRAGMA foreign_keys = 1;
        CREATE TABLE IF NOT EXISTS dds_cat_meta AS
            WITH init(meta) AS (VALUES ('{"version": 0}'))
            SELECT * FROM init;
    )");
    auto meta_st     = db.prepare("SELECT meta FROM dds_cat_meta");
    auto [meta_json] = sqlite3::unpack_single<std::string>(meta_st);

    auto meta = nlohmann::json::parse(meta_json);
    if (!meta.is_object()) {
        throw_external_error<errc::corrupted_catalog_db>();
    }

    auto version_ = meta["version"];
    if (!version_.is_number_integer()) {
        throw_external_error<errc::corrupted_catalog_db>(
            "The catalog database metadata is invalid [bad dds_meta.version]");
    }

    constexpr int current_database_version = 1;

    int version = version_;
    if (version > current_database_version) {
        throw_external_error<errc::catalog_too_new>();
    }

    if (version < 1) {
        migrate_repodb_1(db);
    }
    meta["version"] = 1;
    exec(db, "UPDATE dds_cat_meta SET meta=?", std::forward_as_tuple(meta.dump()));
}

}  // namespace

catalog catalog::open(const std::string& db_path) {
    if (db_path != ":memory:") {
        fs::create_directories(fs::weakly_canonical(db_path).parent_path());
    }
    auto db = sqlite3::database::open(db_path);
    try {
        ensure_migrated(db);
    } catch (const sqlite3::sqlite3_error& e) {
        spdlog::critical(
            "Failed to load the repository database. It appears to be invalid/corrupted. The "
            "exception message is: {}",
            e.what());
        throw_external_error<errc::corrupted_catalog_db>();
    }
    return catalog(std::move(db));
}

catalog::catalog(sqlite3::database db)
    : _db(std::move(db)) {}

void catalog::_store_pkg(const package_info& pkg, const git_remote_listing& git) {
    auto lm_usage = git.auto_lib.value_or(lm::usage{});
    sqlite3::exec(  //
        _stmt_cache,
        R"(
            INSERT OR REPLACE INTO dds_cat_pkgs (
                name,
                version,
                git_url,
                git_ref,
                lm_name,
                lm_namespace,
                description
            ) VALUES (
                ?1,
                ?2,
                ?3,
                ?4,
                CASE WHEN ?5 = '' THEN NULL ELSE ?5 END,
                CASE WHEN ?6 = '' THEN NULL ELSE ?6 END,
                ?7
            )
        )"_sql,
        std::forward_as_tuple(  //
            pkg.ident.name,
            pkg.ident.version.to_string(),
            git.url,
            git.ref,
            lm_usage.name,
            lm_usage.namespace_,
            pkg.description));
}

void catalog::store(const package_info& pkg) {
    sqlite3::transaction_guard tr{_db};

    std::visit([&](auto&& remote) { _store_pkg(pkg, remote); }, pkg.remote);

    auto  db_pkg_id  = _db.last_insert_rowid();
    auto& new_dep_st = _stmt_cache(R"(
        INSERT INTO dds_cat_pkg_deps (
            pkg_id,
            dep_name,
            low,
            high
        ) VALUES (
            ?,
            ?,
            ?,
            ?
        )
    )"_sql);
    for (const auto& dep : pkg.deps) {
        new_dep_st.reset();
        assert(dep.versions.num_intervals() == 1);
        auto iv_1 = *dep.versions.iter_intervals().begin();
        sqlite3::exec(new_dep_st,
                      std::forward_as_tuple(db_pkg_id,
                                            dep.name,
                                            iv_1.low.to_string(),
                                            iv_1.high.to_string()));
    }
}

std::optional<package_info> catalog::get(const package_id& pk_id) const noexcept {
    auto& st = _stmt_cache(R"(
        SELECT
            pkg_id,
            name,
            version,
            git_url,
            git_ref,
            lm_name,
            lm_namespace,
            description
        FROM dds_cat_pkgs
        WHERE name = ? AND version = ?
    )"_sql);
    st.reset();
    st.bindings  = std::forward_as_tuple(pk_id.name, pk_id.version.to_string());
    auto opt_tup = sqlite3::unpack_single_opt<std::int64_t,
                                              std::string,
                                              std::string,
                                              std::optional<std::string>,
                                              std::optional<std::string>,
                                              std::optional<std::string>,
                                              std::optional<std::string>,
                                              std::string>(st);
    if (!opt_tup) {
        dym_target::fill([&] {
            auto all_ids = this->all();
            auto id_strings
                = ranges::views::transform(all_ids, [&](auto id) { return id.to_string(); });
            return did_you_mean(pk_id.to_string(), id_strings);
        });
        return std::nullopt;
    }
    const auto& [pkg_id, name, version, git_url, git_ref, lm_name, lm_namespace, description]
        = *opt_tup;
    assert(pk_id.name == name);
    assert(pk_id.version == semver::version::parse(version));
    assert(git_url);
    assert(git_ref);

    auto deps = dependencies_of(pk_id);

    return package_info{
        pk_id,
        std::move(deps),
        std::move(description),
        git_remote_listing{
            *git_url,
            *git_ref,
            lm_name ? std::make_optional(lm::usage{*lm_namespace, *lm_name}) : std::nullopt,
        },
    };
}

auto pair_to_pkg_id = [](auto&& pair) {
    const auto& [name, ver] = pair;
    return package_id{name, semver::version::parse(ver)};
};

std::vector<package_id> catalog::all() const noexcept {
    return sqlite3::exec_iter<std::string, std::string>(  //
               _stmt_cache,
               "SELECT name, version FROM dds_cat_pkgs"_sql)
        | ranges::views::transform(pair_to_pkg_id)  //
        | ranges::to_vector;
}

std::vector<package_id> catalog::by_name(std::string_view sv) const noexcept {
    return sqlite3::exec_iter<std::string, std::string>(  //
               _stmt_cache,
               R"(
                SELECT name, version
                  FROM dds_cat_pkgs
                 WHERE name = ?
                )"_sql,
               std::tie(sv))                        //
        | ranges::views::transform(pair_to_pkg_id)  //
        | ranges::to_vector;
}

std::vector<dependency> catalog::dependencies_of(const package_id& pkg) const noexcept {
    return sqlite3::exec_iter<std::string,
                              std::string,
                              std::string>(  //
               _stmt_cache,
               R"(
                WITH this_pkg_id AS (
                    SELECT pkg_id
                      FROM dds_cat_pkgs
                     WHERE name = ? AND version = ?
                )
                SELECT dep_name, low, high
                  FROM dds_cat_pkg_deps
                 WHERE pkg_id IN this_pkg_id
              ORDER BY dep_name
                )"_sql,
               std::forward_as_tuple(pkg.name, pkg.version.to_string()))  //
        | ranges::views::transform([](auto&& pair) {
               auto& [name, low, high] = pair;
               return dependency{name, {semver::version::parse(low), semver::version::parse(high)}};
           })  //
        | ranges::to_vector;
}

namespace {

void check_json(bool b, std::string_view what) {
    if (!b) {
        throw_user_error<errc::invalid_catalog_json>("Catalog JSON is invalid: {}", what);
    }
}

}  // namespace

void catalog::import_json_str(std::string_view content) {
    using nlohmann::json;

    auto root = json::parse(content);
    check_json(root.is_object(), "Root of JSON must be an object (key-value mapping)");

    auto version = root["version"];
    check_json(version.is_number_integer(), "/version must be an integral value");
    check_json(version <= 1, "/version is too new. We don't know how to parse this.");

    auto packages = root["packages"];
    check_json(packages.is_object(), "/packages must be an object");

    sqlite3::transaction_guard tr{_db};

    for (const auto& [pkg_name_, versions_map] : packages.items()) {
        std::string pkg_name = pkg_name_;
        check_json(versions_map.is_object(),
                   fmt::format("/packages/{} must be an object", pkg_name));

        for (const auto& [version_, pkg_info] : versions_map.items()) {
            auto version = semver::version::parse(version_);
            check_json(pkg_info.is_object(),
                       fmt::format("/packages/{}/{} must be an object", pkg_name, version_));

            package_info info{{pkg_name, version}, {}, {}, {}};
            auto         deps = pkg_info["depends"];

            if (!deps.is_null()) {
                check_json(deps.is_object(),
                           fmt::format("/packages/{}/{}/depends must be an object",
                                       pkg_name,
                                       version_));

                for (const auto& [dep_name, dep_version] : deps.items()) {
                    check_json(dep_version.is_string(),
                               fmt::format("/packages/{}/{}/depends/{} must be a string",
                                           pkg_name,
                                           version_,
                                           dep_name));
                    auto range = semver::range::parse(std::string(dep_version));
                    info.deps.push_back({
                        std::string(dep_name),
                        {range.low(), range.high()},
                    });
                }
            }

            auto git_remote = pkg_info["git"];
            if (!git_remote.is_null()) {
                check_json(git_remote.is_object(), "`git` must be an object");
                std::string              url      = git_remote["url"];
                std::string              ref      = git_remote["ref"];
                auto                     lm_usage = git_remote["auto-lib"];
                std::optional<lm::usage> autolib;
                if (!lm_usage.is_null()) {
                    autolib = lm::split_usage_string(std::string(lm_usage));
                }
                info.remote = git_remote_listing{url, ref, autolib};
            } else {
                throw_user_error<errc::no_catalog_remote_info>("No remote info for /packages/{}/{}",
                                                               pkg_name,
                                                               version_);
            }

            auto desc_ = pkg_info["description"];
            if (!desc_.is_null()) {
                check_json(desc_.is_string(), "`description` must be a string");
                info.description = desc_;
            }

            store(info);
        }
    }
}
