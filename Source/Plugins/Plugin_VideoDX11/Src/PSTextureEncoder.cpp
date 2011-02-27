// Copyright (C) 2003 Dolphin Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official SVN repository and contact information can be found at
// http://code.google.com/p/dolphin-emu/

#include "PSTextureEncoder.h"

#include "D3DBase.h"
#include "D3DShader.h"
#include "GfxState.h"
#include "BPMemory.h"
#include "FramebufferManager.h"
#include "Render.h"
#include "HW/Memmap.h"
#include "TextureCache.h"

// "Static mode" will compile a new EFB encoder shader for every combination of
// encoding configurations. It's compatible with Shader Model 4.

// "Dynamic mode" will use the dynamic-linking feature of Shader Model 5. Only
// one shader needs to be compiled.

// Unfortunately, the June 2010 DirectX SDK includes a broken HLSL compiler
// which cripples dynamic linking for us.
// See <http://www.gamedev.net/topic/587232-dx11-dynamic-linking-compilation-warnings/>.
// Dynamic mode is disabled for now. To enable it, uncomment the line below.

//#define USE_DYNAMIC_MODE

// FIXME: When Microsoft fixes their HLSL compiler, make Dolphin enable dynamic
// mode on Shader Model 5-compatible cards.

namespace DX11
{
	
union EFBEncodeParams
{
	struct
	{
		UINT NumHalfCacheLinesX;
		UINT NumBlocksY;
		UINT PosX;
		UINT PosY;
		FLOAT TexLeft;
		FLOAT TexTop;
		FLOAT TexRight;
		FLOAT TexBottom;
	};
	// Constant buffers must be a multiple of 16 bytes in size.
	u8 pad[32]; // Should be at least the size of the struct above
};

static const char EFB_ENCODE_VS[] =
"// dolphin-emu EFB encoder vertex shader\n"

"uniform struct\n" // Should match EFBEncodeParams above
"{\n"
	"uint NumHalfCacheLinesX;\n"
	"uint NumBlocksY;\n"
	"uint PosX;\n" // Upper-left corner of source
	"uint PosY;\n"
	"float TexLeft;\n" // Rectangle within EFBTexture representing the actual EFB (normalized)
	"float TexTop;\n"
	"float TexRight;\n"
	"float TexBottom;\n"
"} Params : register(c0);\n"

"struct Output\n"
"{\n"
	"float4 Pos : SV_Position;\n"
	"float2 Coord : ENCODECOORD;\n"
"};\n"

"Output main(in float2 Pos : POSITION)\n"
"{\n"
	"Output result;\n"
	"result.Pos = float4(2*Pos.x-1, -2*Pos.y+1, 0.0, 1.0);\n"
	"result.Coord = Pos * float2(Params.NumHalfCacheLinesX, Params.NumBlocksY);\n"
	"return result;\n"
"}\n"
;

static const char EFB_ENCODE_PS[] =
"// dolphin-emu EFB encoder pixel shader\n"

// Input

"uniform struct\n" // Should match EFBEncodeParams above
"{\n"
	"uint NumHalfCacheLinesX;\n"
	"uint NumBlocksY;\n"
	"uint PosX;\n" // Upper-left corner of source
	"uint PosY;\n"
	"float TexLeft;\n" // Rectangle within EFBTexture representing the actual EFB (normalized)
	"float TexTop;\n"
	"float TexRight;\n"
	"float TexBottom;\n"
"} Params : register(c0);\n"

"Texture2D EFBTexture : register(t0);\n"
"sampler EFBSampler : register(s0);\n"

// Constants

"static const float2 INV_EFB_DIMS = float2(1.0/640.0, 1.0/528.0);\n"

// FIXME: Is this correct?
"static const float3 INTENSITY_COEFFS = float3(0.257, 0.504, 0.098);\n"
"static const float INTENSITY_ADD = 16.0/255.0;\n"

// Utility functions

"uint ExtractA(uint pixel) { return pixel >> 24; }\n"
"uint ExtractA1(uint pixel) { return ExtractA(pixel) >> 7; }\n"
"uint ExtractA3(uint pixel) { return ExtractA(pixel) >> 5; }\n"
"uint ExtractR(uint pixel) { return (pixel >> 16) & 0xFF; }\n"
"uint ExtractR4(uint pixel) { return ExtractR(pixel) >> 4; }\n"
"uint ExtractR5(uint pixel) { return ExtractR(pixel) >> 3; }\n"
"uint ExtractG(uint pixel) { return (pixel >> 8) & 0xFF; }\n"
"uint ExtractG4(uint pixel) { return ExtractG(pixel) >> 4; }\n"
"uint ExtractG5(uint pixel) { return ExtractG(pixel) >> 3; }\n"
"uint ExtractG6(uint pixel) { return ExtractG(pixel) >> 2; }\n"
"uint ExtractB(uint pixel) { return pixel & 0xFF; }\n"
"uint ExtractB4(uint pixel) { return ExtractB(pixel) >> 4; }\n"
"uint ExtractB5(uint pixel) { return ExtractB(pixel) >> 3; }\n"

"uint Swap32(uint v) {\n"
	"return (((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000));\n"
"}\n"

"uint UINT_8888(uint a, uint b, uint c, uint d) {\n"
	"return (a << 24) | (b << 16) | (c << 8) | d;\n"
"}\n"

"uint UINT_44444444(uint a, uint b, uint c, uint d, uint e, uint f, uint g, uint h) {\n"
	"return (a << 28) | (b << 24) | (c << 20) | (d << 16) | (e << 12) | (f << 8) | (g << 4) | h;\n"
"}\n"

"uint UINT_1555(uint a, uint b, uint c, uint d) {\n"
	"return (a << 15) | (b << 10) | (c << 5) | d;\n"
"}\n"

"uint UINT_3444(uint a, uint b, uint c, uint d) {\n"
	"return (a << 12) | (b << 8) | (c << 4) | d;\n"
"}\n"

"uint UINT_565(uint a, uint b, uint c) {\n"
	"return (a << 11) | (b << 5) | c;\n"
"}\n"

"uint UINT_1616(uint a, uint b) {\n"
	"return (a << 16) | b;\n"
"}\n"

"uint EncodeRGB5A3(uint pixel) {\n"
	"if (ExtractA(pixel) >= 224) {\n"
		// Encode to ARGB1555
		"return UINT_1555(ExtractA1(pixel), ExtractR5(pixel), ExtractG5(pixel), ExtractB5(pixel));\n"
	"} else {\n"
		// Encode to ARGB3444
		"return UINT_3444(ExtractA3(pixel), ExtractR4(pixel), ExtractG4(pixel), ExtractB4(pixel));\n"
	"}\n"
"}\n"

"uint EncodeRGB565(uint pixel) {\n"
	"return UINT_565(ExtractR5(pixel), ExtractG6(pixel), ExtractB5(pixel));\n"
"}\n"

"float2 CalcTexCoord(uint2 coord)\n"
"{\n"
	// Add 0.5,0.5 to sample from the center of the EFB pixel
	"float2 efbCoord = float2(coord) + float2(0.5,0.5);\n"
	"return lerp(float2(Params.TexLeft,Params.TexTop), float2(Params.TexRight,Params.TexBottom), efbCoord * INV_EFB_DIMS);\n"
"}\n"

// Interface and classes for different source formats

"float4 Fetch_0(uint2 coord)\n"
"{\n"
	"float2 texCoord = CalcTexCoord(coord);\n"
	"float4 result = EFBTexture.Sample(EFBSampler, texCoord);\n"
	"result.a = 1.0;\n"
	"return result;\n"
"}\n"

"float4 Fetch_1(uint2 coord)\n"
"{\n"
	"float2 texCoord = CalcTexCoord(coord);\n"
	"return EFBTexture.Sample(EFBSampler, texCoord);\n"
"}\n"

"float4 Fetch_2(uint2 coord)\n"
"{\n"
	"float2 texCoord = CalcTexCoord(coord);\n"
	"float4 result = EFBTexture.Sample(EFBSampler, texCoord);\n"
	"result.a = 1.0;\n"
	"return result;\n"
"}\n"

"float4 Fetch_3(uint2 coord)\n"
"{\n"
	"float2 texCoord = CalcTexCoord(coord);\n"
	// Ref: <http://www.horde3d.org/forums/viewtopic.php?f=1&t=569>
	// Ref: <http://code.google.com/p/dolphin-emu/source/detail?r=6217>
	"float depth = 255.99998474121094 * EFBTexture.Sample(EFBSampler, texCoord).r;\n"
	"float4 result = depth.rrrr;\n"

