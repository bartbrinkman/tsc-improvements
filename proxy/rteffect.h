#pragma once

// Render-thread wrapper for D3DX effects (ID3DXEffect, d3dx9_43). The D3DX headers
// are not part of the current SDK, so the wrapper works at the vtable level: hot
// setters and the Begin/Pass/Commit/End sequence are recorded for the render thread,
// metadata lookups pass straight through, and every other method drains the queue
// first and then runs directly.
namespace tscfix::rt {

// Wraps an effect the engine just created; takes over the caller's reference.
void* WrapEffect(void* realEffect);

}  // namespace tscfix::rt
