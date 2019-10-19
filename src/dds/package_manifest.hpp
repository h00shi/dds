#pragma once

#include <dds/util/fs.hpp>

#include <string>

namespace dds {

struct package_manifest {
    std::string name;

    static package_manifest load_from_file(path_ref);
};

} // namespace dds