#include "pch.h"
#include "water.h"

#include "dx/dx_buffer.h"
#include "dx/dx_pipeline.h"
#include "dx/dx_profiling.h"

#include "rendering/render_utils.h"
#include "rendering/render_pass.h"
#include "rendering/render_resources.h"

#include "geometry/mesh_builder.h"

#include "water_rs.hlsli"
#include "transform.hlsli"


static dx_pipeline waterPipeline;

static ref<dx_texture> normalmap1;
static ref<dx_texture> normalmap2;
static ref<dx_texture> foamTexture;
static ref<dx_texture> noiseTexture;

void initializeWaterPipelines()
{
	{
		auto desc = CREATE_GRAPHICS_PIPELINE
			.cullingOff()
			.renderTargets(transparentLightPassFormats, arraysize(transparentLightPassFormats), depthStencilFormat);

		waterPipeline = createReloadablePipeline(desc, { "water_vs", "water_ps" });
	}

	normalmap1 = loadTextureFromFile("assets/water/waterNM1.png", image_load_flags_noncolor | image_load_flags_gen_mips_on_cpu | image_load_flags_cache_to_dds);
	normalmap2 = loadTextureFromFile("assets/water/waterNM2.png", image_load_flags_noncolor | image_load_flags_gen_mips_on_cpu | image_load_flags_cache_to_dds);
	foamTexture = loadTextureFromFile("assets/water/waterFoam.dds", image_load_flags_noncolor | image_load_flags_gen_mips_on_cpu | image_load_flags_cache_to_dds);
	noiseTexture = loadTextureFromFile("assets/water/waterNoise.dds", image_load_flags_noncolor | image_load_flags_gen_mips_on_cpu | image_load_flags_cache_to_dds);
}


struct water_render_data
{
	mat4 m;

	water_settings settings;
	float time;
};

struct water_pipeline
{
	using render_data_t = water_render_data;
	
	PIPELINE_SETUP_DECL;
	PIPELINE_RENDER_DECL;
};

PIPELINE_SETUP_IMPL(water_pipeline)
{
	cl->setPipelineState(*waterPipeline.pipeline);
	cl->setGraphicsRootSignature(*waterPipeline.rootSignature);

	cl->setPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	
	cl->setGraphicsDynamicConstantBuffer(WATER_RS_CAMERA, common.cameraCBV);
	cl->setGraphicsDynamicConstantBuffer(WATER_RS_LIGHTING, common.lightingCBV);
	cl->setDescriptorHeapSRV(WATER_RS_TEXTURES, 0, common.opaqueColor);
	cl->setDescriptorHeapSRV(WATER_RS_TEXTURES, 1, common.opaqueDepth);
	cl->setDescriptorHeapSRV(WATER_RS_TEXTURES, 2, normalmap1 ? normalmap1->defaultSRV : render_resources::nullTextureSRV);
	cl->setDescriptorHeapSRV(WATER_RS_TEXTURES, 3, normalmap2 ? normalmap2->defaultSRV : render_resources::nullTextureSRV);
	cl->setDescriptorHeapSRV(WATER_RS_TEXTURES, 4, foamTexture ? foamTexture->defaultSRV : render_resources::nullTextureSRV);
	cl->setDescriptorHeapSRV(WATER_RS_TEXTURES, 5, noiseTexture ? noiseTexture->defaultSRV : render_resources::nullTextureSRV);

	dx_cpu_descriptor_handle nullTexture = render_resources::nullTextureSRV;

	cl->setDescriptorHeapSRV(WATER_RS_FRAME_CONSTANTS, 0, common.irradiance);
	cl->setDescriptorHeapSRV(WATER_RS_FRAME_CONSTANTS, 1, common.prefilteredRadiance);
	cl->setDescriptorHeapSRV(WATER_RS_FRAME_CONSTANTS, 2, render_resources::brdfTex);
	cl->setDescriptorHeapSRV(WATER_RS_FRAME_CONSTANTS, 3, common.shadowMap);
	cl->setDescriptorHeapSRV(WATER_RS_FRAME_CONSTANTS, 4, common.aoTexture ? common.aoTexture : render_resources::whiteTexture);
	cl->setDescriptorHeapSRV(WATER_RS_FRAME_CONSTANTS, 5, common.sssTexture ? common.sssTexture : render_resources::whiteTexture);
	cl->setDescriptorHeapSRV(WATER_RS_FRAME_CONSTANTS, 6, common.ssrTexture ? common.ssrTexture->defaultSRV : nullTexture);
}

PIPELINE_RENDER_IMPL(water_pipeline)
{
	PROFILE_ALL(cl, "Water");

	water_cb cb;
	cb.deepColor = rc.data.settings.deepWaterColor;
	cb.shallowColor = rc.data.settings.shallowWaterColor;
	cb.shallowDepth = rc.data.settings.shallowDepth;
	cb.transitionStrength = rc.data.settings.transitionStrength;
	cb.uvOffset = normalize(vec2(1.f, 1.f)) * rc.data.time * 0.05f;
	cb.uvScale = rc.data.settings.uvScale;
	cb.normalmapStrength = rc.data.settings.normalStrength;

	cl->setGraphics32BitConstants(WATER_RS_TRANSFORM, transform_cb{ viewProj * rc.data.m, rc.data.m });
	cl->setGraphics32BitConstants(WATER_RS_SETTINGS, cb);
	cl->draw(4, 1, 0, 0);
}

void water_component::update(float dt)
{
	time += dt;
}

void water_component::render(const render_camera& camera, transparent_render_pass* renderPass, vec3 positionOffset, vec2 scale, uint32 entityID)
{
	renderPass->renderObject<water_pipeline>({ createModelMatrix(positionOffset, quat::identity, vec3(scale.x, 1.f, scale.y)), settings, time });
}