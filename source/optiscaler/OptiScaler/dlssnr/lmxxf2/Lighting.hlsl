// [lmxxf2] Iluminação: renodx::tonemap::config::Apply (RenoDX, clshortfuse, MIT) sobre a cor linear depois da rede e do
// Appearance filter, antes do upscaler. A pré-exposição do jogo sai antes e volta depois, para o ponto médio 0.18 valer.
// Compilado fora do jogo com o DXC (HLSL 2021: o RenoDX usa select()), ver build_lighting.ps1.
#include "tonemap.hlsl"

Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
cbuffer Controls : register(b0) {
  uint w; uint h; uint ToneMapper; uint HueMethod;
  float PreExposure; float Exposure; float Highlights; float Shadows;
  float Contrast; float Saturation; float Dechroma; float Flare;
  float Blowout; float HueStrength; float PeakNits; float GameNits;
  float MidGrayNits; float GammaCorrection; float Mix; float pad;
};

[numthreads(8, 8, 1)] void main(uint3 t : SV_DispatchThreadID) {
  if (t.x >= w || t.y >= h) return;
  const float4 original = src.Load(int3(t.xy, 0));
  const float pre = PreExposure > 0 ? PreExposure : 1;
  const float type = ToneMapper == 1 ? renodx::tonemap::config::type::RENODRT
                   : ToneMapper == 2 ? renodx::tonemap::config::type::ACES
                                     : renodx::tonemap::config::type::NONE;
  const renodx::tonemap::Config cfg = renodx::tonemap::config::Create(
      type, PeakNits, GameNits, GammaCorrection, Exposure, Highlights, Shadows, Contrast, Saturation, 0.18f, MidGrayNits,
      1.f, 1.f, 1.f, 1.f, Dechroma, Flare, renodx::tonemap::config::hue_correction_type::INPUT, HueStrength, 0.f,
      (float)HueMethod, renodx::tonemap::renodrt::config::tone_map_method::DANIELE, 0.f, false, Blowout);
  float3 c = renodx::tonemap::config::Apply(max(original.rgb, 0) / pre, cfg);
  // RenoDRT/ACES devolvem 1.0 = brilho do jogo; volta para a escala linear do jogo (ponto médio no mesmo lugar)
  if (ToneMapper != 0) c *= GameNits / MidGrayNits * 0.18f;
  c = lerp(original.rgb, c * pre, saturate(Mix));
  dst[t.xy] = float4(clamp(c, -65504, 65504), original.a);
}