	"result.a = floor(result.a);\n" // bits 31..24

	"result.rgb -= result.a;\n"
	"result.rgb *= 256.0;\n"
	"result.r = floor(result.r);\n" // bits 23..16

	"result.gb -= result.r;\n"
	"result.gb *= 256.0;\n"
	"result.g = floor(result.g);\n" // bits 15..8

	"result.b -= result.g;\n"
	"result.b *= 256.0;\n"
	"result.b = floor(result.b);\n" // bits 7..0

	"result = float4(result.arg / 255.0, 1.0);\n"
	"return result;\n"
"}\n"

"#ifdef DYNAMIC_MODE\n"
"interface iFetch\n"
"{\n"
	"float4 Fetch(uint2 coord);\n"
"};\n"

// Source format 0
"class cFetch_0 : iFetch\n"
"{\n"
	"float4 Fetch(uint2 coord)\n"
	"{ return Fetch_0(coord); }\n"
"};\n"


// Source format 1
"class cFetch_1 : iFetch\n"
"{\n"
	"float4 Fetch(uint2 coord)\n"
	"{ return Fetch_1(coord); }\n"
"};\n"

// Source format 2
"class cFetch_2 : iFetch\n"
"{\n"
	"float4 Fetch(uint2 coord)\n"
	"{ return Fetch_2(coord); }\n"
"};\n"

// Source format 3
"class cFetch_3 : iFetch\n"
"{\n"
	"float4 Fetch(uint2 coord)\n"
	"{ return Fetch_3(coord); }\n"
"};\n"

// Declare fetch interface; must be set by application
"iFetch g_fetch;\n"
"#define IMP_FETCH g_fetch.Fetch\n"

"#endif\n" // #ifdef DYNAMIC_MODE

"#ifndef IMP_FETCH\n"
"#error No Fetch specified\n"
"#endif\n"

// Interface and classes for different intensity settings (on or off)

"float4 Intensity_0(float4 sample)\n"
"{\n"
	"return sample;\n"
"}\n"

"float4 Intensity_1(float4 sample)\n"
"{\n"
	"sample.r = dot(INTENSITY_COEFFS, sample.rgb) + INTENSITY_ADD;\n"
	// FIXME: Is this correct? What happens if you use one of the non-R
	// formats with intensity on?
	"sample = sample.rrrr;\n"
	"return sample;\n"
"}\n"

"#ifdef DYNAMIC_MODE\n"
"interface iIntensity\n"
"{\n"
	"float4 Intensity(float4 sample);\n"
"};\n"

// Intensity off
"class cIntensity_0 : iIntensity\n"
"{\n"
	"float4 Intensity(float4 sample)\n"
	"{ return Intensity_0(sample); }\n"
"};\n"

// Intensity on
"class cIntensity_1 : iIntensity\n"
"{\n"
	"float4 Intensity(float4 sample)\n"
	"{ return Intensity_1(sample); }\n"
"};\n"

// Declare intensity interface; must be set by application
"iIntensity g_intensity;\n"
"#define IMP_INTENSITY g_intensity.Intensity\n"

"#endif\n" // #ifdef DYNAMIC_MODE

"#ifndef IMP_INTENSITY\n"
"#error No Intensity specified\n"
"#endif\n"


// Interface and classes for different scale/filter settings (on or off)

"float4 ScaledFetch_0(uint2 coord)\n"
"{\n"
	"return IMP_FETCH(uint2(Params.PosX,Params.PosY) + coord);\n"
"}\n"

"float4 ScaledFetch_1(uint2 coord)\n"
"{\n"
	"uint2 ul = uint2(Params.PosX,Params.PosY) + 2*coord;\n"
	"float4 sample0 = IMP_FETCH(ul+uint2(0,0));\n"
	"float4 sample1 = IMP_FETCH(ul+uint2(1,0));\n"
	"float4 sample2 = IMP_FETCH(ul+uint2(0,1));\n"
	"float4 sample3 = IMP_FETCH(ul+uint2(1,1));\n"
	// Average all four samples together
	// FIXME: Is this correct?
	"return 0.25 * (sample0+sample1+sample2+sample3);\n"
"}\n"

"#ifdef DYNAMIC_MODE\n"
"interface iScaledFetch\n"
"{\n"
	"float4 ScaledFetch(uint2 coord);\n"
"};\n"

// Scale off
"class cScaledFetch_0 : iScaledFetch\n"
"{\n"
	"float4 ScaledFetch(uint2 coord)\n"
	"{ return ScaledFetch_0(coord); }\n"
"};\n"

// Scale on
"class cScaledFetch_1 : iScaledFetch\n"
"{\n"
	"float4 ScaledFetch(uint2 coord)\n"
	"{ return ScaledFetch_1(coord); }\n"
"};\n"

// Declare scaled fetch interface; must be set by application code
"iScaledFetch g_scaledFetch;\n"
"#define IMP_SCALEDFETCH g_scaledFetch.ScaledFetch\n"

"#endif\n" // #ifdef DYNAMIC_MODE

"#ifndef IMP_SCALEDFETCH\n"
"#error No ScaledFetch specified\n"
"#endif\n"

// Main EFB-sampling function: performs all steps of fetching pixels, scaling,
// applying intensity function

"uint SampleEFB(uint2 coord)\n"
"{\n"
	// FIXME: Does intensity happen before or after scaling? Or does
	// it matter?
	"float4 sample = IMP_SCALEDFETCH(coord);\n"
	"sample = IMP_INTENSITY(sample);\n"
	"float4 byteSample = 255.0 * sample;\n"
	"return UINT_8888(byteSample.a, byteSample.r, byteSample.g, byteSample.b);\n"
"}\n"

// Interfaces and classes for different destination formats

"uint4 Generate_0(uint2 cacheCoord)\n"
"{\n"
	"uint2 blockCoord = cacheCoord / uint2(2,1);\n"

	"uint2 blockUL = blockCoord * uint2(8,8);\n"
	"uint2 subBlockUL = blockUL + uint2(0, 4*(cacheCoord.x%2));\n"

