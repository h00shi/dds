#pragma once

#include <dds/source/dist.hpp>
#include <dds/temp.hpp>

namespace dds {

struct package_info;

struct temporary_sdist {
    temporary_dir tmpdir;
    dds::sdist    sdist;
};

temporary_sdist get_package_sdist(const package_info&);

}  // namespace dds