#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <iostream>

using Microsoft::WRL::ComPtr;

int main() {
    const char* hlsl = 
        "struct VS_IN { float3 pos : POSITION; float4 col : COLOR; };\n"
        "struct PS_IN { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
        "PS_IN VSMain(VS_IN input) { PS_IN o; o.pos = float4(input.pos, 1.0f); o.col = input.col; return o; }\n"
        "float4 PSMain(PS_IN input) : SV_TARGET { return input.col; }\n";

    ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
    HRESULT hr = D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, "VSMain", "vs_4_0", 0, 0, &vsBlob, &errBlob);
    if (FAILED(hr)) {
        if (errBlob) std::cerr << "VS Compile Error: " << (char*)errBlob->GetBufferPointer() << std::endl;
        return 1;
    }
    hr = D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, "PSMain", "ps_4_0", 0, 0, &psBlob, &errBlob);
    if (FAILED(hr)) {
        if (errBlob) std::cerr << "PS Compile Error: " << (char*)errBlob->GetBufferPointer() << std::endl;
        return 1;
    }
    std::cout << "D3DCompile succeeded! VS bytes: " << vsBlob->GetBufferSize() << ", PS bytes: " << psBlob->GetBufferSize() << std::endl;
    return 0;
}