	"uint sample[32];\n"
	"for (uint y = 0; y < 4; ++y) {\n"
		"for (uint x = 0; x < 8; ++x) {\n"
			"sample[y*8+x] = SampleEFB(subBlockUL+uint2(x,y));\n"
		"}\n"
	"}\n"
		
	"uint dw[4];\n"
	"for (uint i = 0; i < 4; ++i) {\n"
		"dw[i] = UINT_44444444(\n"
			"ExtractR4(sample[8*i+0]), ExtractR4(sample[8*i+1]), ExtractR4(sample[8*i+2]), ExtractR4(sample[8*i+3]),\n"
			"ExtractR4(sample[8*i+4]), ExtractR4(sample[8*i+5]), ExtractR4(sample[8*i+6]), ExtractR4(sample[8*i+7])\n"
			");\n"
	"}\n"

	"return uint4(Swap32(dw[0]), Swap32(dw[1]), Swap32(dw[2]), Swap32(dw[3]));\n"
"}\n"

"uint4 Generate_4(uint2 cacheCoord)\n"
"{\n"
	"uint2 blockCoord = cacheCoord / uint2(2,1);\n"

	"uint2 blockUL = blockCoord * uint2(4,4);\n"
	"uint2 subBlockUL = blockUL + uint2(0, 2*(cacheCoord.x%2));\n"

	"uint sample0 = SampleEFB(subBlockUL+uint2(0,0));\n"
	"uint sample1 = SampleEFB(subBlockUL+uint2(1,0));\n"
	"uint sample2 = SampleEFB(subBlockUL+uint2(2,0));\n"
	"uint sample3 = SampleEFB(subBlockUL+uint2(3,0));\n"
	"uint sample4 = SampleEFB(subBlockUL+uint2(0,1));\n"
	"uint sample5 = SampleEFB(subBlockUL+uint2(1,1));\n"
	"uint sample6 = SampleEFB(subBlockUL+uint2(2,1));\n"
	"uint sample7 = SampleEFB(subBlockUL+uint2(3,1));\n"
		
	"uint dw0 = UINT_1616(EncodeRGB565(sample0), EncodeRGB565(sample1));\n"
	"uint dw1 = UINT_1616(EncodeRGB565(sample2), EncodeRGB565(sample3));\n"
	"uint dw2 = UINT_1616(EncodeRGB565(sample4), EncodeRGB565(sample5));\n"
	"uint dw3 = UINT_1616(EncodeRGB565(sample6), EncodeRGB565(sample7));\n"

	"return uint4(Swap32(dw0), Swap32(dw1), Swap32(dw2), Swap32(dw3));\n"
"}\n"

"uint4 Generate_5(uint2 cacheCoord)\n"
"{\n"
	"uint2 blockCoord = cacheCoord / uint2(2,1);\n"

	"uint2 blockUL = blockCoord * uint2(4,4);\n"
	"uint2 subBlockUL = blockUL + uint2(0, 2*(cacheCoord.x%2));\n"

	"uint sample0 = SampleEFB(subBlockUL+uint2(0,0));\n"
	"uint sample1 = SampleEFB(subBlockUL+uint2(1,0));\n"
	"uint sample2 = SampleEFB(subBlockUL+uint2(2,0));\n"
	"uint sample3 = SampleEFB(subBlockUL+uint2(3,0));\n"
	"uint sample4 = SampleEFB(subBlockUL+uint2(0,1));\n"
	"uint sample5 = SampleEFB(subBlockUL+uint2(1,1));\n"
	"uint sample6 = SampleEFB(subBlockUL+uint2(2,1));\n"
	"uint sample7 = SampleEFB(subBlockUL+uint2(3,1));\n"
		
	"uint dw0 = UINT_1616(EncodeRGB5A3(sample0), EncodeRGB5A3(sample1));\n"
	"uint dw1 = UINT_1616(EncodeRGB5A3(sample2), EncodeRGB5A3(sample3));\n"
	"uint dw2 = UINT_1616(EncodeRGB5A3(sample4), EncodeRGB5A3(sample5));\n"
	"uint dw3 = UINT_1616(EncodeRGB5A3(sample6), EncodeRGB5A3(sample7));\n"

	"return uint4(Swap32(dw0), Swap32(dw1), Swap32(dw2), Swap32(dw3));\n"
"}\n"

"uint4 Generate_6(uint2 cacheCoord)\n"
"{\n"
	"uint2 blockCoord = cacheCoord / uint2(4,1);\n"

	"uint2 blockUL = blockCoord * uint2(4,4);\n"
	"uint2 subBlockUL = blockUL + uint2(0, 2*(cacheCoord.x%2));\n"

	"uint sample0 = SampleEFB(subBlockUL+uint2(0,0));\n"
	"uint sample1 = SampleEFB(subBlockUL+uint2(1,0));\n"
	"uint sample2 = SampleEFB(subBlockUL+uint2(2,0));\n"
	"uint sample3 = SampleEFB(subBlockUL+uint2(3,0));\n"
	"uint sample4 = SampleEFB(subBlockUL+uint2(0,1));\n"
	"uint sample5 = SampleEFB(subBlockUL+uint2(1,1));\n"
	"uint sample6 = SampleEFB(subBlockUL+uint2(2,1));\n"
	"uint sample7 = SampleEFB(subBlockUL+uint2(3,1));\n"

	"uint dw0;\n"
	"uint dw1;\n"
	"uint dw2;\n"
	"uint dw3;\n"
	"if (cacheCoord.x % 4 < 2)\n"
	"{\n"
		// First cache line gets AR
		"dw0 = UINT_8888(ExtractA(sample0), ExtractR(sample0), ExtractA(sample1), ExtractR(sample1));\n"
		"dw1 = UINT_8888(ExtractA(sample2), ExtractR(sample2), ExtractA(sample3), ExtractR(sample3));\n"
		"dw2 = UINT_8888(ExtractA(sample4), ExtractR(sample4), ExtractA(sample5), ExtractR(sample5));\n"
		"dw3 = UINT_8888(ExtractA(sample6), ExtractR(sample6), ExtractA(sample7), ExtractR(sample7));\n"
	"}\n"
	"else\n"
	"{\n"
		// Second cache line gets GB
		"dw0 = UINT_8888(ExtractG(sample0), ExtractB(sample0), ExtractG(sample1), ExtractB(sample1));\n"
		"dw1 = UINT_8888(ExtractG(sample2), ExtractB(sample2), ExtractG(sample3), ExtractB(sample3));\n"
		"dw2 = UINT_8888(ExtractG(sample4), ExtractB(sample4), ExtractG(sample5), ExtractB(sample5));\n"
		"dw3 = UINT_8888(ExtractG(sample6), ExtractB(sample6), ExtractG(sample7), ExtractB(sample7));\n"
	"}\n"

	"return uint4(Swap32(dw0), Swap32(dw1), Swap32(dw2), Swap32(dw3));\n"
"}\n"

"uint4 Generate_7(uint2 cacheCoord)\n"
"{\n"
	"uint2 blockCoord = cacheCoord / uint2(2,1);\n"

	"uint2 blockUL = blockCoord * uint2(8,4);\n"
	"uint2 subBlockUL = blockUL + uint2(0, 2*(cacheCoord.x%2));\n"

