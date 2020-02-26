#include "../rendering/common.h"
#include "../rendering/GpuCircularBuffer.h"
#include <d3dcompiler.h>
#include "BSGraphicsRenderer.h"
#include "BSShader/BSShaderAccumulator.h"
#include "BSShader/BSShaderRenderTargets.h"
#include "BSReadWriteLock.h"
#include "MOC.h"
#include "NiMain/BSGeometry.h"

namespace BSGraphics::Utility
{
	void CopyNiColorAToFloat(float *Floats, const NiColorA& Color)
	{
		Floats[0] = Color.r;
		Floats[1] = Color.g;
		Floats[2] = Color.b;
		Floats[3] = Color.a;
	}

	void CopyNiColorAToFloat(DirectX::XMVECTOR *Floats, const NiColorA& Color)
	{
		*Floats = Color.AsXmm();
	}

	void PackDynamicParticleData(uint32_t ParticleCount, class NiParticles *Particles, void *Buffer)
	{
		AutoFunc(decltype(&PackDynamicParticleData), sub_140D75710, 0xD75710);
		sub_140D75710(ParticleCount, Particles, Buffer);
	}
}

namespace BSGraphics
{
	void BeginEvent(wchar_t *Marker)
	{
		Renderer::GetGlobals()->BeginEvent(Marker);
	}

	void EndEvent()
	{
		Renderer::GetGlobals()->EndEvent();
	}

	int CurrentFrameIndex;

	const uint32_t VertexIndexRingBufferSize = 128 * 1024 * 1024;
	const uint32_t ShaderConstantRingBufferSize = 32 * 1024 * 1024;
	const uint32_t RingBufferMaxFrames = 8;

	thread_local VertexShader *TLS_CurrentVertexShader;
	thread_local PixelShader *TLS_CurrentPixelShader;

	GpuCircularBuffer *DynamicBuffer;		// Holds vertices and indices
	GpuCircularBuffer *ShaderConstantBuffer;// Holds shader constant values

	ID3D11Query *FrameCompletedQueries[8];
	bool FrameCompletedQueryPending[8];

	thread_local uint64_t TestBufferUsedBits[4];
	ID3D11Buffer *TestBuffers[4][64];
	ID3D11Buffer *TestLargeBuffer;

	ID3D11Buffer *TempDynamicBuffers[11];

	const uint32_t ThresholdSize = 32;

	std::unordered_map<uint64_t, ID3D11InputLayout *> m_InputLayoutMap;
	BSReadWriteLock m_InputLayoutLock;

	std::unordered_map<void *, std::pair<void *, size_t>> m_ShaderBytecodeMap;

	Renderer *Renderer::GetGlobals()
	{
		return (Renderer *)HACK_GetThreadedGlobals();
	}

	Renderer *Renderer::GetGlobalsNonThreaded()
	{
		return (Renderer *)HACK_GetMainGlobals();
	}

	void Renderer::Initialize(ID3D11Device2 *Device)
	{
		BSShaderAccumulator::InitCallbackTable();

		for (uint32_t i = 0; i < RingBufferMaxFrames; i++)
		{
			D3D11_QUERY_DESC desc;
			desc.Query = D3D11_QUERY_EVENT;
			desc.MiscFlags = 0;

			Assert(SUCCEEDED(Device->CreateQuery(&desc, &FrameCompletedQueries[i])));
		}

		//
		// Various temporary dynamic buffers and dynamic ring buffer (indices + vertices)
		//
		// Temp buffers: 128 bytes to 131072 bytes 
		// Ring buffer: User set (default 32MB)
		//
		for (int i = 0; i < 11; i++)
		{
			D3D11_BUFFER_DESC desc;
			desc.ByteWidth = (1u << (i + 7));// pow(2, i + 7)
			desc.Usage = D3D11_USAGE_DYNAMIC;
			desc.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			desc.MiscFlags = 0;
			desc.StructureByteStride = 0;
			Assert(SUCCEEDED(Device->CreateBuffer(&desc, nullptr, &TempDynamicBuffers[i])));
		}

		DynamicBuffer = new GpuCircularBuffer(Device, D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER, VertexIndexRingBufferSize, RingBufferMaxFrames);

		//
		// Small temporary shader constant buffers and ring buffer
		//
		// Temp buffers: 0 bytes to 1008 bytes 
		// Ring buffer: User set (default 32MB)
		//
		TestBufferUsedBits[0] = 0;
		TestBufferUsedBits[1] = 0;
		TestBufferUsedBits[2] = 0;
		TestBufferUsedBits[3] = 0;

		for (int i = 0; i < 64; i++)
		{
			if (i == 0)
				continue;

			D3D11_BUFFER_DESC desc;
			desc.ByteWidth = i * 16;
			desc.Usage = D3D11_USAGE_DYNAMIC;
			desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			desc.MiscFlags = 0;
			desc.StructureByteStride = 0;

			for (int j = 0; j < 4; j++)
				Assert(SUCCEEDED(Device->CreateBuffer(&desc, nullptr, &TestBuffers[j][i])));
		}

		D3D11_BUFFER_DESC desc;
		desc.ByteWidth = 4096;
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		desc.MiscFlags = 0;
		desc.StructureByteStride = 0;
		Assert(SUCCEEDED(Device->CreateBuffer(&desc, nullptr, &TestLargeBuffer)));

		ShaderConstantBuffer = new GpuCircularBuffer(Device, D3D11_BIND_CONSTANT_BUFFER, ShaderConstantRingBufferSize, RingBufferMaxFrames);
	}

	void Renderer::OnNewFrame()
	{
		Assert(!FrameCompletedQueryPending[CurrentFrameIndex]);

		// Set a marker for when the GPU is done processing the previous frame
		GetGlobalsNonThreaded()->m_DeviceContext->End(FrameCompletedQueries[CurrentFrameIndex]);
		FrameCompletedQueryPending[CurrentFrameIndex] = true;

		DynamicBuffer->SwapFrame(CurrentFrameIndex);
		ShaderConstantBuffer->SwapFrame(CurrentFrameIndex);

		// "Pop" the query from 6 frames ago. This acts as a ring buffer.
		int prevQueryIndex = CurrentFrameIndex - 6;

		if (prevQueryIndex < 0)
			prevQueryIndex += 8;

		if (FrameCompletedQueryPending[prevQueryIndex])
		{
			BOOL data;
			HRESULT hr = GetGlobalsNonThreaded()->m_DeviceContext->GetData(FrameCompletedQueries[prevQueryIndex], &data, sizeof(data), D3D11_ASYNC_GETDATA_DONOTFLUSH);

			// Those commands are REQUIRED to be complete by now - no exceptions
			AssertMsg(SUCCEEDED(hr) && data == TRUE, "DeviceContext::GetData() MUST SUCCEED BY NOW");

			DynamicBuffer->FreeOldFrame(prevQueryIndex);
			ShaderConstantBuffer->FreeOldFrame(prevQueryIndex);
			FrameCompletedQueryPending[prevQueryIndex] = false;
		}

		FlushThreadedVars();
		CurrentFrameIndex++;

		if (CurrentFrameIndex >= 8)
			CurrentFrameIndex = 0;
	}

	void Renderer::FlushThreadedVars()
	{
		memset(&TestBufferUsedBits, 0, sizeof(TestBufferUsedBits));

		//
		// Shaders should've been unique because each technique is different,
		// but for some reason that isn't the case.
		//
		// Other code in Skyrim might be setting the parameters before I do,
		// so it's not guaranteed to be cleared. (Pretend something is set)
		//
		TLS_CurrentVertexShader = (VertexShader *)0xFEFEFEFEFEFEFEFE;
		TLS_CurrentPixelShader = (PixelShader *)0xFEFEFEFEFEFEFEFE;
	}

