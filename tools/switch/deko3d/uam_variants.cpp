// The deko3d shaders compiled on the build machine (tools/switch/deko3d/make_dk_shaders.py --uam-variants), so the
// console finds them in the build's DKSH cache instead of compiling them (docs/SWITCH-deko3d.md).
//
//   uam_variants <uam flags> <job file>
//
// Each line of the job file is "<vert|frag> <GLSL file> <DKSH file>": the GLSL is the exact text plume's deko3d back end
// would compile (the shader with its constants defined after #version), the DKSH is written where the line says.
// One process compiles its whole list (uam initialised once); the script runs several. Exit code 1 if any failed.
//
// Built against thirdparty/uam (uamlib) by tools/switch/host-stage.sh.
#include "uam_api.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

int main(int argc, char **argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <uam flags> <job file>\n", argv[0]);
        return 2;
    }
    uam::setBanjoFlags(unsigned(std::strtoul(argv[1], nullptr, 0)));
    uam::init(true);
    std::ifstream jobs(argv[2]);
    std::string stage, glslPath, dkshPath;
    int failures = 0;
    while (jobs >> stage >> glslPath >> dkshPath) {
        std::ifstream in(glslPath, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        const uam::Result r = uam::compile(stage == "vert" ? uam::Stage::Vertex : uam::Stage::Fragment, text.str().c_str());
        if (!r.ok) {
            std::fprintf(stderr, "uam failed for %s: %s\n", glslPath.c_str(), r.log.c_str());
            ++failures;
            continue;
        }
        std::ofstream out(dkshPath, std::ios::binary);
        out.write(reinterpret_cast<const char *>(r.dksh.data()), std::streamsize(r.dksh.size()));
    }
    uam::shutdown();
    return failures ? 1 : 0;
}
