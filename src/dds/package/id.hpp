#pragma once

#include <semver/version.hpp>

#include <string>
#include <string_view>
#include <tuple>

namespace dds {

/**
 * Represents a unique package ID. We store this as a simple name-version pair.
 *
 * In text, this is represented with an `@` symbol in between. The `parse` and
 * `to_string` method convert between this textual representation, and supports
 * full round-trips.
 */
struct package_id {
    /// The name of the package
    std::string name;
    /// The version of the package
    semver::version version;

    /// Default-initialize a package_id with a blank name and a default version
    package_id() = default;
    /// Construct a package ID from a name-version pair
    package_id(std::string_view s, semver::version v);

    /**
     * Parse the given string into a package_id object.
     */
    static package_id parse(std::string_view);

    /**
     * Convert this package_id into its corresponding textual representation.
     * The returned string can be passed back to `parse()` for a round-trip
     */
    std::string to_string() const noexcept;

    friend bool operator<(const package_id& lhs, const package_id& rhs) noexcept {
        return std::tie(lhs.name, lhs.version) < std::tie(rhs.name, rhs.version);
    }
    friend bool operator==(const package_id& lhs, const package_id& rhs) noexcept {
        return std::tie(lhs.name, lhs.version) == std::tie(rhs.name, rhs.version);
    }
};

}  // namespace dds