	void Renderer::BeginEvent(wchar_t *Marker) const
	{
		m_DeviceContext->BeginEventInt(Marker, 0);
	}

	void Renderer::EndEvent() const
	{
		m_DeviceContext->EndEvent();
	}

	void Renderer::DrawLineShape(LineShape *GraphicsLineShape, uint32_t StartIndex, uint32_t Count)
	{
		SetVertexDescription(GraphicsLineShape->m_VertexDesc);
		SetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);

		SyncD3DState(false);

		UINT stride = BSGeometry::CalculateVertexSize(GraphicsLineShape->m_VertexDesc);
		UINT offset = 0;

		m_DeviceContext->IASetVertexBuffers(0, 1, &GraphicsLineShape->m_VertexBuffer, &stride, &offset);
		m_DeviceContext->IASetIndexBuffer(GraphicsLineShape->m_IndexBuffer, DXGI_FORMAT_R16_UINT, 0);
		m_DeviceContext->DrawIndexed(2 * Count, StartIndex, 0);
	}

	void Renderer::DrawTriShape(TriShape *GraphicsTriShape, uint32_t StartIndex, uint32_t Count)
	{
		SetVertexDescription(GraphicsTriShape->m_VertexDesc);
		SetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		SyncD3DState(false);

		UINT stride = BSGeometry::CalculateVertexSize(GraphicsTriShape->m_VertexDesc);
		UINT offset = 0;

		m_DeviceContext->IASetVertexBuffers(0, 1, &GraphicsTriShape->m_VertexBuffer, &stride, &offset);
		m_DeviceContext->IASetIndexBuffer(GraphicsTriShape->m_IndexBuffer, DXGI_FORMAT_R16_UINT, 0);
		m_DeviceContext->DrawIndexed(3 * Count, StartIndex, 0);
	}

	DynamicTriShape *Renderer::GetParticlesDynamicTriShape()
	{
		static DynamicTriShape particles =
		{
			m_UnknownVertexBuffer,
			m_UnknownIndexBuffer,
			0x840200004000051,
			0xFFFFFFFF,
			0,
			1,
			nullptr,
			nullptr
		};

		return &particles;
	}

	void *Renderer::MapDynamicTriShapeDynamicData(BSDynamicTriShape *Shape, DynamicTriShape *ShapeData, DynamicTriShapeDrawData *DrawData, uint32_t VertexSize)
	{
		if (VertexSize <= 0)
			VertexSize = ShapeData->m_VertexAllocationSize;

		return MapDynamicBuffer(VertexSize, &ShapeData->m_VertexAllocationOffset);
	}

	void Renderer::UnmapDynamicTriShapeDynamicData(DynamicTriShape *Shape, DynamicTriShapeDrawData *DrawData)
	{
		m_DeviceContext->Unmap(m_DynamicBuffers[m_CurrentDynamicBufferIndex], 0);
	}

	void Renderer::DrawDynamicTriShape(DynamicTriShape *Shape, DynamicTriShapeDrawData *DrawData, uint32_t IndexStartOffset, uint32_t TriangleCount)
	{
		UnknownStruct params;
		params.m_VertexBuffer = Shape->m_VertexBuffer;
		params.m_IndexBuffer = Shape->m_IndexBuffer;
		params.m_VertexDesc = Shape->m_VertexDesc;

		DrawDynamicTriShape(&params, DrawData, IndexStartOffset, TriangleCount, Shape->m_VertexAllocationOffset);
	}

	void Renderer::DrawDynamicTriShape(UnknownStruct *Params, DynamicTriShapeDrawData *DrawData, uint32_t IndexStartOffset, uint32_t TriangleCount, uint32_t VertexBufferOffset)
	{
		SetVertexDescription(Params->m_VertexDesc);
		SetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		SyncD3DState(false);

		ID3D11Buffer *buffers[2];
		buffers[0] = Params->m_VertexBuffer;
		buffers[1] = m_DynamicBuffers[m_CurrentDynamicBufferIndex];

		UINT strides[2];
		strides[0] = BSGeometry::CalculateVertexSize(Params->m_VertexDesc);
		strides[1] = BSGeometry::CalculateDyanmicVertexSize(Params->m_VertexDesc);

		UINT offsets[2];
		offsets[0] = 0;
		offsets[1] = VertexBufferOffset;

		m_DeviceContext->IASetVertexBuffers(0, 2, buffers, strides, offsets);
		m_DeviceContext->IASetIndexBuffer(Params->m_IndexBuffer, DXGI_FORMAT_R16_UINT, 0);
		m_DeviceContext->DrawIndexed(TriangleCount * 3, IndexStartOffset, 0);
	}

	void Renderer::DrawParticleShaderTriShape(const void *DynamicData, uint32_t Count)
	{
		// Send dynamic data to GPU buffer
		uint32_t vertexStride = 48;
		uint32_t vertexOffset = 0;
		void *particleBuffer = MapDynamicBuffer(vertexStride * Count, &vertexOffset);

		memcpy_s(particleBuffer, vertexStride * Count, DynamicData, vertexStride * Count);
		UnmapDynamicTriShapeDynamicData(nullptr, nullptr);

		// Update flags but don't update the input layout - we use a custom one here
		SetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		m_StateUpdateFlags &= ~0x400;

		SyncD3DState(false);

		if (!m_UnknownInputLayout)
		{
			constexpr static D3D11_INPUT_ELEMENT_DESC inputDesc[] =
			{
				{ "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "NORMAL", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 1, DXGI_FORMAT_R8G8B8A8_SINT, 0, 44, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			};

			Assert(SUCCEEDED(m_Device->CreateInputLayout(
				inputDesc,
				ARRAYSIZE(inputDesc),
				m_CurrentVertexShader->m_RawBytecode,
				m_CurrentVertexShader->m_ShaderLength,
				&m_UnknownInputLayout)));
		}

		m_InputLayoutLock.LockForWrite();
		{
			uint64_t desc = m_VertexDescSetting & m_CurrentVertexShader->m_VertexDescription;
			m_InputLayoutMap.try_emplace(desc, m_UnknownInputLayout);
		}
		m_InputLayoutLock.UnlockWrite();

		m_DeviceContext->IASetInputLayout(m_UnknownInputLayout);
		m_StateUpdateFlags |= 0x400;

		m_DeviceContext->IASetIndexBuffer(m_UnknownIndexBuffer, DXGI_FORMAT_R16_UINT, 0);
		m_DeviceContext->IASetVertexBuffers(0, 1, &m_DynamicBuffers[m_CurrentDynamicBufferIndex], &vertexStride, &vertexOffset);
		m_DeviceContext->DrawIndexed(6 * (Count / 4), 0, 0);
	}

	SRWLOCK InputLayoutLock = SRWLOCK_INIT;

	void Renderer::SyncD3DState(bool Unknown)
	{
		auto renderer = BSGraphics::Renderer::GetGlobals();

		__int64 v5; // rdx
		int v10; // edx
		signed __int64 v12; // rcx
		float v14; // xmm0_4
		float v15; // xmm0_4

		renderer->UnmapDynamicConstantBuffer();

		uint64_t *v3 = (uint64_t *)renderer->qword_14304BF00;

		if (uint32_t flags = renderer->m_StateUpdateFlags; flags != 0)
		{
			if (flags & 1)
			{
				//
				// Build active render target view array
				//
				ID3D11RenderTargetView *renderTargetViews[8];
				uint32_t viewCount = 0;

				if (renderer->unknown1 == -1)
				{
					// This loops through all 8 entries ONLY IF they are not RENDER_TARGET_NONE. Otherwise break early.
					for (int i = 0; i < 8; i++)
					{
						uint32_t& rtState = renderer->m_RenderTargetStates[i];
						uint32_t rtIndex = renderer->m_RenderTargetIndexes[i];

						if (rtIndex == RENDER_TARGET_NONE)
							break;

						renderTargetViews[i] = (ID3D11RenderTargetView *)*((uint64_t *)v3 + 6 * rtIndex + 0x14B);
						viewCount++;

						if (rtState == 0)// if state == SRTM_CLEAR
						{
							renderer->m_DeviceContext->ClearRenderTargetView(renderTargetViews[i], (const FLOAT *)v3 + 2522);
							rtState = 4;// SRTM_INIT?
						}
					}
				}
				else
				{
					// Use a single RT instead. The purpose of this is unknown...
					v5 = *((uint64_t *)renderer->qword_14304BF00
						+ (signed int)renderer->unknown2
						+ 8i64 * (signed int)renderer->unknown1
						+ 1242);
					renderTargetViews[0] = (ID3D11RenderTargetView *)v5;
					viewCount = 1;

					if (!*(DWORD *)&renderer->__zz0[4])
					{
						renderer->m_DeviceContext->ClearRenderTargetView((ID3D11RenderTargetView *)v5, (float *)(char *)renderer->qword_14304BF00 + 10088);
						*(DWORD *)&renderer->__zz0[4] = 4;
					}
				}

				v10 = *(DWORD *)renderer->__zz0;
				if (v10 <= 2u || v10 == 6)
				{
					*((BYTE *)v3 + 34) = 0;
				}

				//
				// Determine which depth stencil to render to. When there's no active depth stencil
				// we simply send a nullptr to dx11.
				//
				ID3D11DepthStencilView *depthStencil = nullptr;

				if (renderer->rshadowState_iDepthStencil != -1)
				{
					v12 = renderer->rshadowState_iDepthStencilSlice
						+ 19i64 * (signed int)renderer->rshadowState_iDepthStencil;

					if (*((BYTE *)v3 + 34))
						depthStencil = (ID3D11DepthStencilView *)v3[v12 + 1022];
					else
						depthStencil = (ID3D11DepthStencilView *)v3[v12 + 1014];

					// Only clear the stencil if specific flags are set
					if (depthStencil && v10 != 3 && v10 != 4 && v10 != 5)
					{
						uint32_t clearFlags;

						switch (v10)
						{
						case 0:
						case 6:
							clearFlags = D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL;
							break;

						case 2:
							clearFlags = D3D11_CLEAR_STENCIL;
							break;

						case 1:
							clearFlags = D3D11_CLEAR_DEPTH;
							break;

						default:
							Assert(false);
							break;
						}

						renderer->m_DeviceContext->ClearDepthStencilView(depthStencil, clearFlags, 1.0f, 0);
						*(DWORD *)renderer->__zz0 = 4;
					}
				}

				renderer->m_DeviceContext->OMSetRenderTargets(viewCount, renderTargetViews, depthStencil);
			}

			// OMSetDepthStencilState
			if (flags & (0x4 | 0x8))
			{
				// OMSetDepthStencilState(m_DepthStates[m_DepthMode][m_StencilMode], m_StencilRef);
				renderer->m_DeviceContext->OMSetDepthStencilState(
					renderer->m_DepthStates[*(signed int *)&renderer->__zz0[32]][*(signed int *)&renderer->__zz0[40]],
					*(UINT *)&renderer->__zz0[44]);
			}

			// RSSetState
			if (flags & (0x1000 | 0x40 | 0x20 | 0x10))
			{
				// Cull mode, depth bias, fill mode, scissor mode, scissor rect (order unknown)
				void *wtf = renderer->m_RasterStates[0][0][0][*(signed int *)&renderer->__zz0[60]
					+ 2
					* (*(signed int *)&renderer->__zz0[56]
						+ 12
						* (*(signed int *)&renderer->__zz0[52]// Cull mode
							+ 3i64 * *(signed int *)&renderer->__zz0[48]))];

				renderer->m_DeviceContext->RSSetState((ID3D11RasterizerState *)wtf);

				flags = renderer->m_StateUpdateFlags;
				if (renderer->m_StateUpdateFlags & 0x40)
				{
					if (*(float *)&renderer->__zz0[24] != *(float *)&renderer->__zz2[640]
						|| (v14 = *(float *)&renderer->__zz0[28],
							*(float *)&renderer->__zz0[28] != *(float *)&renderer->__zz2[644]))
					{
						v14 = *(float *)&renderer->__zz2[644];
						*(DWORD *)&renderer->__zz0[24] = *(DWORD *)&renderer->__zz2[640];
						flags = renderer->m_StateUpdateFlags | 2;
						*(DWORD *)&renderer->__zz0[28] = *(DWORD *)&renderer->__zz2[644];
						renderer->m_StateUpdateFlags |= 2u;
					}
					if (*(DWORD *)&renderer->__zz0[56])
					{
						v15 = v14 - renderer->m_UnknownFloats1[0][*(signed int *)&renderer->__zz0[56]];
						flags |= 2u;
						renderer->m_StateUpdateFlags = flags;
						*(float *)&renderer->__zz0[28] = v15;
					}
				}
			}

			// RSSetViewports
			if (flags & 0x2)
			{
				renderer->m_DeviceContext->RSSetViewports(1, (D3D11_VIEWPORT *)&renderer->__zz0[8]);
			}

			// OMSetBlendState
			if (flags & 0x80)
			{
				float *blendFactor = (float *)(g_ModuleBase + 0x1E2C168);

				// Mode, write mode, alpha to coverage, blend state (order unknown)
				void *wtf = renderer->m_BlendStates[0][0][0][*(unsigned int *)&renderer->__zz2[656]
					+ 2
					* (*(signed int *)&renderer->__zz0[72]
						+ 13
						* (*(signed int *)&renderer->__zz0[68]
							+ 2i64 * *(signed int *)&renderer->__zz0[64]))];// AlphaBlendMode

				renderer->m_DeviceContext->OMSetBlendState((ID3D11BlendState *)wtf, blendFactor, 0xFFFFFFFF);
			}

			if (flags & (0x200 | 0x100))
			{
				D3D11_MAPPED_SUBRESOURCE resource;
				renderer->m_DeviceContext->Map(renderer->m_AlphaTestRefCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &resource);

				if (renderer->__zz0[76])
					*(float *)resource.pData = renderer->m_AlphaTestRef;
				else
					*(float *)resource.pData = 0.0f;

				renderer->m_DeviceContext->Unmap(renderer->m_AlphaTestRefCB, 0);
			}

			// Shader input layout creation + updates
			if (!Unknown && (flags & 0x400))
			{
				m_InputLayoutLock.LockForWrite();
				{
					uint64_t desc = renderer->m_VertexDescSetting & renderer->m_CurrentVertexShader->m_VertexDescription;

					// Does the entry exist already?
					if (auto e = m_InputLayoutMap.find(desc); e != m_InputLayoutMap.end())
					{
						// It does. We're done now.
						renderer->m_DeviceContext->IASetInputLayout(e->second);
					}
					else
					{
						// Create and insert
						AutoFunc(__int64(__fastcall *)(unsigned __int64 a1), sub_140D705F0, 0xD70620);
						ID3D11InputLayout *layout = (ID3D11InputLayout *)sub_140D705F0(desc);

						if (layout || desc != 0x300000000407)
							m_InputLayoutMap.emplace(desc, layout);

						renderer->m_DeviceContext->IASetInputLayout(layout);
					}
				}
				m_InputLayoutLock.UnlockWrite();
			}

			// IASetPrimitiveTopology
			if (flags & 0x800)
			{
				renderer->m_DeviceContext->IASetPrimitiveTopology(renderer->m_PrimitiveTopology);
			}

			if (Unknown)
				renderer->m_StateUpdateFlags = flags & 0x400;
			else
				renderer->m_StateUpdateFlags = 0;
		}

		SyncD3DResources();
	}

	void Renderer::SyncD3DResources()
	{
		auto *renderer = BSGraphics::Renderer::GetGlobals();

		//
		// Resource/state setting code. It's been modified to take 1 of 2 paths for each type:
		//
		// 1: modifiedBits == 0 { Do nothing }
		// 2: modifiedBits > 0  { Build minimal state change [X entries] before submitting it to DX }
		//
#define for_each_bit(itr, bits) for (unsigned long itr; _BitScanForward(&itr, bits); bits &= ~(1 << itr))

		// Compute shader unordered access views (UAVs)
		if (uint32_t bits = renderer->m_CSUAVModifiedBits; bits != 0)
		{
			AssertMsg((bits & 0xFFFFFF00) == 0, "CSUAVModifiedBits must not exceed 7th index");

			for_each_bit(i, bits)
				renderer->m_DeviceContext->CSSetUnorderedAccessViews(i, 1, &renderer->m_CSUAVResources[i], nullptr);

			renderer->m_CSUAVModifiedBits = 0;
		}

		// Pixel shader samplers
		if (uint32_t bits = renderer->m_PSSamplerModifiedBits; bits != 0)
		{
			AssertMsg((bits & 0xFFFF0000) == 0, "PSSamplerModifiedBits must not exceed 15th index");

			for_each_bit(i, bits)
				renderer->m_DeviceContext->PSSetSamplers(i, 1, &renderer->m_SamplerStates[renderer->m_PSSamplerAddressMode[i]][renderer->m_PSSamplerFilterMode[i]]);

			renderer->m_PSSamplerModifiedBits = 0;
		}

		// Pixel shader resources
		if (uint32_t bits = renderer->m_PSResourceModifiedBits; bits != 0)
		{
			AssertMsg((bits & 0xFFFF0000) == 0, "PSResourceModifiedBits must not exceed 15th index");

			for_each_bit(i, bits)
			{
				// Combine PSSSR(0, 1, [rsc1]) + PSSSR(1, 1, [rsc2]) into PSSSR(0, 2, [rsc1, rsc2])
				if (bits & (1 << (i + 1)))
				{
					renderer->m_DeviceContext->PSSetShaderResources(i, 2, &renderer->m_PSResources[i]);
					bits &= ~(1 << (i + 1));
				}
				else
					renderer->m_DeviceContext->PSSetShaderResources(i, 1, &renderer->m_PSResources[i]);
			}

			renderer->m_PSResourceModifiedBits = 0;
		}

		// Compute shader samplers
		if (uint32_t bits = renderer->m_CSSamplerModifiedBits; bits != 0)
		{
			AssertMsg((bits & 0xFFFF0000) == 0, "CSSamplerModifiedBits must not exceed 15th index");

			for_each_bit(i, bits)
				renderer->m_DeviceContext->CSSetSamplers(i, 1, &renderer->m_SamplerStates[renderer->m_CSSamplerSetting1[i]][renderer->m_CSSamplerSetting2[i]]);

			renderer->m_CSSamplerModifiedBits = 0;
		}

		// Compute shader resources
		if (uint32_t bits = renderer->m_CSResourceModifiedBits; bits != 0)
		{
			AssertMsg((bits & 0xFFFF0000) == 0, "CSResourceModifiedBits must not exceed 15th index");

			for_each_bit(i, bits)
				renderer->m_DeviceContext->CSSetShaderResources(i, 1, &renderer->m_CSResources[i]);

			renderer->m_CSResourceModifiedBits = 0;
		}

#undef for_each_bit
	}

	void Renderer::DepthStencilStateSetDepthMode(DepthStencilDepthMode Mode)
	{
		if (*(DWORD *)&__zz0[32] != Mode)
		{
			*(DWORD *)&__zz0[32] = Mode;

			// Temp var to prevent duplicate state setting? Don't know where this gets set.
			if (*(DWORD *)&__zz0[36] != Mode)
				m_StateUpdateFlags |= 0x4;
			else
				m_StateUpdateFlags &= ~0x4;
		}
	}

	DepthStencilDepthMode Renderer::DepthStencilStateGetDepthMode() const
	{
		return (DepthStencilDepthMode)*(DWORD *)&__zz0[32];
	}

	void Renderer::DepthStencilStateSetStencilMode(uint32_t Mode, uint32_t StencilRef)
	{
		if (*(DWORD *)&__zz0[40] != Mode || *(DWORD *)&__zz0[44] != StencilRef)
		{
			*(DWORD *)&__zz0[40] = Mode;
			*(DWORD *)&__zz0[44] = StencilRef;
			m_StateUpdateFlags |= 0x8;
		}
	}

	void Renderer::RasterStateSetCullMode(uint32_t CullMode)
	{
		if (*(DWORD *)&__zz0[52] != CullMode)
		{
			*(DWORD *)&__zz0[52] = CullMode;
			m_StateUpdateFlags |= 0x20;
		}
	}

	void Renderer::RasterStateSetUnknown1(uint32_t Value)
	{
		if (*(DWORD *)&__zz0[56] != Value)
		{
			*(DWORD *)&__zz0[56] = Value;
			m_StateUpdateFlags |= 0x40;
		}
	}

	void Renderer::AlphaBlendStateSetMode(uint32_t Mode)
	{
		if (*(DWORD *)&__zz0[64] != Mode)
		{
			*(DWORD *)&__zz0[64] = Mode;
			m_StateUpdateFlags |= 0x80;
		}
	}

	void Renderer::AlphaBlendStateSetUnknown1(uint32_t Value)
	{
		if (*(DWORD *)&__zz0[68] != Value)
		{
			*(DWORD *)&__zz0[68] = Value;
			m_StateUpdateFlags |= 0x80;
		}
	}

	void Renderer::AlphaBlendStateSetWriteMode(uint32_t Value)
	{
		if (*(DWORD *)&__zz0[72] != Value)
		{
			*(DWORD *)&__zz0[72] = Value;
			m_StateUpdateFlags |= 0x80;
		}
	}

	uint32_t Renderer::AlphaBlendStateGetUnknown2() const
	{
		return *(DWORD *)&__zz0[72];
	}

	void Renderer::SetUseAlphaTestRef(bool UseStoredValue)
	{
		// When UseStoredValue is false, the constant buffer data is zeroed, but m_AlphaTestRef is saved
		if (__zz0[76] != (char)UseStoredValue)
		{
			__zz0[76] = UseStoredValue;
			m_StateUpdateFlags |= 0x100u;
		}
	}

	void Renderer::SetAlphaTestRef(float Value)
	{
		if (m_AlphaTestRef != Value)
		{
			m_AlphaTestRef = Value;
			m_StateUpdateFlags |= 0x200u;
		}
	}

	void Renderer::SetVertexDescription(uint64_t VertexDesc)
	{
		if (m_VertexDescSetting != VertexDesc)
		{
			m_VertexDescSetting = VertexDesc;
			m_StateUpdateFlags |= 0x400;
		}
	}

	void Renderer::SetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY Topology)
	{
		if (m_PrimitiveTopology != Topology)
		{
			m_PrimitiveTopology = Topology;
			m_StateUpdateFlags |= 0x800;
		}
	}

	void Renderer::SetVertexShader(VertexShader *Shader)
	{
		if (Shader == TLS_CurrentVertexShader)
			return;

		// The input layout (IASetInputLayout) may need to be created and updated
		TLS_CurrentVertexShader = Shader;
		m_CurrentVertexShader = Shader;
		m_StateUpdateFlags |= 0x400;
		m_DeviceContext->VSSetShader(Shader ? Shader->m_Shader : nullptr, nullptr, 0);
	}

	void Renderer::SetPixelShader(PixelShader *Shader)
	{
		if (Shader == TLS_CurrentPixelShader)
			return;

		TLS_CurrentPixelShader = Shader;
		m_CurrentPixelShader = Shader;
		m_DeviceContext->PSSetShader(Shader ? Shader->m_Shader : nullptr, nullptr, 0);
	}

	void Renderer::SetHullShader(HullShader *Shader)
	{
		m_DeviceContext->HSSetShader(Shader ? Shader->m_Shader : nullptr, nullptr, 0);
	}

	void Renderer::SetDomainShader(DomainShader *Shader)
	{
		m_DeviceContext->DSSetShader(Shader ? Shader->m_Shader : nullptr, nullptr, 0);
	}

	void Renderer::SetTexture(uint32_t Index, const NiSourceTexture *Texture)
	{
		SetTexture(Index, Texture ? Texture->QRendererTexture() : nullptr);
	}

	void Renderer::SetTexture(uint32_t Index, const Texture *Resource)
	{
		SetShaderResource(Index, Resource ? Resource->m_ResourceView : nullptr);
	}

	void Renderer::SetTextureMode(uint32_t Index, uint32_t AddressMode, uint32_t FilterMode)
	{
		SetTextureAddressMode(Index, AddressMode);
		SetTextureFilterMode(Index, FilterMode);
	}

	void Renderer::SetTextureAddressMode(uint32_t Index, uint32_t Mode)
	{
		if (m_PSSamplerAddressMode[Index] != Mode)
		{
			m_PSSamplerAddressMode[Index] = Mode;
			m_PSSamplerModifiedBits |= 1 << Index;
		}
	}

	void Renderer::SetTextureFilterMode(uint32_t Index, uint32_t Mode)
	{
		if (m_PSSamplerFilterMode[Index] != Mode)
		{
			m_PSSamplerFilterMode[Index] = Mode;
			m_PSSamplerModifiedBits |= 1 << Index;
		}
	}

	// Not a real function name. Needs to be removed.
	void Renderer::SetShaderResource(uint32_t Index, ID3D11ShaderResourceView *Resource)
	{
		if (m_PSResources[Index] != Resource)
		{
			m_PSResources[Index] = Resource;
			m_PSResourceModifiedBits |= 1 << Index;
		}
	}

	ID3D11Buffer *Renderer::MapConstantBuffer(void **DataPointer, uint32_t *AllocationSize, uint32_t *AllocationOffset, uint32_t Level)
	{
		uint32_t initialAllocSize = *AllocationSize;
		uint32_t roundedAllocSize = 0;
		ID3D11Buffer *buffer = nullptr;

		Assert(initialAllocSize > 0);

		//
		// If the user lets us, try to use the global ring buffer instead of small temporary
		// allocations. It must be used on the immediate D3D context only. No MTR here.
		//
		//if (initialAllocSize > ThresholdSize && !MTRenderer::IsRenderingMultithreaded())
		if (initialAllocSize > ThresholdSize && true)
		{
			// Size must be rounded up to nearest 256 bytes (D3D11.1 specification)
			roundedAllocSize = (initialAllocSize + 256 - 1) & ~(256 - 1);

			*DataPointer = ShaderConstantBuffer->MapData(m_DeviceContext, roundedAllocSize, AllocationOffset, false);
			*AllocationSize = roundedAllocSize;
			buffer = ShaderConstantBuffer->D3DBuffer;
		}
		else
		{
			Assert(initialAllocSize <= 4096);

			if (Level >= ARRAYSIZE(TestBufferUsedBits))
				Level = ARRAYSIZE(TestBufferUsedBits) - 1;

			// Small constant buffer pool: round to nearest 16, determine bit index, then loop until there's a free slot
			roundedAllocSize = (initialAllocSize + 16 - 1) & ~(16 - 1);
			D3D11_MAPPED_SUBRESOURCE map;

			if (roundedAllocSize <= (63 * 16))
			{
				for (uint32_t bitIndex = roundedAllocSize / 16; bitIndex < 64;)
				{
					if ((TestBufferUsedBits[Level] & (1ull << bitIndex)) == 0)
					{
						TestBufferUsedBits[Level] |= (1ull << bitIndex);
						buffer = TestBuffers[Level][bitIndex];
						break;
					}

					// Try next largest buffer size
					bitIndex += 1;
					roundedAllocSize += 16;
				}
			}
			else
			{
				// Last-ditch effort for a large valid buffer
				roundedAllocSize = 4096;
				buffer = TestLargeBuffer;
			}

			Assert(buffer);
			Assert(SUCCEEDED(m_DeviceContext->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &map)));

			*DataPointer = map.pData;
			*AllocationSize = map.RowPitch;

			if (AllocationOffset)
				*AllocationOffset = 0;
		}

		ProfileCounterAdd("CB Bytes Requested", initialAllocSize);
		ProfileCounterAdd("CB Bytes Wasted", (roundedAllocSize - initialAllocSize));
		return buffer;
	}

	void Renderer::UnmapDynamicConstantBuffer()
	{
		//if (!MTRenderer::IsRenderingMultithreaded())
			ShaderConstantBuffer->UnmapData(m_DeviceContext);

		memset(&TestBufferUsedBits, 0, sizeof(TestBufferUsedBits));
	}

	void *Renderer::MapDynamicBuffer(uint32_t AllocationSize, uint32_t *AllocationOffset)
	{
		ProfileCounterAdd("VIB Bytes Requested", AllocationSize);

		//
		// Try to use the global ring buffer instead of small temporary allocations. It
		// must be used on the immediate D3D context only. No MTR here.
		//
		//if (!MTRenderer::IsRenderingMultithreaded())
		if (true)
		{
			m_DynamicBuffers[0] = DynamicBuffer->D3DBuffer;
			m_CurrentDynamicBufferIndex = 0;
			m_FrameDataUsedSize = AllocationSize;

			return DynamicBuffer->MapData(m_DeviceContext, AllocationSize, AllocationOffset, true);
		}

		//
		// Select one of the random temporary buffers: index = ceil(log2(max(AllocationSize, 256)))
		//
		// NOTE: There might be a race condition since there's only 1 array used. If skyrim discards
		// each allocation after a draw call, this is generally OK.
		//
		DWORD logIndex = 0;
		_BitScanReverse(&logIndex, std::max<uint32_t>(AllocationSize, 256));

		if ((1u << logIndex) < AllocationSize)
			logIndex += 1;

		Assert(logIndex >= 7 || logIndex < (11 + 7));

		// Adjust base index - buffers start at 256 (2^7) bytes
		ID3D11Buffer *buffer = TempDynamicBuffers[logIndex - 7];

		D3D11_MAPPED_SUBRESOURCE map;
		Assert(SUCCEEDED(m_DeviceContext->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &map)));

		m_DynamicBuffers[0] = buffer;
		m_CurrentDynamicBufferIndex = 0;
		m_FrameDataUsedSize = AllocationSize;
		*AllocationOffset = 0;

		return map.pData;
	}

	void ReflectConstantBuffers(ID3D11ShaderReflection *Reflector, Buffer *ConstantGroups, uint32_t MaxGroups, std::function<const char *(int Index)> GetConstant, uint8_t *Offsets, uint32_t MaxOffsets)
	{
		D3D11_SHADER_DESC desc;
		Assert(SUCCEEDED(Reflector->GetDesc(&desc)));

		// These should always be cleared first - invalid offsets don't get sent to the GPU
		memset(Offsets, INVALID_CONSTANT_BUFFER_OFFSET, MaxOffsets * sizeof(uint8_t));
		memset(ConstantGroups, 0, MaxGroups * sizeof(Buffer));

		if (desc.ConstantBuffers <= 0)
			return;

		auto mapBufferConsts = [&](ID3D11ShaderReflectionConstantBuffer *Reflector, Buffer *ConstantGroup)
		{
			// If this call fails, it's an invalid buffer
			D3D11_SHADER_BUFFER_DESC bufferDesc;
			if (FAILED(Reflector->GetDesc(&bufferDesc)))
				return;

			for (uint32_t i = 0; i < bufferDesc.Variables; i++)
			{
				ID3D11ShaderReflectionVariable *var = Reflector->GetVariableByIndex(i);

				D3D11_SHADER_VARIABLE_DESC varDesc;
				Assert(SUCCEEDED(var->GetDesc(&varDesc)));

				AssertMsgVa(varDesc.StartOffset % 4 == 0, "Variable '%s' is not aligned to 4", varDesc.Name);

				// Variable name maps to hardcoded index in SSE executable
				for (int j = 0;; j++)
				{
					const char *hardConstName = GetConstant(j);

					if (!hardConstName)
						break;

					if (_stricmp(hardConstName, varDesc.Name) == 0)
					{
						// Found!
						Offsets[j] = varDesc.StartOffset / 4;
						var = nullptr;
					}
				}

				AssertMsgVa(var == nullptr, "Variable '%s' did not have an index mapping in the executable", varDesc.Name);
			}

			// Nasty type cast here, but it's how the game does it (round up to nearest 16 bytes)
			*(uintptr_t *)&ConstantGroup->m_Buffer = (bufferDesc.Size + 15) & ~15;
		};

		// Each buffer is optional (nullptr if nonexistent)
		Assert(MaxGroups == 3);

		mapBufferConsts(Reflector->GetConstantBufferByName("PerGeometry"), &ConstantGroups[2]);
		mapBufferConsts(Reflector->GetConstantBufferByName("PerMaterial"), &ConstantGroups[1]);
		mapBufferConsts(Reflector->GetConstantBufferByName("PerTechnique"), &ConstantGroups[0]);
	}

	void ReflectSamplers(ID3D11ShaderReflection *Reflector, std::function<const char *(int Index)> GetSampler)
	{
		D3D11_SHADER_DESC desc;
		Assert(SUCCEEDED(Reflector->GetDesc(&desc)));

		if (desc.BoundResources <= 0)
			return;

		// Loop through all shader resources, then pick out sampler types specifically
		for (uint32_t i = 0; i < desc.BoundResources; i++)
		{
			D3D11_SHADER_INPUT_BIND_DESC inputDesc;
			if (FAILED(Reflector->GetResourceBindingDesc(i, &inputDesc)))
				continue;

			if (inputDesc.Type != D3D_SIT_SAMPLER)
				continue;

			// Do a partial string match
			const char *ourSamplerName = GetSampler(inputDesc.BindPoint);
			const char *dxSamplerName = inputDesc.Name;

			AssertMsgVa(_strnicmp(ourSamplerName, dxSamplerName, strlen(ourSamplerName)) == 0, "Sampler names don't match (%s != %s)", ourSamplerName, dxSamplerName);
		}
	}

	void Renderer::ValidateShaderReplacement(ID3D11PixelShader *Original, ID3D11PixelShader *Replacement)
	{
		ValidateShaderReplacement(Original, Replacement, __uuidof(ID3D11PixelShader));
	}

	void Renderer::ValidateShaderReplacement(ID3D11VertexShader *Original, ID3D11VertexShader *Replacement)
	{
		ValidateShaderReplacement(Original, Replacement, __uuidof(ID3D11VertexShader));
	}

	void Renderer::ValidateShaderReplacement(ID3D11ComputeShader *Original, ID3D11ComputeShader *Replacement)
	{
		ValidateShaderReplacement(Original, Replacement, __uuidof(ID3D11ComputeShader));
	}

	void Renderer::ValidateShaderReplacement(void *Original, void *Replacement, const GUID& Guid)
	{
		return;
		// First get the shader<->bytecode entry
		const auto& oldData = m_ShaderBytecodeMap.find(Original);
		const auto& newData = m_ShaderBytecodeMap.find(Replacement);

		Assert(oldData != m_ShaderBytecodeMap.end() && newData != m_ShaderBytecodeMap.end());

		// Disassemble both shaders, then compare the string output (case insensitive)
		UINT stripFlags = D3DCOMPILER_STRIP_REFLECTION_DATA | D3DCOMPILER_STRIP_DEBUG_INFO | D3DCOMPILER_STRIP_TEST_BLOBS | D3DCOMPILER_STRIP_PRIVATE_DATA;
		ID3DBlob *oldStrippedBlob = nullptr;
		ID3DBlob *newStrippedBlob = nullptr;

		Assert(SUCCEEDED(D3DStripShader(oldData->second.first, oldData->second.second, stripFlags, &oldStrippedBlob)));
		Assert(SUCCEEDED(D3DStripShader(newData->second.first, newData->second.second, stripFlags, &newStrippedBlob)));

		UINT disasmFlags = D3D_DISASM_ENABLE_INSTRUCTION_OFFSET;
		ID3DBlob *oldDataBlob = nullptr;
		ID3DBlob *newDataBlob = nullptr;

		Assert(SUCCEEDED(D3DDisassemble(oldStrippedBlob->GetBufferPointer(), oldStrippedBlob->GetBufferSize(), disasmFlags, nullptr, &oldDataBlob)));
		Assert(SUCCEEDED(D3DDisassemble(newStrippedBlob->GetBufferPointer(), newStrippedBlob->GetBufferSize(), disasmFlags, nullptr, &newDataBlob)));

		const char *oldDataStr = (const char *)oldDataBlob->GetBufferPointer();
		const char *newDataStr = (const char *)newDataBlob->GetBufferPointer();

		// Split the strings into multiple lines for better feedback info. Also skip some debug information
		// at the top of the file.
		auto tokenize = [](const std::string& str, std::vector<std::string> *tokens)
		{
			std::string::size_type lastPos = str.find_first_not_of("\n", 0);
			std::string::size_type pos = str.find_first_of("\n", lastPos);

			while (pos != std::string::npos || lastPos != std::string::npos)
			{
				tokens->push_back(str.substr(lastPos, pos - lastPos));
				lastPos = str.find_first_not_of("\n", pos);
				pos = str.find_first_of("\n", lastPos);
			}
		};

		std::vector<std::string> tokensOld;
		tokenize(std::string(oldDataStr, oldDataStr + oldDataBlob->GetBufferSize()), &tokensOld);

		std::vector<std::string> tokensNew;
		tokenize(std::string(newDataStr, newDataStr + newDataBlob->GetBufferSize()), &tokensNew);

		Assert(tokensOld.size() == tokensNew.size());

		for (size_t i = 0; i < tokensOld.size(); i++)
		{
			// Does the line match 1:1?
			if (_stricmp(tokensOld[i].c_str(), tokensNew[i].c_str()) == 0)
				continue;

			// Skip "Approximately X instruction slots used" which is not always accurate.
			// Skip "dcl_constantbuffer" which is not always in order.
			if (strstr(tokensOld[i].c_str(), "// Approximately") || strstr(tokensOld[i].c_str(), "dcl_constantbuffer"))
				continue;

			AssertMsgVa(false, "Shader disasm doesn't match.\n\n%s\n%s", tokensOld[i].c_str(), tokensNew[i].c_str());
		}

		oldStrippedBlob->Release();
		newStrippedBlob->Release();
		oldDataBlob->Release();
		newDataBlob->Release();
	}

	void Renderer::RegisterShaderBytecode(void *Shader, const void *Bytecode, size_t BytecodeLength)
	{
		// Grab a copy since the pointer isn't going to be valid forever
		void *codeCopy = malloc(BytecodeLength);
		memcpy(codeCopy, Bytecode, BytecodeLength);

		m_ShaderBytecodeMap.emplace(Shader, std::make_pair(codeCopy, BytecodeLength));
	}

	const std::pair<void *, size_t>& Renderer::GetShaderBytecode(void *Shader)
	{
		return m_ShaderBytecodeMap.at(Shader);
	}

	ID3DBlob *Renderer::CompileShader(const wchar_t *FilePath, const std::vector<std::pair<const char *, const char *>>& Defines, const char *ProgramType)
	{
		// Build defines (aka convert vector->D3DCONSTANT array)
		D3D_SHADER_MACRO macros[20 + 3 + 1];
		memset(macros, 0, sizeof(macros));

		AssertMsg(Defines.size() <= 20, "Not enough space reserved for #defines and null terminator");

		for (size_t i = 0; i < Defines.size(); i++)
		{
			macros[i].Name = Defines[i].first;
			macros[i].Definition = Defines[i].second;
		}

		macros[Defines.size() + 0].Name = "PLACEHOLDER_TYPE";
		macros[Defines.size() + 0].Definition = "";
		macros[Defines.size() + 1].Name = "WINPC";
		macros[Defines.size() + 1].Definition = "";
		macros[Defines.size() + 2].Name = "DX11";
		macros[Defines.size() + 2].Definition = "";

		if (!_stricmp(ProgramType, "ps_5_0"))
			macros[Defines.size() + 0].Name = "PIXELSHADER";
		else if (!_stricmp(ProgramType, "vs_5_0"))
			macros[Defines.size() + 0].Name = "VERTEXSHADER";
		else if (!_stricmp(ProgramType, "hs_5_0"))
			macros[Defines.size() + 0].Name = "HULLSHADER";
		else if (!_stricmp(ProgramType, "ds_5_0"))
			macros[Defines.size() + 0].Name = "DOMAINSHADER";
		else if (!_stricmp(ProgramType, "cs_5_0"))
			macros[Defines.size() + 0].Name = "COMPUTESHADER";
		else
			Assert(false);

		// Compiler setup
		UINT flags = D3DCOMPILE_DEBUG | D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
		ID3DBlob *shaderBlob = nullptr;
		ID3DBlob *shaderErrors = nullptr;

		if (FAILED(D3DCompileFromFile(FilePath, macros, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", ProgramType, flags, 0, &shaderBlob, &shaderErrors)))
		{
			AssertMsgVa(false, "Shader compilation failed:\n\n%s", shaderErrors ? (const char *)shaderErrors->GetBufferPointer() : "Unknown error");

			if (shaderBlob)
				shaderBlob->Release();

			if (shaderErrors)
				shaderErrors->Release();

			return nullptr;
		}

		if (shaderErrors)
			shaderErrors->Release();

		return shaderBlob;
	}

	VertexShader *Renderer::CompileVertexShader(const wchar_t *FilePath, const std::vector<std::pair<const char *, const char *>>& Defines, std::function<const char *(int Index)> GetConstant)
	{
		ID3DBlob *shaderBlob = CompileShader(FilePath, Defines, "vs_5_0");

		if (!shaderBlob)
			return nullptr;

		void *rawPtr = malloc(sizeof(VertexShader) + shaderBlob->GetBufferSize());
		VertexShader *vs = new (rawPtr) VertexShader;

		// Shader reflection: gather constant buffer variable offsets
		ID3D11ShaderReflection *reflector;
		Assert(SUCCEEDED(D3DReflect(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), __uuidof(ID3D11ShaderReflection), (void **)&reflector)));

		ReflectConstantBuffers(reflector, vs->m_ConstantGroups, ARRAYSIZE(vs->m_ConstantGroups), GetConstant, vs->m_ConstantOffsets, ARRAYSIZE(vs->m_ConstantOffsets));

		// Register shader with the DX runtime itself
		Assert(SUCCEEDED(m_Device->CreateVertexShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr, &vs->m_Shader)));

		// Final step: append raw bytecode to the end of the struct
		memcpy(vs->m_RawBytecode, shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize());
		vs->m_ShaderLength = (uint32_t)shaderBlob->GetBufferSize();

		RegisterShaderBytecode(vs->m_Shader, shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize());
		reflector->Release();
		shaderBlob->Release();

		return vs;
	}

	PixelShader *Renderer::CompilePixelShader(const wchar_t *FilePath, const std::vector<std::pair<const char *, const char *>>& Defines, std::function<const char *(int Index)> GetSampler, std::function<const char *(int Index)> GetConstant)
	{
		ID3DBlob *shaderBlob = CompileShader(FilePath, Defines, "ps_5_0");

		if (!shaderBlob)
			return nullptr;

		void *rawPtr = malloc(sizeof(PixelShader));
		PixelShader *ps = new (rawPtr) PixelShader;

		// Shader reflection: gather constant buffer variable offsets and check for valid sampler mappings
		ID3D11ShaderReflection *reflector;
		Assert(SUCCEEDED(D3DReflect(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), __uuidof(ID3D11ShaderReflection), (void **)&reflector)));

		ReflectConstantBuffers(reflector, ps->m_ConstantGroups, ARRAYSIZE(ps->m_ConstantGroups), GetConstant, ps->m_ConstantOffsets, ARRAYSIZE(ps->m_ConstantOffsets));
		ReflectSamplers(reflector, GetSampler);

		// Register shader with the DX runtime itself
		Assert(SUCCEEDED(m_Device->CreatePixelShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr, &ps->m_Shader)));

		RegisterShaderBytecode(ps->m_Shader, shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize());
		reflector->Release();
		shaderBlob->Release();

		return ps;
	}

	HullShader *Renderer::CompileHullShader(const wchar_t *FilePath, const std::vector<std::pair<const char *, const char *>>& Defines)
	{
		ID3DBlob *shaderBlob = CompileShader(FilePath, Defines, "hs_5_0");

		if (!shaderBlob)
			return nullptr;

		void *rawPtr = malloc(sizeof(HullShader));
		HullShader *hs = new (rawPtr) HullShader;

		// Register shader with the DX runtime itself
		Assert(SUCCEEDED(m_Device->CreateHullShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr, &hs->m_Shader)));

		RegisterShaderBytecode(hs->m_Shader, shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize());
		shaderBlob->Release();

		return hs;
	}

	DomainShader *Renderer::CompileDomainShader(const wchar_t *FilePath, const std::vector<std::pair<const char *, const char *>>& Defines)
	{
		ID3DBlob *shaderBlob = CompileShader(FilePath, Defines, "ds_5_0");

		if (!shaderBlob)
			return nullptr;

		void *rawPtr = malloc(sizeof(DomainShader));
		DomainShader *ds = new (rawPtr) DomainShader;

		// Register shader with the DX runtime itself
		Assert(SUCCEEDED(m_Device->CreateDomainShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr, &ds->m_Shader)));

		RegisterShaderBytecode(ds->m_Shader, shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize());
		shaderBlob->Release();

		return ds;
	}

	CustomConstantGroup Renderer::GetShaderConstantGroup(uint32_t Size, ConstantGroupLevel Level)
	{
		CustomConstantGroup temp;
		temp.m_Buffer = MapConstantBuffer(&temp.m_Map.pData, &Size, &temp.m_UnifiedByteOffset, Level);
		temp.m_Map.DepthPitch = Size;
		temp.m_Map.RowPitch = Size;
		temp.m_Unified = (temp.m_Buffer == ShaderConstantBuffer->D3DBuffer);

		// DirectX expects you to overwrite the entire buffer. **SKYRIM DOES NOT**, so I'm zeroing it now.
		memset(temp.m_Map.pData, 0, Size);
		return temp;
	}

	VertexCGroup Renderer::GetShaderConstantGroup(VertexShader *Shader, ConstantGroupLevel Level)
	{
		ConstantGroup<VertexShader> temp;
		Buffer *group = &Shader->m_ConstantGroups[Level];

		if (group->m_Buffer)
		{
			D3D11_BUFFER_DESC desc;

			if ((uintptr_t)group->m_Buffer > 0x10000)
			{
				group->m_Buffer->GetDesc(&desc);
				group->m_Buffer = (ID3D11Buffer *)desc.ByteWidth;
			}
			else
			{
				desc.ByteWidth = (uint32_t)group->m_Buffer;
			}

			temp = GetShaderConstantGroup(desc.ByteWidth, Level);
		}
		else
		{
			temp.m_Map.pData = group->m_Data;
			// Size to memset() is unknown here
		}

		temp.m_Shader = Shader;
		return temp;
	}

	PixelCGroup Renderer::GetShaderConstantGroup(PixelShader *Shader, ConstantGroupLevel Level)
	{
		ConstantGroup<PixelShader> temp;
		Buffer *group = &Shader->m_ConstantGroups[Level];

		if (group->m_Buffer)
		{
			D3D11_BUFFER_DESC desc;

			if ((uintptr_t)group->m_Buffer > 0x10000)
			{
				group->m_Buffer->GetDesc(&desc);
				group->m_Buffer = (ID3D11Buffer *)desc.ByteWidth;
			}
			else
			{
				desc.ByteWidth = (uint32_t)group->m_Buffer;
			}

			temp = GetShaderConstantGroup(desc.ByteWidth, Level);
		}
		else
		{
			temp.m_Map.pData = group->m_Data;
			// Size to memset() is unknown here
		}

		temp.m_Shader = Shader;
		return temp;
	}

	void Renderer::FlushConstantGroup(CustomConstantGroup *Group)
	{
		if (Group->m_Buffer)
		{
			if (!Group->m_Unified)
				m_DeviceContext->Unmap(Group->m_Buffer, 0);

			// Invalidate the data pointer only - ApplyConstantGroup still needs RowPitch info
			Group->m_Map.pData = (void *)0xFEFEFEFEFEFEFEFE;
		}
	}

	void Renderer::FlushConstantGroupVSPS(ConstantGroup<VertexShader> *VertexGroup, ConstantGroup<PixelShader> *PixelGroup)
	{
		if (VertexGroup)
			FlushConstantGroup(VertexGroup);

		if (PixelGroup)
			FlushConstantGroup(PixelGroup);
	}

	void Renderer::ApplyConstantGroupVS(const CustomConstantGroup *Group, ConstantGroupLevel Level)
	{
		if (Group->m_Unified)
		{
			UINT offset = Group->m_UnifiedByteOffset / 16;
			UINT size = Group->m_Map.RowPitch / 16;
			m_DeviceContext->VSSetConstantBuffers1(Level, 1, &Group->m_Buffer, &offset, &size);
		}
		else
		{
			m_DeviceContext->VSSetConstantBuffers(Level, 1, &Group->m_Buffer);
		}
	}

	void Renderer::ApplyConstantGroupPS(const CustomConstantGroup *Group, ConstantGroupLevel Level)
	{
		if (Group->m_Unified)
		{
			UINT offset = Group->m_UnifiedByteOffset / 16;
			UINT size = Group->m_Map.RowPitch / 16;
			m_DeviceContext->PSSetConstantBuffers1(Level, 1, &Group->m_Buffer, &offset, &size);
		}
		else
		{
			m_DeviceContext->PSSetConstantBuffers(Level, 1, &Group->m_Buffer);
		}
	}

	void Renderer::ApplyConstantGroupVSPS(const ConstantGroup<VertexShader> *VertexGroup, const ConstantGroup<PixelShader> *PixelGroup, ConstantGroupLevel Level)
	{
		if (VertexGroup)
			ApplyConstantGroupVS(VertexGroup, Level);

		if (PixelGroup)
			ApplyConstantGroupPS(PixelGroup, Level);
	}

	void Renderer::IncRef(TriShape *Shape)
	{
		InterlockedIncrement(&Shape->m_RefCount);
	}

	void Renderer::DecRef(TriShape *Shape)
	{
		if (InterlockedDecrement(&Shape->m_RefCount) == 0)
		{
			MOC::RemoveCachedVerticesAndIndices(Shape);

			if (Shape->m_VertexBuffer)
				Shape->m_VertexBuffer->Release();

			if (Shape->m_IndexBuffer)
				Shape->m_IndexBuffer->Release();

			AutoFunc(void(__fastcall *)(void *), sub_1400F7DC0, 0xF7DC0);
			AutoFunc(void(__fastcall *)(void *, __int64), sub_140136112C, 0x136112C);

			if (Shape->m_RawVertexData)
				sub_1400F7DC0(Shape->m_RawVertexData);

			if (Shape->m_RawIndexData)
				sub_1400F7DC0(Shape->m_RawIndexData);

			sub_140136112C(Shape, sizeof(TriShape));
		}
	}

	void Renderer::IncRef(DynamicTriShape *Shape)
	{
		InterlockedIncrement(&Shape->m_RefCount);
	}

	void Renderer::DecRef(DynamicTriShape *Shape)
	{
		if (InterlockedDecrement(&Shape->m_RefCount) == 0)
		{
			MOC::RemoveCachedVerticesAndIndices(Shape);

			if (Shape->m_VertexBuffer)
				Shape->m_VertexBuffer->Release();

			Shape->m_IndexBuffer->Release();

			AutoFunc(void(__fastcall *)(void *), sub_1400F7DC0, 0xF7DC0);
			AutoFunc(void(__fastcall *)(void *, __int64), sub_140136112C, 0x136112C);

			sub_1400F7DC0(Shape->m_RawIndexData);
			sub_140136112C(Shape, sizeof(DynamicTriShape));
		}
	}
}