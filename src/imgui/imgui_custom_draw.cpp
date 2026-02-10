#include "imgui_custom_draw.h"
#include "imgui_application_host.h"

struct WAVEFORM_CONSTANT_BUFFER
{
	struct
	{
		vec2 Pos, UV;
	} PerVertex[6];
	struct
	{
		vec2 Size, SizeInv;
	} CB_RectSize;
	struct
	{
		f32 R, G, B, A;
	} Color;
	float Amplitudes[CustomDraw::WaveformPixelsPerChunk];
};

// TODO: Reimplement these on SDL3/SDL_GPU backend
namespace CustomDraw
{
	void GPUTexture::Load(const GPUTextureDesc &desc)
	{
		if (ApplicationHost::GlobalState.SDL_GPUDeviceHandle == nullptr)
		{
			IM_ASSERT(false && "SDL_GPUDeviceHandle is null!");
			return;
		}

		auto image_width = static_cast<u32>(desc.Size.x);
		auto image_height = static_cast<u32>(desc.Size.y);
		auto device = static_cast<SDL_GPUDevice *>(ApplicationHost::GlobalState.SDL_GPUDeviceHandle);
		// Create texture
		SDL_GPUTextureCreateInfo texture_info = {};
		texture_info.type = SDL_GPU_TEXTURETYPE_2D;
		if (desc.Format == GPUPixelFormat::RGBA)
			texture_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		else if (desc.Format == GPUPixelFormat::BGRA)
			texture_info.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
		else
			IM_ASSERT(false && "Unsupported GPUPixelFormat in GPUTexture::Load");
		texture_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
		texture_info.width = image_width;
		texture_info.height = image_height; 
		texture_info.layer_count_or_depth = 1;
		texture_info.num_levels = 1;
		texture_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
		
		auto sampler_info = SDL_GPUSamplerCreateInfo {
            .min_filter = SDL_GPU_FILTER_NEAREST,
            .mag_filter = SDL_GPU_FILTER_NEAREST,
            .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
            .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
            .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
            .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
		};

		this->Binding.texture = SDL_CreateGPUTexture(device, &texture_info);
		this->Binding.sampler = SDL_CreateGPUSampler(device, &sampler_info);

		// Create transfer buffer
		// FIXME: A real engine would likely keep one around, see what the SDL_GPU backend is doing.
		SDL_GPUTransferBufferCreateInfo transferbuffer_info = {};
		transferbuffer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
		transferbuffer_info.size = image_width * image_height * 4;
		SDL_GPUTransferBuffer *transferbuffer = SDL_CreateGPUTransferBuffer(device, &transferbuffer_info);
		IM_ASSERT(transferbuffer != NULL);

		// Copy to transfer buffer
		uint32_t upload_pitch = image_width * 4;
		void *texture_ptr = SDL_MapGPUTransferBuffer(device, transferbuffer, true);
		for (int y = 0; y < image_height; y++)
			memcpy((void *)((uintptr_t)texture_ptr + y * upload_pitch), (u8 *)desc.InitialPixels + y * upload_pitch, upload_pitch);
		SDL_UnmapGPUTransferBuffer(device, transferbuffer);

		SDL_GPUTextureTransferInfo transfer_info = {};
		transfer_info.offset = 0;
		transfer_info.transfer_buffer = transferbuffer;

		SDL_GPUTextureRegion texture_region = {};
		texture_region.texture = this->Binding.texture;
		texture_region.x = 0;
		texture_region.y = 0;
		texture_region.w = image_width;
		texture_region.h = image_height;
		texture_region.d = 1;

		// Upload
		SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device);
		SDL_GPUCopyPass *copy_pass = SDL_BeginGPUCopyPass(cmd);
		SDL_UploadToGPUTexture(copy_pass, &transfer_info, &texture_region, false);
		SDL_EndGPUCopyPass(copy_pass);
		SDL_SubmitGPUCommandBuffer(cmd);

		SDL_ReleaseGPUTransferBuffer(device, transferbuffer);