	"uint sample0 = SampleEFB(subBlockUL+uint2(0,0));\n"
	"uint sample1 = SampleEFB(subBlockUL+uint2(1,0));\n"
	"uint sample2 = SampleEFB(subBlockUL+uint2(2,0));\n"
	"uint sample3 = SampleEFB(subBlockUL+uint2(3,0));\n"
	"uint sample4 = SampleEFB(subBlockUL+uint2(4,0));\n"
	"uint sample5 = SampleEFB(subBlockUL+uint2(5,0));\n"
	"uint sample6 = SampleEFB(subBlockUL+uint2(6,0));\n"
	"uint sample7 = SampleEFB(subBlockUL+uint2(7,0));\n"
	"uint sample8 = SampleEFB(subBlockUL+uint2(0,1));\n"
	"uint sample9 = SampleEFB(subBlockUL+uint2(1,1));\n"
	"uint sampleA = SampleEFB(subBlockUL+uint2(2,1));\n"
	"uint sampleB = SampleEFB(subBlockUL+uint2(3,1));\n"
	"uint sampleC = SampleEFB(subBlockUL+uint2(4,1));\n"
	"uint sampleD = SampleEFB(subBlockUL+uint2(5,1));\n"
	"uint sampleE = SampleEFB(subBlockUL+uint2(6,1));\n"
	"uint sampleF = SampleEFB(subBlockUL+uint2(7,1));\n"

	"uint dw0 = UINT_8888(ExtractA(sample0), ExtractA(sample1), ExtractA(sample2), ExtractA(sample3));\n"
	"uint dw1 = UINT_8888(ExtractA(sample4), ExtractA(sample5), ExtractA(sample6), ExtractA(sample7));\n"
	"uint dw2 = UINT_8888(ExtractA(sample8), ExtractA(sample9), ExtractA(sampleA), ExtractA(sampleB));\n"
	"uint dw3 = UINT_8888(ExtractA(sampleC), ExtractA(sampleD), ExtractA(sampleE), ExtractA(sampleF));\n"

	"return uint4(Swap32(dw0), Swap32(dw1), Swap32(dw2), Swap32(dw3));\n"
"}\n"

"uint4 Generate_8(uint2 cacheCoord)\n"
"{\n"
	"uint2 blockCoord = cacheCoord / uint2(2,1);\n"

	"uint2 blockUL = blockCoord * uint2(8,4);\n"
	"uint2 subBlockUL = blockUL + uint2(0, 2*(cacheCoord.x%2));\n"

	"uint sample0 = SampleEFB(subBlockUL+uint2(0,0));\n"
	"uint sample1 = SampleEFB(subBlockUL+uint2(1,0));\n"
	"uint sample2 = SampleEFB(subBlockUL+uint2(2,0));\n"
	"uint sample3 = SampleEFB(subBlockUL+uint2(3,0));\n"
	"uint sample4 = SampleEFB(subBlockUL+uint2(4,0));\n"
	"uint sample5 = SampleEFB(subBlockUL+uint2(5,0));\n"
	"uint sample6 = SampleEFB(subBlockUL+uint2(6,0));\n"
	"uint sample7 = SampleEFB(subBlockUL+uint2(7,0));\n"
	"uint sample8 = SampleEFB(subBlockUL+uint2(0,1));\n"
	"uint sample9 = SampleEFB(subBlockUL+uint2(1,1));\n"
	"uint sampleA = SampleEFB(subBlockUL+uint2(2,1));\n"
	"uint sampleB = SampleEFB(subBlockUL+uint2(3,1));\n"
	"uint sampleC = SampleEFB(subBlockUL+uint2(4,1));\n"
	"uint sampleD = SampleEFB(subBlockUL+uint2(5,1));\n"
	"uint sampleE = SampleEFB(subBlockUL+uint2(6,1));\n"
	"uint sampleF = SampleEFB(subBlockUL+uint2(7,1));\n"

	"uint dw0 = UINT_8888(ExtractR(sample0), ExtractR(sample1), ExtractR(sample2), ExtractR(sample3));\n"
	"uint dw1 = UINT_8888(ExtractR(sample4), ExtractR(sample5), ExtractR(sample6), ExtractR(sample7));\n"
	"uint dw2 = UINT_8888(ExtractR(sample8), ExtractR(sample9), ExtractR(sampleA), ExtractR(sampleB));\n"
	"uint dw3 = UINT_8888(ExtractR(sampleC), ExtractR(sampleD), ExtractR(sampleE), ExtractR(sampleF));\n"

	"return uint4(Swap32(dw0), Swap32(dw1), Swap32(dw2), Swap32(dw3));\n"
"}\n"

"uint4 Generate_A(uint2 cacheCoord)\n"
"{\n"
	"uint2 blockCoord = cacheCoord / uint2(2,1);\n"

	"uint2 blockUL = blockCoord * uint2(8,4);\n"
	"uint2 subBlockUL = blockUL + uint2(0, 2*(cacheCoord.x%2));\n"

	"uint sample0 = SampleEFB(subBlockUL+uint2(0,0));\n"
	"uint sample1 = SampleEFB(subBlockUL+uint2(1,0));\n"
	"uint sample2 = SampleEFB(subBlockUL+uint2(2,0));\n"
	"uint sample3 = SampleEFB(subBlockUL+uint2(3,0));\n"
	"uint sample4 = SampleEFB(subBlockUL+uint2(4,0));\n"
	"uint sample5 = SampleEFB(subBlockUL+uint2(5,0));\n"
	"uint sample6 = SampleEFB(subBlockUL+uint2(6,0));\n"
	"uint sample7 = SampleEFB(subBlockUL+uint2(7,0));\n"
	"uint sample8 = SampleEFB(subBlockUL+uint2(0,1));\n"
	"uint sample9 = SampleEFB(subBlockUL+uint2(1,1));\n"
	"uint sampleA = SampleEFB(subBlockUL+uint2(2,1));\n"
	"uint sampleB = SampleEFB(subBlockUL+uint2(3,1));\n"
	"uint sampleC = SampleEFB(subBlockUL+uint2(4,1));\n"
	"uint sampleD = SampleEFB(subBlockUL+uint2(5,1));\n"
	"uint sampleE = SampleEFB(subBlockUL+uint2(6,1));\n"
	"uint sampleF = SampleEFB(subBlockUL+uint2(7,1));\n"

	"uint dw0 = UINT_8888(ExtractB(sample0), ExtractB(sample1), ExtractB(sample2), ExtractB(sample3));\n"
	"uint dw1 = UINT_8888(ExtractB(sample4), ExtractB(sample5), ExtractB(sample6), ExtractB(sample7));\n"
	"uint dw2 = UINT_8888(ExtractB(sample8), ExtractB(sample9), ExtractB(sampleA), ExtractB(sampleB));\n"
	"uint dw3 = UINT_8888(ExtractB(sampleC), ExtractB(sampleD), ExtractB(sampleE), ExtractB(sampleF));\n"

	"return uint4(Swap32(dw0), Swap32(dw1), Swap32(dw2), Swap32(dw3));\n"
"}\n"

"uint4 Generate_B(uint2 cacheCoord)\n"
"{\n"
	"uint2 blockCoord = cacheCoord / uint2(2,1);\n"

