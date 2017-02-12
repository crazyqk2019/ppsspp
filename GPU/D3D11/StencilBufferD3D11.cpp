// Copyright (c) 2014- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <d3d11.h>

#include "base/logging.h"

#include "gfx/d3d9_state.h"
#include "ext/native/thin3d/thin3d.h"
#include "Core/Reporting.h"
#include "GPU/D3D11/FramebufferManagerD3D11.h"
#include "GPU/D3D11/FragmentShaderGeneratorD3D11.h"
#include "GPU/D3D11/ShaderManagerD3D11.h"
#include "GPU/D3D11/TextureCacheD3D11.h"
#include "GPU/D3D11/D3D11Util.h"

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

struct StencilUB {
	float u_stencilValue[4];
};

static const char *stencil_ps =
"SamplerState samp : register(s0);\n"
"Texture2D<float4> tex : register(t0);\n"
"cbuffer base : register(b0) {\n"
"  float4 u_stencilValue;\n"
"};\n"
"struct PS_IN {\n"
"  float2 v_texcoord0 : TEXCOORD0;\n"
"};\n"
"float roundAndScaleTo255f(in float x) { return floor(x * 255.99); }\n"
"float4 main(PS_IN In) : SV_Target {\n"
"  float4 index = tex.Sample(samp, In.v_texcoord0);\n"
"  float shifted = roundAndScaleTo255f(index.a) / roundAndScaleTo255f(u_stencilValue.x);\n"
"  clip(fmod(floor(shifted), 2.0) - 0.99);\n"
"  return index.aaaa;\n"
"}\n";

static const char *stencil_vs =
"struct VS_IN {\n"
"  float4 a_position : POSITION;\n"
"  float2 a_texcoord0 : TEXCOORD0;\n"
"};\n"
"struct VS_OUT {\n"
"  float2 v_texcoord0 : TEXCOORD0;\n"
"  float4 position : SV_Position;\n"
"};\n"
"VS_OUT main(VS_IN In) {\n"
"  VS_OUT Out;\n"
"  Out.position = In.a_position;\n"
"  Out.v_texcoord0 = In.a_texcoord0;\n"
"  return Out;\n"
"}\n";

static u8 StencilBits5551(const u8 *ptr8, u32 numPixels) {
	const u32 *ptr = (const u32 *)ptr8;

	for (u32 i = 0; i < numPixels / 2; ++i) {
		if (ptr[i] & 0x80008000) {
			return 1;
		}
	}
	return 0;
}

static u8 StencilBits4444(const u8 *ptr8, u32 numPixels) {
	const u32 *ptr = (const u32 *)ptr8;
	u32 bits = 0;

	for (u32 i = 0; i < numPixels / 2; ++i) {
		bits |= ptr[i];
	}

	return ((bits >> 12) & 0xF) | (bits >> 28);
}

static u8 StencilBits8888(const u8 *ptr8, u32 numPixels) {
	const u32 *ptr = (const u32 *)ptr8;
	u32 bits = 0;

	for (u32 i = 0; i < numPixels; ++i) {
		bits |= ptr[i];
	}

	return bits >> 24;
}