		this->Size = desc.Size;
		this->Format = desc.Format;
		this->Access = desc.Access;
	}

	void GPUTexture::Unload()
	{
		auto device = static_cast<SDL_GPUDevice *>(ApplicationHost::GlobalState.SDL_GPUDeviceHandle);
		SDL_ReleaseGPUTexture(device, this->Binding.texture);
		SDL_ReleaseGPUSampler(device, this->Binding.sampler);
		this->Binding.texture = nullptr;
		this->Binding.sampler = nullptr;
		this->Access = GPUAccessType::Static;
		this->Size = ivec2(0, 0);
		this->Format = GPUPixelFormat::RGBA;
	}

	void GPUTexture::UpdateDynamic(ivec2 size, const void *newPixels)
	{
		// TODO: Reimplement since this is not in used by current code
	}

	b8 GPUTexture::IsValid() const { return this->Binding.texture != nullptr && this->Binding.sampler != nullptr; }
	ivec2 GPUTexture::GetSize() const { return this->Size; }
	vec2 GPUTexture::GetSizeF32() const { return vec2(GetSize()); }
	GPUPixelFormat GPUTexture::GetFormat() const { return this->Format; }
	ImTextureID GPUTexture::GetTexID() const { return (ImTextureID)(intptr_t)&this->Binding; }
	void DrawWaveformChunk(ImDrawList *drawList, Rect rect, u32 color, const WaveformChunk &chunk)
	{
		// Render waveform using a triangle strip (connected quads)
		const int sampleCount = WaveformPixelsPerChunk;
		if (sampleCount < 2) return;

		// 4 vertices per sample: TopTransparent, TopSolid, BottomSolid, BottomTransparent
		const int vertexCount = sampleCount * 4;
		// 3 quads per sample-step (TopFringe, MainBody, BottomFringe) * 6 indices per quad
		const int indexCount = (sampleCount - 1) * 18;

		// Check if we would overflow the 16-bit index buffer when VtxOffset is not supported or handled automatically.
		// PrimReserve should handle splitting, but just in case, we safeguard.
		// (Although splitting a continuous strip is hard, ImGui doesn't split strip automatically unless we separate commands).
		// We trust PrimReserve, but we perform strict pointer logic.

		drawList->PrimReserve(indexCount, vertexCount);

		ImDrawVert* vtx_write = drawList->_VtxWritePtr;
		ImDrawIdx* idx_write = drawList->_IdxWritePtr;
		unsigned int vtx_current_idx = drawList->_VtxCurrentIdx;
		ImVec2 uv_white = drawList->_Data->TexUvWhitePixel;

		f32 xStart = rect.TL.x;
		f32 xStep = rect.GetWidth() / static_cast<f32>(sampleCount - 1);
		
		f32 yCenter = rect.GetCenter().y;
		f32 halfHeight = rect.GetHeight() * 0.5f;
		
		f32 aa_size = 1.0f; 
		u32 color_trans = color & ~IM_COL32_A_MASK;

		for (int i = 0; i < sampleCount; i++)
		{
			f32 amplitude = chunk.PerPixelAmplitude[i];
			f32 x = xStart + (i * xStep);
			f32 yOffset = amplitude * halfHeight; 

			f32 yTop = yCenter - yOffset;
			f32 yBot = yCenter + yOffset;

			// V0: Top Outer
			vtx_write[0].pos.x = x; vtx_write[0].pos.y = yTop - aa_size; vtx_write[0].uv = uv_white; vtx_write[0].col = color_trans;
			// V1: Top Inner
			vtx_write[1].pos.x = x; vtx_write[1].pos.y = yTop;           vtx_write[1].uv = uv_white; vtx_write[1].col = color;
			// V2: Bottom Inner
			vtx_write[2].pos.x = x; vtx_write[2].pos.y = yBot;           vtx_write[2].uv = uv_white; vtx_write[2].col = color;
			// V3: Bottom Outer
			vtx_write[3].pos.x = x; vtx_write[3].pos.y = yBot + aa_size; vtx_write[3].uv = uv_white; vtx_write[3].col = color_trans;

			vtx_write += 4;
		}

		for (int i = 0; i < sampleCount - 1; i++)
		{
			unsigned int idx = vtx_current_idx + (i * 4);
			unsigned int next_idx = idx + 4; // Indices are relative to the vertex buffer unless VtxOffset is used.
			
			// Top Fringe
			idx_write[0] = (ImDrawIdx)(idx + 0); idx_write[1] = (ImDrawIdx)(next_idx + 0); idx_write[2] = (ImDrawIdx)(next_idx + 1);
			idx_write[3] = (ImDrawIdx)(next_idx + 1); idx_write[4] = (ImDrawIdx)(idx + 1); idx_write[5] = (ImDrawIdx)(idx + 0);

			// Main Body
			idx_write[6] = (ImDrawIdx)(idx + 1); idx_write[7] = (ImDrawIdx)(next_idx + 1); idx_write[8] = (ImDrawIdx)(next_idx + 2);
			idx_write[9] = (ImDrawIdx)(next_idx + 2); idx_write[10] = (ImDrawIdx)(idx + 2); idx_write[11] = (ImDrawIdx)(idx + 1);

			// Bottom Fringe
			idx_write[12] = (ImDrawIdx)(idx + 2); idx_write[13] = (ImDrawIdx)(next_idx + 2); idx_write[14] = (ImDrawIdx)(next_idx + 3);
			idx_write[15] = (ImDrawIdx)(next_idx + 3); idx_write[16] = (ImDrawIdx)(idx + 3); idx_write[17] = (ImDrawIdx)(idx + 2);

			idx_write += 18;
		}

		drawList->_VtxWritePtr = vtx_write;
		drawList->_IdxWritePtr = idx_write;
		drawList->_VtxCurrentIdx += vertexCount;
	}
}