	"uint2 blockUL = blockCoord * uint2(4,4);\n"
	"uint2 subBlockUL = blockUL + uint2(0, 2*(cacheCoord.x%2));\n"

	"uint sample0 = SampleEFB(subBlockUL+uint2(0,0));\n"
	"uint sample1 = SampleEFB(subBlockUL+uint2(1,0));\n"
	"uint sample2 = SampleEFB(subBlockUL+uint2(2,0));\n"
	"uint sample3 = SampleEFB(subBlockUL+uint2(3,0));\n"
	"uint sample4 = SampleEFB(subBlockUL+uint2(0,1));\n"
	"uint sample5 = SampleEFB(subBlockUL+uint2(1,1));\n"
	"uint sample6 = SampleEFB(subBlockUL+uint2(2,1));\n"
	"uint sample7 = SampleEFB(subBlockUL+uint2(3,1));\n"

	"uint dw0 = UINT_8888(ExtractG(sample0), ExtractR(sample0), ExtractG(sample1), ExtractR(sample1));\n"
	"uint dw1 = UINT_8888(ExtractG(sample2), ExtractR(sample2), ExtractG(sample3), ExtractR(sample3));\n"
	"uint dw2 = UINT_8888(ExtractG(sample4), ExtractR(sample4), ExtractG(sample5), ExtractR(sample5));\n"
	"uint dw3 = UINT_8888(ExtractG(sample6), ExtractR(sample6), ExtractG(sample7), ExtractR(sample7));\n"

	"return uint4(Swap32(dw0), Swap32(dw1), Swap32(dw2), Swap32(dw3));\n"
"}\n"

"#ifdef DYNAMIC_MODE\n"
"interface iGenerator\n"
"{\n"
	"uint4 Generate(uint2 cacheCoord);\n"
"};\n"

"class cGenerator_4 : iGenerator\n"
"{\n"
	"uint4 Generate(uint2 cacheCoord)\n"
	"{ return Generate_4(cacheCoord); }\n"
"};\n"

"class cGenerator_5 : iGenerator\n"
"{\n"
	"uint4 Generate(uint2 cacheCoord)\n"
	"{ return Generate_5(cacheCoord); }\n"
"};\n"

"class cGenerator_6 : iGenerator\n"
"{\n"
	"uint4 Generate(uint2 cacheCoord)\n"
	"{ return Generate_6(cacheCoord); }\n"
"};\n"

"class cGenerator_8 : iGenerator\n"
"{\n"
	"uint4 Generate(uint2 cacheCoord)\n"
	"{ return Generate_8(cacheCoord); }\n"
"};\n"

"class cGenerator_B : iGenerator\n"
"{\n"
	"uint4 Generate(uint2 cacheCoord)\n"
	"{ return Generate_B(cacheCoord); }\n"
"};\n"

// Declare generator interface; must be set by application
"iGenerator g_generator;\n"
"#define IMP_GENERATOR g_generator.Generate\n"

"#endif\n"

"#ifndef IMP_GENERATOR\n"
"#error No generator specified\n"
"#endif\n"

"void main(out uint4 ocol0 : SV_Target, in float4 Pos : SV_Position, in float2 fCacheCoord : ENCODECOORD)\n"
"{\n"
	"uint2 cacheCoord = uint2(fCacheCoord);\n"
	"ocol0 = IMP_GENERATOR(cacheCoord);\n"
"}\n"
;

PSTextureEncoder::PSTextureEncoder()
	: m_ready(false), m_out(NULL), m_outRTV(NULL), m_outStage(NULL),
	m_encodeParams(NULL),
	m_quad(NULL), m_vShader(NULL), m_quadLayout(NULL),
	m_efbEncodeBlendState(NULL), m_efbEncodeDepthState(NULL),
	m_efbEncodeRastState(NULL), m_efbSampler(NULL),
	m_dynamicShader(NULL), m_classLinkage(NULL)
{
	for (size_t i = 0; i < 4; ++i)
		m_fetchClass[i] = NULL;
	for (size_t i = 0; i < 2; ++i)
		m_scaledFetchClass[i] = NULL;
	for (size_t i = 0; i < 2; ++i)
		m_intensityClass[i] = NULL;
	for (size_t i = 0; i < 16; ++i)
		m_generatorClass[i] = NULL;
}

static const D3D11_INPUT_ELEMENT_DESC QUAD_LAYOUT_DESC[] = {
	{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 }
};

static const struct QuadVertex
{
	float posX;
	float posY;
} QUAD_VERTS[4] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };

