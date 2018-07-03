#ifndef GUARD_MIGRAPHLIB_ONNX_HPP
#define GUARD_MIGRAPHLIB_ONNX_HPP

#include <migraph/program.hpp>

namespace migraph {

program parse_onnx(const std::string& name);

} // namespace migraph

#endif