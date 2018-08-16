
#include <migraph/onnx.hpp>

#include <migraph/gpu/target.hpp>
#include <migraph/gpu/hip.hpp>
#include <migraph/generate.hpp>
#include <migraph/verify.hpp>

migraph::program::parameter_map create_param_map(const migraph::program& p, bool gpu=true)
{
    migraph::program::parameter_map m;
    for(auto&& x : p.get_parameter_shapes())
    {
        if(gpu)
            m[x.first] = migraph::gpu::to_gpu(migraph::generate_argument(x.second));
        else
            m[x.first] = migraph::generate_argument(x.second);
    }
    return m;
}

int main(int argc, char const* argv[])
{
    if(argc > 1)
    {
        std::string file = argv[1];
        auto p = migraph::parse_onnx(file);
        p.compile(migraph::gpu::target{});
        auto m = create_param_map(p);
        p.perf_report(std::cout, 10, m);
    }
}