void PSTextureEncoder::Init()
{
	m_ready = false;

	HRESULT hr;

	// Create output texture RGBA format

	// This format allows us to generate one cache line in two pixels.
	D3D11_TEXTURE2D_DESC t2dd = CD3D11_TEXTURE2D_DESC(
		DXGI_FORMAT_R32G32B32A32_UINT,
		EFB_WIDTH, EFB_HEIGHT/4, 1, 1, D3D11_BIND_RENDER_TARGET);
	hr = D3D::device->CreateTexture2D(&t2dd, NULL, &m_out);
	CHECK(SUCCEEDED(hr), "create efb encode output texture");
	D3D::SetDebugObjectName(m_out, "efb encoder output texture");

	// Create output render target view

	D3D11_RENDER_TARGET_VIEW_DESC rtvd = CD3D11_RENDER_TARGET_VIEW_DESC(m_out,
		D3D11_RTV_DIMENSION_TEXTURE2D, DXGI_FORMAT_R32G32B32A32_UINT);
	hr = D3D::device->CreateRenderTargetView(m_out, &rtvd, &m_outRTV);
	CHECK(SUCCEEDED(hr), "create efb encode output render target view");
	D3D::SetDebugObjectName(m_outRTV, "efb encoder output rtv");

	// Create output staging buffer
	
	t2dd.Usage = D3D11_USAGE_STAGING;
	t2dd.BindFlags = 0;
	t2dd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	hr = D3D::device->CreateTexture2D(&t2dd, NULL, &m_outStage);
	CHECK(SUCCEEDED(hr), "create efb encode output staging buffer");
	D3D::SetDebugObjectName(m_outStage, "efb encoder output staging buffer");

	// Create constant buffer for uploading data to shaders

	D3D11_BUFFER_DESC bd = CD3D11_BUFFER_DESC(sizeof(EFBEncodeParams),
		D3D11_BIND_CONSTANT_BUFFER);
	hr = D3D::device->CreateBuffer(&bd, NULL, &m_encodeParams);
	CHECK(SUCCEEDED(hr), "create efb encode params buffer");
	D3D::SetDebugObjectName(m_encodeParams, "efb encoder params buffer");

	// Create vertex quad

	bd = CD3D11_BUFFER_DESC(sizeof(QUAD_VERTS), D3D11_BIND_VERTEX_BUFFER,
		D3D11_USAGE_IMMUTABLE);
	D3D11_SUBRESOURCE_DATA srd = { QUAD_VERTS, 0, 0 };

	hr = D3D::device->CreateBuffer(&bd, &srd, &m_quad);
	CHECK(SUCCEEDED(hr), "create efb encode quad vertex buffer");
	D3D::SetDebugObjectName(m_quad, "efb encoder quad vertex buffer");

	// Create vertex shader

	D3DBlob* bytecode = NULL;
	if (!D3D::CompileVertexShader(EFB_ENCODE_VS, sizeof(EFB_ENCODE_VS), &bytecode))
	{
		ERROR_LOG(VIDEO, "EFB encode vertex shader failed to compile");
		return;
	}

	hr = D3D::device->CreateVertexShader(bytecode->Data(), bytecode->Size(), NULL, &m_vShader);
	CHECK(SUCCEEDED(hr), "create efb encode vertex shader");
	D3D::SetDebugObjectName(m_vShader, "efb encoder vertex shader");

	// Create input layout for vertex quad using bytecode from vertex shader

	hr = D3D::device->CreateInputLayout(QUAD_LAYOUT_DESC,
		sizeof(QUAD_LAYOUT_DESC)/sizeof(D3D11_INPUT_ELEMENT_DESC),
		bytecode->Data(), bytecode->Size(), &m_quadLayout);
	CHECK(SUCCEEDED(hr), "create efb encode quad vertex layout");
	D3D::SetDebugObjectName(m_quadLayout, "efb encoder quad layout");

	bytecode->Release();

	// Create pixel shader

#ifdef USE_DYNAMIC_MODE
	if (!InitDynamicMode())
#else
	if (!InitStaticMode())
#endif
		return;

	// Create blend state

	D3D11_BLEND_DESC bld = CD3D11_BLEND_DESC(CD3D11_DEFAULT());
	hr = D3D::device->CreateBlendState(&bld, &m_efbEncodeBlendState);
	CHECK(SUCCEEDED(hr), "create efb encode blend state");
	D3D::SetDebugObjectName(m_efbEncodeBlendState, "efb encoder blend state");

	// Create depth state

	D3D11_DEPTH_STENCIL_DESC dsd = CD3D11_DEPTH_STENCIL_DESC(CD3D11_DEFAULT());
	dsd.DepthEnable = FALSE;
	hr = D3D::device->CreateDepthStencilState(&dsd, &m_efbEncodeDepthState);
	CHECK(SUCCEEDED(hr), "create efb encode depth state");
	D3D::SetDebugObjectName(m_efbEncodeDepthState, "efb encoder depth state");

	// Create rasterizer state

	D3D11_RASTERIZER_DESC rd = CD3D11_RASTERIZER_DESC(CD3D11_DEFAULT());
	rd.CullMode = D3D11_CULL_NONE;
	rd.DepthClipEnable = FALSE;
	hr = D3D::device->CreateRasterizerState(&rd, &m_efbEncodeRastState);
	CHECK(SUCCEEDED(hr), "create efb encode rast state");
	D3D::SetDebugObjectName(m_efbEncodeRastState, "efb encoder rast state");

	// Create efb texture sampler

	D3D11_SAMPLER_DESC sd = CD3D11_SAMPLER_DESC(CD3D11_DEFAULT());
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	hr = D3D::device->CreateSamplerState(&sd, &m_efbSampler);
	CHECK(SUCCEEDED(hr), "create efb encode texture sampler");
	D3D::SetDebugObjectName(m_efbSampler, "efb encoder texture sampler");

	m_ready = true;
}

void PSTextureEncoder::Shutdown()
{
	m_ready = false;
	
	for (size_t i = 0; i < 4; ++i)
		SAFE_RELEASE(m_fetchClass[i]);
	for (size_t i = 0; i < 2; ++i)
		SAFE_RELEASE(m_scaledFetchClass[i]);
	for (size_t i = 0; i < 2; ++i)
		SAFE_RELEASE(m_intensityClass[i]);
	for (size_t i = 0; i < 16; ++i)
		SAFE_RELEASE(m_generatorClass[i]);
	m_linkageArray.clear();
	
	SAFE_RELEASE(m_classLinkage);
	SAFE_RELEASE(m_dynamicShader);

	for (ComboMap::iterator it = m_staticShaders.begin();
		it != m_staticShaders.end(); ++it)
	{
		SAFE_RELEASE(it->second);
	}
	m_staticShaders.clear();

	SAFE_RELEASE(m_efbSampler);
	SAFE_RELEASE(m_efbEncodeRastState);
	SAFE_RELEASE(m_efbEncodeDepthState);
	SAFE_RELEASE(m_efbEncodeBlendState);
	SAFE_RELEASE(m_quadLayout);
	SAFE_RELEASE(m_vShader);
	SAFE_RELEASE(m_quad);
	SAFE_RELEASE(m_encodeParams);
	SAFE_RELEASE(m_outStage);
	SAFE_RELEASE(m_outRTV);
	SAFE_RELEASE(m_out);
}

