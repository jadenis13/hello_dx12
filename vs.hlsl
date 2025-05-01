struct VS_INPUT {
    float4 position : POSITION;
    float2 texcoord : TEXCOORD;
};

struct VS_OUTPUT {
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD;
};

cbuffer ConstantBuffer : register(b0) {
    float4x4 wvpMatrix;
}

VS_OUTPUT main(VS_INPUT input) {
    VS_OUTPUT output;
    output.position = mul(input.position, wvpMatrix);
    output.texcoord = input.texcoord;
    return output;
}