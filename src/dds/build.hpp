#ifndef DDS_BUILD_HPP_INCLUDED
#define DDS_BUILD_HPP_INCLUDED

#include <dds/package_manifest.hpp>
#include <dds/toolchain.hpp>
#include <dds/util/fs.hpp>

#include <optional>

namespace dds {

struct build_params {
    fs::path       root;
    fs::path       out_root;
    fs::path       lm_index;
    dds::toolchain toolchain;
    bool           do_export       = false;
    bool           build_tests     = false;
    bool           enable_warnings = false;
    bool           build_apps      = false;
    bool           build_deps      = false;
    int            parallel_jobs   = 0;
};

void build(const build_params&, const package_manifest& man);

}  // namespace dds

#endif  // DDS_BUILD_HPP_INCLUDED