size_t PSTextureEncoder::Encode(u8* dst, unsigned int dstFormat,
	unsigned int srcFormat, const EFBRectangle& srcRect, bool isIntensity,
	bool scaleByHalf)
{
	if (!m_ready) // Make sure we initialized OK
		return 0;

	HRESULT hr;

	unsigned int blockW = BLOCK_WIDTHS[dstFormat];
	unsigned int blockH = BLOCK_HEIGHTS[dstFormat];

	// Round up source dims to multiple of block size
	unsigned int actualWidth = srcRect.GetWidth() / (scaleByHalf ? 2 : 1);
	actualWidth = (actualWidth + blockW-1) & ~(blockW-1);
	unsigned int actualHeight = srcRect.GetHeight() / (scaleByHalf ? 2 : 1);
	actualHeight = (actualHeight + blockH-1) & ~(blockH-1);

	unsigned int numBlocksX = actualWidth/blockW;
	unsigned int numBlocksY = actualHeight/blockH;

	unsigned int cacheLinesPerRow;
	if (dstFormat == 0x6) // RGBA takes two cache lines per block; all others take one
		cacheLinesPerRow = numBlocksX*2;
	else
		cacheLinesPerRow = numBlocksX;
	CHECK(cacheLinesPerRow*32 <= MAX_BYTES_PER_BLOCK_ROW, "cache lines per row sanity check");

	unsigned int totalCacheLines = cacheLinesPerRow * numBlocksY;
	CHECK(totalCacheLines*32 <= MAX_BYTES_PER_ENCODE, "total encode size sanity check");

	size_t encodeSize = 0;
	
	// Reset API

	g_renderer->ResetAPIState();

	// Set up all the state for EFB encoding
	
#ifdef USE_DYNAMIC_MODE
	if (SetDynamicShader(dstFormat, srcFormat, isIntensity, scaleByHalf))
#else
	if (SetStaticShader(dstFormat, srcFormat, isIntensity, scaleByHalf))
#endif
	{
		D3D::context->VSSetShader(m_vShader, NULL, 0);

		D3D::stateman->PushBlendState(m_efbEncodeBlendState);
		D3D::stateman->PushDepthState(m_efbEncodeDepthState);
		D3D::stateman->PushRasterizerState(m_efbEncodeRastState);
		D3D::stateman->Apply();
	
		D3D11_VIEWPORT vp = CD3D11_VIEWPORT(0.f, 0.f, FLOAT(cacheLinesPerRow*2), FLOAT(numBlocksY));
		D3D::context->RSSetViewports(1, &vp);

		D3D::context->IASetInputLayout(m_quadLayout);
		D3D::context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		UINT stride = sizeof(QuadVertex);
		UINT offset = 0;
		D3D::context->IASetVertexBuffers(0, 1, &m_quad, &stride, &offset);
	
		EFBRectangle fullSrcRect;
		fullSrcRect.left = 0;
		fullSrcRect.top = 0;
		fullSrcRect.right = EFB_WIDTH;
		fullSrcRect.bottom = EFB_HEIGHT;
		TargetRectangle targetRect = g_renderer->ConvertEFBRectangle(fullSrcRect);
	
		EFBEncodeParams params = { 0 };
		params.NumHalfCacheLinesX = cacheLinesPerRow*2;
		params.NumBlocksY = numBlocksY;
		params.PosX = srcRect.left;
		params.PosY = srcRect.top;
		params.TexLeft = float(targetRect.left) / g_renderer->GetFullTargetWidth();
		params.TexTop = float(targetRect.top) / g_renderer->GetFullTargetHeight();
		params.TexRight = float(targetRect.right) / g_renderer->GetFullTargetWidth();
		params.TexBottom = float(targetRect.bottom) / g_renderer->GetFullTargetHeight();
		D3D::context->UpdateSubresource(m_encodeParams, 0, NULL, &params, 0, 0);

		D3D::context->VSSetConstantBuffers(0, 1, &m_encodeParams);
	
		D3D::context->OMSetRenderTargets(1, &m_outRTV, NULL);

		ID3D11ShaderResourceView* pEFB = (srcFormat == PIXELFMT_Z24) ?
			FramebufferManager::GetEFBDepthTexture()->GetSRV() :
			FramebufferManager::GetEFBColorTexture()->GetSRV();

		D3D::context->PSSetConstantBuffers(0, 1, &m_encodeParams);
		D3D::context->PSSetShaderResources(0, 1, &pEFB);
		D3D::context->PSSetSamplers(0, 1, &m_efbSampler);

		// Encode!

		D3D::context->Draw(4, 0);

		// Copy to staging buffer

		D3D11_BOX srcBox = CD3D11_BOX(0, 0, 0, cacheLinesPerRow*2, numBlocksY, 1);
		D3D::context->CopySubresourceRegion(m_outStage, 0, 0, 0, 0, m_out, 0, &srcBox);

		// Clean up state
	
		IUnknown* nullDummy = NULL;

		D3D::context->PSSetSamplers(0, 1, (ID3D11SamplerState**)&nullDummy);
		D3D::context->PSSetShaderResources(0, 1, (ID3D11ShaderResourceView**)&nullDummy);
		D3D::context->PSSetConstantBuffers(0, 1, (ID3D11Buffer**)&nullDummy);
	
		D3D::context->OMSetRenderTargets(0, NULL, NULL);

		D3D::context->VSSetConstantBuffers(0, 1, (ID3D11Buffer**)&nullDummy);
		
		D3D::stateman->PopRasterizerState();
		D3D::stateman->PopDepthState();
		D3D::stateman->PopBlendState();

		D3D::context->PSSetShader(NULL, NULL, 0);
		D3D::context->VSSetShader(NULL, NULL, 0);

		// Transfer staging buffer to GameCube/Wii RAM

		D3D11_MAPPED_SUBRESOURCE map = { 0 };
		hr = D3D::context->Map(m_outStage, 0, D3D11_MAP_READ, 0, &map);
		CHECK(SUCCEEDED(hr), "map staging buffer");

		u8* src = (u8*)map.pData;
		for (unsigned int y = 0; y < numBlocksY; ++y)
		{
			memcpy(dst, src, cacheLinesPerRow*32);
			dst += bpmem.copyMipMapStrideChannels*32;
			src += map.RowPitch;
		}

		D3D::context->Unmap(m_outStage, 0);

		encodeSize = bpmem.copyMipMapStrideChannels*32 * numBlocksY;
	}

	// Restore API

	g_renderer->RestoreAPIState();
	D3D::context->OMSetRenderTargets(1,
		&FramebufferManager::GetEFBColorTexture()->GetRTV(),
		FramebufferManager::GetEFBDepthTexture()->GetDSV());

	return encodeSize;
}

bool PSTextureEncoder::InitStaticMode()
{
	// Nothing to really do.
	return true;
}

static const char* FETCH_FUNC_NAMES[4] = {
	"Fetch_0", "Fetch_1", "Fetch_2", "Fetch_3"
};

static const char* SCALEDFETCH_FUNC_NAMES[2] = {
	"ScaledFetch_0", "ScaledFetch_1"
};

static const char* INTENSITY_FUNC_NAMES[2] = {
	"Intensity_0", "Intensity_1"
};

bool PSTextureEncoder::SetStaticShader(unsigned int dstFormat, unsigned int srcFormat,
	bool isIntensity, bool scaleByHalf)
{
	size_t fetchNum = srcFormat;
	size_t scaledFetchNum = scaleByHalf ? 1 : 0;
	size_t intensityNum = isIntensity ? 1 : 0;
	size_t generatorNum = dstFormat;

	ComboKey key = MakeComboKey(dstFormat, srcFormat, isIntensity, scaleByHalf);

	ComboMap::iterator it = m_staticShaders.find(key);
	if (it == m_staticShaders.end())
	{
		const char* generatorFuncName = NULL;
		switch (generatorNum)
		{
		case 0x0: generatorFuncName = "Generate_0"; break;
		case 0x4: generatorFuncName = "Generate_4"; break;
		case 0x5: generatorFuncName = "Generate_5"; break;
		case 0x6: generatorFuncName = "Generate_6"; break;
		case 0x7: generatorFuncName = "Generate_7"; break;
		case 0x8: generatorFuncName = "Generate_8"; break;
		case 0xA: generatorFuncName = "Generate_A"; break;
		case 0xB: generatorFuncName = "Generate_B"; break;
		default:
			WARN_LOG(VIDEO, "No generator available for dst format 0x%X; aborting", generatorNum);
			m_staticShaders[key] = NULL;
			return false;
		}

		// Shader permutation not found, so compile it
		D3DBlob* bytecode = NULL;
		D3D_SHADER_MACRO macros[] = {
			{ "IMP_FETCH", FETCH_FUNC_NAMES[fetchNum] },
			{ "IMP_SCALEDFETCH", SCALEDFETCH_FUNC_NAMES[scaledFetchNum] },
			{ "IMP_INTENSITY", INTENSITY_FUNC_NAMES[intensityNum] },
			{ "IMP_GENERATOR", generatorFuncName },
			{ NULL, NULL }
		};
		if (!D3D::CompilePixelShader(EFB_ENCODE_PS, sizeof(EFB_ENCODE_PS), &bytecode, macros))
		{
			WARN_LOG(VIDEO, "EFB encoder shader for dstFormat 0x%X, srcFormat %d, isIntensity %d, scaleByHalf %d failed to compile",
				dstFormat, srcFormat, isIntensity ? 1 : 0, scaleByHalf ? 1 : 0);
			// Add dummy shader to map to prevent trying to compile over and
			// over again
			m_staticShaders[key] = NULL;
			return false;
		}

		ID3D11PixelShader* newShader;
		HRESULT hr = D3D::device->CreatePixelShader(bytecode->Data(), bytecode->Size(), NULL, &newShader);
		CHECK(SUCCEEDED(hr), "create efb encoder pixel shader");

		m_staticShaders[key] = newShader;
		bytecode->Release();

		it = m_staticShaders.find(key);
	}

	if (it != m_staticShaders.end())
	{
		if (it->second)
		{
			D3D::context->PSSetShader(it->second, NULL, 0);
			return true;
		}
		else
			return false;
	}
	else
		return false;
}

