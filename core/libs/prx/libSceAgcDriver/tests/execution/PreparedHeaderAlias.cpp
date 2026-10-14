#include "VulkanTestDevice.hpp"
#include "SceShaders.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include <array>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<typename TAction>
void ExpectFailure(TAction action, const char* expected) {
    try {
        action();
    } catch (const std::exception& error) {
        Require(std::string(error.what()).find(expected) != std::string::npos, error.what());
        return;
    }
    throw std::runtime_error("expected header alias failure");
}

// Bound header copies of one shader binary: the same code with different
// header addresses must resolve when every compared field matches, even
// when user-data presence differs (a vertex copy bound with user_data null
// while registered with a table, and the reverse). Anything else —
// changed target, relocated-to-garbage pointers, unreadable or unknown
// headers — must still be refused.
void HeaderAlias() {
    alignas(256) std::array<std::uint32_t, 1> code{0xbf810000u};
    struct Header {
        Shader shader{};
        std::array<ShaderRegister, 9> registers{};
        std::array<ShaderRegister, 2> context{{{0x1b6, 0}, {0x1b6, 1}}};
        ShaderSpecialRegs specials{};
        ShaderUserData users{};
    } header;
    const auto address = reinterpret_cast<std::uintptr_t>(code.data());
    header.shader.file_header = 0x34333231u;
    header.shader.version = 0x18;
    header.shader.header_size = sizeof(header);
    header.shader.shader_size = sizeof(code);
    header.shader.code = code.data();
    header.shader.sh_registers = header.registers.data();
    header.shader.num_sh_registers = header.registers.size();
    header.shader.cx_registers = header.context.data();
    header.shader.num_cx_registers = header.context.size();
    header.shader.specials = &header.specials;
    header.specials.dispatch_modifier = 0x8000;
    header.shader.type = 0;
    header.registers = {{{0x20c, static_cast<std::uint32_t>(address >> 8u)}, {0x20d, static_cast<std::uint32_t>(address >> 40u)}, {0x207, 2}, {0x208, 1}, {0x209, 1}, {0x212, 0}, {0x213, 0}, {0x207, 1}, {0x207, 1}}};
    AgcDriverRegisterShader_nid_postfix(&header.shader);
    AgcDriverResolveShaderAbi_nid_postfix(&header.shader, {}, {});
    Shader identical = header.shader;
    AgcDriverResolveShaderAbi_nid_postfix(&identical, {}, {});
    Shader setCopy = header.shader;
    setCopy.user_data = &header.users;
    AgcDriverResolveShaderAbi_nid_postfix(&setCopy, {}, {});
    alignas(Shader) std::array<std::byte, sizeof(Shader)> padded{};
    std::memcpy(padded.data(), &setCopy, sizeof(Shader));
    padded.back() = std::byte{0x7d};
    AgcDriverResolveShaderAbi_nid_postfix(reinterpret_cast<Shader*>(padded.data()), {}, {});
    Shader badTarget = header.shader;
    badTarget.target ^= 1u;
    ExpectFailure([&] { AgcDriverResolveShaderAbi_nid_postfix(&badTarget, {}, {}); }, "replaced shader header");
    Header moved = header;
    moved.shader.sh_registers = moved.registers.data();
    moved.shader.cx_registers = moved.context.data();
    moved.shader.specials = &moved.specials;
    AgcDriverResolveShaderAbi_nid_postfix(&moved.shader, {}, {});
    moved.shader.specials = &moved.specials + 1;
    ExpectFailure([&] { AgcDriverResolveShaderAbi_nid_postfix(&moved.shader, {}, {}); }, "replaced shader header");
    Shader unmapped = header.shader;
    unmapped.sh_registers = reinterpret_cast<ShaderRegister*>(0x12340000u);
    ExpectFailure([&] { AgcDriverResolveShaderAbi_nid_postfix(&unmapped, {}, {}); }, "replaced shader header");
    Shader wrongCode = header.shader;
    wrongCode.code = reinterpret_cast<const void*>(0x12340000u);
    ExpectFailure([&] { AgcDriverResolveShaderAbi_nid_postfix(&wrongCode, {}, {}); }, "unregistered shader");
    header.shader.user_data = &header.users;
    AgcDriverRegisterShader_nid_postfix(&header.shader);
    Shader noUserData = header.shader;
    noUserData.user_data = nullptr;
    AgcDriverResolveShaderAbi_nid_postfix(&noUserData, {}, {});
}

} // namespace

int main() {
    try {
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        device.reset();
        HeaderAlias();
        // Drain background compile work and shut the driver down in order:
        // returning with workers in flight tears down Vulkan underneath
        // them on some drivers.
        AgcDriverWaitIdle_nid_postfix();
        AgcDriverShutdown_nid_postfix();
        std::cout << "header alias tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try {
            AgcDriverShutdown_nid_postfix();
        } catch (const std::exception& shutdownError) {
            std::cerr << shutdownError.what() << '\n';
        }
        return 1;
    }
}