// TODO : If SV_StencilRef is available (D3D11.3) then this can be done in a single pass.
bool FramebufferManagerD3D11::NotifyStencilUpload(u32 addr, int size, bool skipZero) {
	if (!MayIntersectFramebuffer(addr)) {
		return false;
	}

	VirtualFramebuffer *dstBuffer = 0;
	for (size_t i = 0; i < vfbs_.size(); ++i) {
		VirtualFramebuffer *vfb = vfbs_[i];
		if (MaskedEqual(vfb->fb_address, addr)) {
			dstBuffer = vfb;
		}
	}
	if (!dstBuffer) {
		return false;
	}

	int values = 0;
	u8 usedBits = 0;

	const u8 *src = Memory::GetPointer(addr);
	if (!src) {
		return false;
	}

	switch (dstBuffer->format) {
	case GE_FORMAT_565:
		// Well, this doesn't make much sense.
		return false;
	case GE_FORMAT_5551:
		usedBits = StencilBits5551(src, dstBuffer->fb_stride * dstBuffer->bufferHeight);
		values = 2;
		break;
	case GE_FORMAT_4444:
		usedBits = StencilBits4444(src, dstBuffer->fb_stride * dstBuffer->bufferHeight);
		values = 16;
		break;
	case GE_FORMAT_8888:
		usedBits = StencilBits8888(src, dstBuffer->fb_stride * dstBuffer->bufferHeight);
		values = 256;
		break;
	case GE_FORMAT_INVALID:
		// Impossible.
		break;
	}

	if (usedBits == 0) {
		if (skipZero) {
			// Common when creating buffers, it's already 0.  We're done.
			return false;
		}

		// Clear stencil+alpha but not color. Only way is to draw a quad.
		context_->OMSetBlendState(stockD3D11.blendStateDisabledWithColorMask[0x8], nullptr, 0xFFFFFFFF);
		context_->RSSetState(stockD3D11.rasterStateNoCull);
		context_->OMSetDepthStencilState(stockD3D11.depthDisabledStencilWrite, 0);
		context_->IASetVertexBuffers(0, 1, &fsQuadBuffer_, &quadStride_, &quadOffset_);
		context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		context_->Draw(4, 0);
		return true;
	}

	// TODO: Helper with logging?
	if (!stencilUploadPS_) {
		std::string errorMessage;
		stencilUploadPS_ = CreatePixelShaderD3D11(device_, stencil_ps, strlen(stencil_ps));
	}
	if (!stencilUploadVS_) {
		std::string errorMessage;
		std::vector<uint8_t> byteCode;
		stencilUploadVS_ = CreateVertexShaderD3D11(device_, stencil_vs, strlen(stencil_vs), &byteCode);
		device_->CreateInputLayout(g_QuadVertexElements, 2, byteCode.data(), byteCode.size(), &stencilUploadInputLayout_);
	}

	shaderManager_->DirtyLastShader();

	u16 w = dstBuffer->renderWidth;
	u16 h = dstBuffer->renderHeight;
	MakePixelTexture(src, dstBuffer->format, dstBuffer->fb_stride, dstBuffer->bufferWidth, dstBuffer->bufferHeight);
	if (dstBuffer->fbo) {
		draw_->BindFramebufferAsRenderTarget(dstBuffer->fbo);
	} else {
		// something is wrong...
	}
	D3D11_VIEWPORT vp{ 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
	context_->RSSetViewports(1, &vp);

	// Zero stencil
	draw_->Clear(Draw::ClearFlag::STENCIL, 0, 0, 0);

	float fw = dstBuffer->width;
	float fh = dstBuffer->height;

	float coord[20] = {
		0.0f,0.0f,0.0f, 0.0f,0.0f,
		fw,0.0f,0.0f, 1.0f,0.0f,
		fw,fh,0.0f, 1.0f,1.0f,
		0.0f,fh,0.0f, 0.0f,1.0f,
	};
	// I think all these calculations pretty much cancel out?
	float invDestW = 1.0f / (fw * 0.5f);
	float invDestH = 1.0f / (fh * 0.5f);
	for (int i = 0; i < 4; i++) {
		coord[i * 5] = coord[i * 5] * invDestW - 1.0f;
		coord[i * 5 + 1] = -(coord[i * 5 + 1] * invDestH - 1.0f);
	}

	D3D11_MAPPED_SUBRESOURCE map;
	context_->Map(quadBuffer_, 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
	memcpy(map.pData, coord, sizeof(float) * 4 * 5);
	context_->Unmap(quadBuffer_, 0);

	shaderManager_->DirtyLastShader();
	textureCacheD3D11_->ForgetLastTexture();

	context_->OMSetBlendState(stockD3D11.blendStateDisabledWithColorMask[0x8], nullptr, 0xFFFFFFFF);
	context_->IASetInputLayout(stencilUploadInputLayout_);
	context_->PSSetShader(stencilUploadPS_, nullptr, 0);
	context_->VSSetShader(stencilUploadVS_, nullptr, 0);
	context_->PSSetShaderResources(0, 1, &drawPixelsTexView_);
	context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	context_->RSSetState(stockD3D11.rasterStateNoCull);
	context_->IASetVertexBuffers(0, 1, &quadBuffer_, &quadStride_, &quadOffset_);
	context_->OMSetBlendState(stockD3D11.blendStateDisabledWithColorMask[0x8], nullptr, 0xFFFFFFFF);
	context_->OMSetDepthStencilState(stockD3D11.depthDisabledStencilWrite, 0xFF);
	context_->RSSetState(stockD3D11.rasterStateNoCull);

	for (int i = 1; i < values; i += i) {
		if (!(usedBits & i)) {
			// It's already zero, let's skip it.
			continue;
		}
		uint8_t mask = 0;
		uint8_t value = 0;
		if (dstBuffer->format == GE_FORMAT_4444) {
			mask = i | (i << 4);
			value = i * 16;
		} else if (dstBuffer->format == GE_FORMAT_5551) {
			mask = 0xFF;
			value = i * 128;
		} else {
			mask = i;
			value = i;
		}
		if (!stencilMaskStates_[mask]) {
			D3D11_DEPTH_STENCIL_DESC desc{};
			desc.DepthEnable = false;
			desc.StencilEnable = true;
			desc.StencilReadMask = 0xFF;
			desc.StencilWriteMask = mask;
			desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_REPLACE;
			desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_REPLACE;
			desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
			desc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
			desc.BackFace = desc.FrontFace;
			device_->CreateDepthStencilState(&desc, &stencilMaskStates_[mask]);
		}
		context_->OMSetDepthStencilState(stencilMaskStates_[mask], value);
		context_->Draw(4, 0);
	}
	RebindFramebuffer();
	return true;
}