bool PSTextureEncoder::InitDynamicMode()
{
	HRESULT hr;

	D3D_SHADER_MACRO macros[] = {
		{ "DYNAMIC_MODE", NULL },
		{ NULL, NULL }
	};

	D3DBlob* bytecode = NULL;
	if (!D3D::CompilePixelShader(EFB_ENCODE_PS, sizeof(EFB_ENCODE_PS), &bytecode, macros))
	{
		ERROR_LOG(VIDEO, "EFB encode pixel shader failed to compile");
		return false;
	}

	hr = D3D::device->CreateClassLinkage(&m_classLinkage);
	CHECK(SUCCEEDED(hr), "create efb encode class linkage");
	D3D::SetDebugObjectName(m_classLinkage, "efb encoder class linkage");

	hr = D3D::device->CreatePixelShader(bytecode->Data(), bytecode->Size(), m_classLinkage, &m_dynamicShader);
	CHECK(SUCCEEDED(hr), "create efb encode pixel shader");
	D3D::SetDebugObjectName(m_dynamicShader, "efb encoder pixel shader");
	
	// Use D3DReflect

	ID3D11ShaderReflection* reflect = NULL;
	hr = PD3DReflect(bytecode->Data(), bytecode->Size(), IID_ID3D11ShaderReflection, (void**)&reflect);
	CHECK(SUCCEEDED(hr), "reflect on efb encoder shader");

	// Get number of slots and create dynamic linkage array

	UINT numSlots = reflect->GetNumInterfaceSlots();
	m_linkageArray.resize(numSlots, NULL);

	// Get interface slots

	ID3D11ShaderReflectionVariable* var = reflect->GetVariableByName("g_fetch");
	m_fetchSlot = var->GetInterfaceSlot(0);

	var = reflect->GetVariableByName("g_scaledFetch");
	m_scaledFetchSlot = var->GetInterfaceSlot(0);

	var = reflect->GetVariableByName("g_intensity");
	m_intensitySlot = var->GetInterfaceSlot(0);

	var = reflect->GetVariableByName("g_generator");
	m_generatorSlot = var->GetInterfaceSlot(0);

	INFO_LOG(VIDEO, "fetch slot %d, scaledFetch slot %d, intensity slot %d, generator slot %d",
		m_fetchSlot, m_scaledFetchSlot, m_intensitySlot, m_generatorSlot);

	// Class instances will be created at the time they are used

	for (size_t i = 0; i < 4; ++i)
		m_fetchClass[i] = NULL;
	for (size_t i = 0; i < 2; ++i)
		m_scaledFetchClass[i] = NULL;
	for (size_t i = 0; i < 2; ++i)
		m_intensityClass[i] = NULL;
	for (size_t i = 0; i < 16; ++i)
		m_generatorClass[i] = NULL;

	reflect->Release();
	bytecode->Release();

	return true;
}

static const char* FETCH_CLASS_NAMES[4] = {
	"cFetch_0", "cFetch_1", "cFetch_2", "cFetch_3"
};

static const char* SCALEDFETCH_CLASS_NAMES[2] = {
	"cScaledFetch_0", "cScaledFetch_1"
};

static const char* INTENSITY_CLASS_NAMES[2] = {
	"cIntensity_0", "cIntensity_1"
};

bool PSTextureEncoder::SetDynamicShader(unsigned int dstFormat,
	unsigned int srcFormat, bool isIntensity, bool scaleByHalf)
{
	size_t fetchNum = srcFormat;
	size_t scaledFetchNum = scaleByHalf ? 1 : 0;
	size_t intensityNum = isIntensity ? 1 : 0;
	size_t generatorNum = dstFormat;

	// FIXME: Not all the possible generators are available as classes yet.
	// When dynamic mode is usable, implement them.
	const char* generatorName = NULL;
	switch (generatorNum)
	{
	case 0x4: generatorName = "cGenerator_4"; break;
	case 0x5: generatorName = "cGenerator_5"; break;
	case 0x6: generatorName = "cGenerator_6"; break;
	case 0x8: generatorName = "cGenerator_8"; break;
	case 0xB: generatorName = "cGenerator_B"; break;
	default:
		WARN_LOG(VIDEO, "No generator available for dst format 0x%X; aborting", generatorNum);
		return false;
	}

	// Make sure class instances are available
	if (!m_fetchClass[fetchNum])
	{
		INFO_LOG(VIDEO, "Creating %s class instance for encoder 0x%X",
			FETCH_CLASS_NAMES[fetchNum], dstFormat);
		HRESULT hr = m_classLinkage->CreateClassInstance(
			FETCH_CLASS_NAMES[fetchNum], 0, 0, 0, 0, &m_fetchClass[fetchNum]);
		CHECK(SUCCEEDED(hr), "create fetch class instance");
	}
	if (!m_scaledFetchClass[scaledFetchNum])
	{
		INFO_LOG(VIDEO, "Creating %s class instance for encoder 0x%X",
			SCALEDFETCH_CLASS_NAMES[scaledFetchNum], dstFormat);
		HRESULT hr = m_classLinkage->CreateClassInstance(
			SCALEDFETCH_CLASS_NAMES[scaledFetchNum], 0, 0, 0, 0,
			&m_scaledFetchClass[scaledFetchNum]);
		CHECK(SUCCEEDED(hr), "create scaled fetch class instance");
	}
	if (!m_intensityClass[intensityNum])
	{
		INFO_LOG(VIDEO, "Creating %s class instance for encoder 0x%X",
			INTENSITY_CLASS_NAMES[intensityNum], dstFormat);
		HRESULT hr = m_classLinkage->CreateClassInstance(
			INTENSITY_CLASS_NAMES[intensityNum], 0, 0, 0, 0,
			&m_intensityClass[intensityNum]);
		CHECK(SUCCEEDED(hr), "create intensity class instance");
	}
	if (!m_generatorClass[generatorNum])
	{
		INFO_LOG(VIDEO, "Creating %s class instance for encoder 0x%X",
			generatorName, dstFormat);
		HRESULT hr = m_classLinkage->CreateClassInstance(
			generatorName, 0, 0, 0, 0, &m_generatorClass[generatorNum]);
		CHECK(SUCCEEDED(hr), "create generator class instance");
	}

	// Assemble dynamic linkage array
	if (m_fetchSlot != UINT(-1))
		m_linkageArray[m_fetchSlot] = m_fetchClass[fetchNum];
	if (m_scaledFetchSlot != UINT(-1))
		m_linkageArray[m_scaledFetchSlot] = m_scaledFetchClass[scaledFetchNum];
	if (m_intensitySlot != UINT(-1))
		m_linkageArray[m_intensitySlot] = m_intensityClass[intensityNum];
	if (m_generatorSlot != UINT(-1))
		m_linkageArray[m_generatorSlot] = m_generatorClass[generatorNum];
	
	D3D::context->PSSetShader(m_dynamicShader,
		m_linkageArray.empty() ? NULL : &m_linkageArray[0],
		(UINT)m_linkageArray.size());

	return true;
}

}