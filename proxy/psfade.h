#pragma once
#include <cstdint>
#include <vector>

// LOD fade, stage 2: a screen-door discard added to a compiled pixel shader (shader model 3).
//
// The patched shader starts with:
//     dcl vPos.xy                         (unless the shader has it)
//     def cK, 0.06711056, 0.00583715, 52.9829189, 0
//     dp2add rT, vPos, cK, cK.w           noise = frac(52.98 * frac(dot(vPos.xy, cK.xy)))
//     frc rT, rT
//     mul rT, rT, cK.z
//     frc rT, rT
//     mad rT, rT, cF.y, cF.x              cF = (offset, sign, -, -)
//     texkill rT
// cF is register c223. With cF = (0, 0) nothing is discarded. With (-t, 1) the pixels whose noise is
// below t are discarded (an object that fades out), with (t, -1) the pixels above t (the object
// that fades in): the two cover complementary pixels.
namespace tscfix::psfade {

constexpr int kFadeRegister = 223;  // cF
constexpr int kNoiseRegister = 222; // cK (defined in the shader itself)

enum class Result { Patched, NotModel3, RegisterInUse, NoFreeTemp, Malformed };

// `code` is a complete pixel shader (version token to end token). On Patched, `out` holds the new
// shader. Otherwise `out` is untouched.
Result Patch(const uint32_t* code, std::vector<uint32_t>* out);
// Number of tokens of a shader including the end token, or 0 when no end token is found in `limit` tokens.
size_t Length(const uint32_t* code, size_t limit);

}  // namespace tscfix::psfade
