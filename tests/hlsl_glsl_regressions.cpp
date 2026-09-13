// hlsl_glsl_regressions.cpp — Artemis HLSL-subset -> GLSL translation.

#include "render/hlsl_glsl.h"

#include <iostream>
#include <string>

namespace {

int g_failures = 0;
void Check(bool ok, const std::string &what) {
    if (!ok) { ++g_failures; std::cerr << "FAIL: " << what << "\n"; }
}
bool Has(const std::string &s, const std::string &needle) {
    return s.find(needle) != std::string::npos;
}

void TestTranslate() {
    const std::string hlsl = R"(
sampler2D samplerFore : register(s0);
sampler2D samplerMask : register(s1);

float4 ps(float2 uv : TEXCOORD0) : COLOR
{
    float4 c = tex2D(samplerFore, uv);
    float3 g = lerp(c.rgb, dot(c.rgb, float3(0.3, 0.6, 0.1)), 0.5);
    return saturate(float4(g, c.a) * colorMultiply);
}
)";
    Check(artc::LooksLikeHlsl(hlsl), "detects HLSL");
    const std::string glsl = artc::TranslateHlslToGlsl(hlsl);
    Check(Has(glsl, "uniform sampler2D textureFore;"), "register sampler -> uniform + alias");
    Check(!Has(glsl, "register"), "no register left");
    Check(Has(glsl, "void main()"), "ps -> main");
    Check(Has(glsl, "vec2 uv = resultCoord1;"), "uv bound to varying");
    Check(Has(glsl, "texture2D(textureFore"), "tex2D -> texture2D");
    Check(Has(glsl, "vec4 c"), "float4 -> vec4");
    Check(Has(glsl, "mix(c.rgb"), "lerp -> mix");
    Check(Has(glsl, "vec3(0.3, 0.6, 0.1)"), "float3 -> vec3");
    Check(Has(glsl, "clamp(") && Has(glsl, ", 0.0, 1.0)"), "saturate -> clamp");
    Check(Has(glsl, "gl_FragColor ="), "return -> gl_FragColor");
}

void TestNonHlslUntouched() {
    const std::string glsl = "precision highp float;\nvoid main(){gl_FragColor=vec4(1.0);}";
    Check(!artc::LooksLikeHlsl(glsl), "GLSL not detected as HLSL");
}

} // namespace

int main() {
    TestTranslate();
    TestNonHlslUntouched();
    if (g_failures == 0) std::cout << "hlsl_glsl_regressions: ok\n";
    return g_failures == 0 ? 0 : 1;
}
