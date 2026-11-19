#pragma once
// Method names in vtable order, from the public DirectX 9 SDK headers (d3d9.h, d3dx9effect.h).

namespace tscfix::d3dnames {

inline const char* const kDevice9[] = {
    "QueryInterface", "AddRef", "Release", "TestCooperativeLevel", "GetAvailableTextureMem",
    "EvictManagedResources", "GetDirect3D", "GetDeviceCaps", "GetDisplayMode", "GetCreationParameters",
    "SetCursorProperties", "SetCursorPosition", "ShowCursor", "CreateAdditionalSwapChain", "GetSwapChain",
    "GetNumberOfSwapChains", "Reset", "Present", "GetBackBuffer", "GetRasterStatus", "SetDialogBoxMode",
    "SetGammaRamp", "GetGammaRamp", "CreateTexture", "CreateVolumeTexture", "CreateCubeTexture",
    "CreateVertexBuffer", "CreateIndexBuffer", "CreateRenderTarget", "CreateDepthStencilSurface",
    "UpdateSurface", "UpdateTexture", "GetRenderTargetData", "GetFrontBufferData", "StretchRect", "ColorFill",
    "CreateOffscreenPlainSurface", "SetRenderTarget", "GetRenderTarget", "SetDepthStencilSurface",
    "GetDepthStencilSurface", "BeginScene", "EndScene", "Clear", "SetTransform", "GetTransform",
    "MultiplyTransform", "SetViewport", "GetViewport", "SetMaterial", "GetMaterial", "SetLight", "GetLight",
    "LightEnable", "GetLightEnable", "SetClipPlane", "GetClipPlane", "SetRenderState", "GetRenderState",
    "CreateStateBlock", "BeginStateBlock", "EndStateBlock", "SetClipStatus", "GetClipStatus", "GetTexture",
    "SetTexture", "GetTextureStageState", "SetTextureStageState", "GetSamplerState", "SetSamplerState",
    "ValidateDevice", "SetPaletteEntries", "GetPaletteEntries", "SetCurrentTexturePalette",
    "GetCurrentTexturePalette", "SetScissorRect", "GetScissorRect", "SetSoftwareVertexProcessing",
    "GetSoftwareVertexProcessing", "SetNPatchMode", "GetNPatchMode", "DrawPrimitive", "DrawIndexedPrimitive",
    "DrawPrimitiveUP", "DrawIndexedPrimitiveUP", "ProcessVertices", "CreateVertexDeclaration",
    "SetVertexDeclaration", "GetVertexDeclaration", "SetFVF", "GetFVF", "CreateVertexShader", "SetVertexShader",
    "GetVertexShader", "SetVertexShaderConstantF", "GetVertexShaderConstantF", "SetVertexShaderConstantI",
    "GetVertexShaderConstantI", "SetVertexShaderConstantB", "GetVertexShaderConstantB", "SetStreamSource",
    "GetStreamSource", "SetStreamSourceFreq", "GetStreamSourceFreq", "SetIndices", "GetIndices",
    "CreatePixelShader", "SetPixelShader", "GetPixelShader", "SetPixelShaderConstantF", "GetPixelShaderConstantF",
    "SetPixelShaderConstantI", "GetPixelShaderConstantI", "SetPixelShaderConstantB", "GetPixelShaderConstantB",
    "DrawRectPatch", "DrawTriPatch", "DeletePatch", "CreateQuery",
};
static_assert(sizeof(kDevice9) / sizeof(kDevice9[0]) == 119, "IDirect3DDevice9 has 119 methods");

inline const char* const kEffect[] = {
    "QueryInterface", "AddRef", "Release", "GetDesc", "GetParameterDesc", "GetTechniqueDesc", "GetPassDesc",
    "GetFunctionDesc", "GetParameter", "GetParameterByName", "GetParameterBySemantic", "GetParameterElement",
    "GetTechnique", "GetTechniqueByName", "GetPass", "GetPassByName", "GetFunction", "GetFunctionByName",
    "GetAnnotation", "GetAnnotationByName", "SetValue", "GetValue", "SetBool", "GetBool", "SetBoolArray",
    "GetBoolArray", "SetInt", "GetInt", "SetIntArray", "GetIntArray", "SetFloat", "GetFloat", "SetFloatArray",
    "GetFloatArray", "SetVector", "GetVector", "SetVectorArray", "GetVectorArray", "SetMatrix", "GetMatrix",
    "SetMatrixArray", "GetMatrixArray", "SetMatrixPointerArray", "GetMatrixPointerArray", "SetMatrixTranspose",
    "GetMatrixTranspose", "SetMatrixTransposeArray", "GetMatrixTransposeArray", "SetMatrixTransposePointerArray",
    "GetMatrixTransposePointerArray", "SetString", "GetString", "SetTexture", "GetTexture", "GetPixelShader",
    "GetVertexShader", "SetArrayRange", "GetPool", "SetTechnique", "GetCurrentTechnique", "ValidateTechnique",
    "FindNextValidTechnique", "IsParameterUsed", "Begin", "BeginPass", "CommitChanges", "EndPass", "End",
    "GetDevice", "OnLostDevice", "OnResetDevice", "SetStateManager", "GetStateManager", "BeginParameterBlock",
    "EndParameterBlock", "ApplyParameterBlock", "DeleteParameterBlock", "CloneEffect", "SetRawValue",
};
static_assert(sizeof(kEffect) / sizeof(kEffect[0]) == 79, "ID3DXEffect (d3dx9_43) has 79 methods");

}  // namespace tscfix::d3dnames
