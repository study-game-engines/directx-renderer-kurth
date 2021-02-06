#include "pch.h"
#include "dx_renderer.h"
#include "dx_command_list.h"
#include "dx_render_target.h"
#include "dx_pipeline.h"
#include "geometry.h"
#include "dx_texture.h"
#include "dx_barrier_batcher.h"
#include "texture_preprocessing.h"
#include "skinning.h"
#include "dx_context.h"
#include "dx_profiling.h"
#include "random.h"

#include "depth_only_rs.hlsli"
#include "outline_rs.hlsli"
#include "sky_rs.hlsli"
#include "light_culling_rs.hlsli"
#include "camera.hlsli"
#include "transform.hlsli"
#include "random.h"

#include "raytracing.h"


static ref<dx_texture> whiteTexture;
static ref<dx_texture> blackTexture;
static ref<dx_texture> blackCubeTexture;

static ref<dx_texture> shadowMap;

static dx_pipeline clearObjectIDsPipeline;

static dx_pipeline depthOnlyPipeline;
static dx_pipeline animatedDepthOnlyPipeline;
static dx_pipeline shadowPipeline;
static dx_pipeline pointLightShadowPipeline;

static dx_pipeline textureSkyPipeline;
static dx_pipeline proceduralSkyPipeline;

static dx_pipeline blitPipeline;


static dx_pipeline atmospherePipeline;

static dx_pipeline outlineMarkerPipeline;
static dx_pipeline outlineDrawerPipeline;

static dx_pipeline worldSpaceFrustaPipeline;
static dx_pipeline lightCullingPipeline;

static dx_pipeline taaPipeline;
static dx_pipeline tonemapPipeline;
static dx_pipeline presentPipeline;


static dx_mesh positionOnlyMesh;

static submesh_info cubeMesh;



static ref<dx_texture> brdfTex;



DXGI_FORMAT dx_renderer::screenFormat;

dx_cpu_descriptor_handle dx_renderer::nullTextureSRV;
dx_cpu_descriptor_handle dx_renderer::nullBufferSRV;


static bool performedSkinning;

static vec2 haltonSequence[128];


enum stencil_flags
{
	stencil_flag_selected_object = (1 << 0),
};


void dx_renderer::initializeCommon(DXGI_FORMAT screenFormat)
{
	dx_renderer::screenFormat = screenFormat;

	{
		uint8 white[] = { 255, 255, 255, 255 };
		whiteTexture = createTexture(white, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM);
		SET_NAME(whiteTexture->resource, "White");
	}
	{
		uint8 black[] = { 0, 0, 0, 255 };
		blackTexture = createTexture(black, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM);
		SET_NAME(blackTexture->resource, "Black");

		blackCubeTexture = createCubeTexture(black, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM);
		SET_NAME(blackCubeTexture->resource, "Black cube");
	}

	nullTextureSRV = dxContext.descriptorAllocatorCPU.getFreeHandle().createNullTextureSRV();
	nullBufferSRV = dxContext.descriptorAllocatorCPU.getFreeHandle().createNullBufferSRV();

	initializeTexturePreprocessing();
	initializeSkinning();


	shadowMap = createDepthTexture(SHADOW_MAP_WIDTH, SHADOW_MAP_HEIGHT, shadowDepthFormat);


	// Sky.
	{
		auto desc = CREATE_GRAPHICS_PIPELINE
			.inputLayout(inputLayout_position)
			.renderTargets(hdrFormat)
			.depthSettings(false, false)
			.cullFrontFaces();

		proceduralSkyPipeline = createReloadablePipeline(desc, { "sky_vs", "sky_procedural_ps" });
		textureSkyPipeline = createReloadablePipeline(desc, { "sky_vs", "sky_texture_ps" });
	}

	// Depth prepass.
	{
		DXGI_FORMAT depthOnlyFormat[] = { screenVelocitiesFormat, objectIDsFormat };

		auto desc = CREATE_GRAPHICS_PIPELINE
			.renderTargets(depthOnlyFormat, arraysize(depthOnlyFormat), hdrDepthStencilFormat)
			.inputLayout(inputLayout_position_uv_normal_tangent);

		depthOnlyPipeline = createReloadablePipeline(desc, { "depth_only_vs", "depth_only_ps" }, rs_in_vertex_shader);
		animatedDepthOnlyPipeline = createReloadablePipeline(desc, { "depth_only_animated_vs", "depth_only_ps" }, rs_in_vertex_shader);
	}

	// Shadow.
	{
		auto desc = CREATE_GRAPHICS_PIPELINE
			.renderTargets(0, 0, shadowDepthFormat)
			.inputLayout(inputLayout_position_uv_normal_tangent)
			//.cullFrontFaces()
			;

		shadowPipeline = createReloadablePipeline(desc, { "shadow_vs" }, rs_in_vertex_shader);
		pointLightShadowPipeline = createReloadablePipeline(desc, { "shadow_point_light_vs", "shadow_point_light_ps" }, rs_in_vertex_shader);
	}

	// Outline.
	{
		auto markerDesc = CREATE_GRAPHICS_PIPELINE
			.inputLayout(inputLayout_position_uv_normal_tangent)
			.renderTargets(0, 0, hdrDepthStencilFormat)
			.stencilSettings(D3D12_COMPARISON_FUNC_ALWAYS, 
				D3D12_STENCIL_OP_REPLACE, 
				D3D12_STENCIL_OP_REPLACE, 
				D3D12_STENCIL_OP_KEEP, 
				D3D12_DEFAULT_STENCIL_READ_MASK, 
				stencil_flag_selected_object) // Mark selected object.
			.depthSettings(false, false);

		outlineMarkerPipeline = createReloadablePipeline(markerDesc, { "outline_vs" }, rs_in_vertex_shader);


		auto drawerDesc = CREATE_GRAPHICS_PIPELINE
			.renderTargets(hdrFormat, hdrDepthStencilFormat)
			.stencilSettings(D3D12_COMPARISON_FUNC_EQUAL,
				D3D12_STENCIL_OP_KEEP,
				D3D12_STENCIL_OP_KEEP,
				D3D12_STENCIL_OP_KEEP,
				stencil_flag_selected_object, // Read only selected object bit.
				0)
			.depthSettings(false, false);

		outlineDrawerPipeline = createReloadablePipeline(drawerDesc, { "fullscreen_triangle_vs", "outline_ps" });
	}

	// Blit.
	{
		auto desc = CREATE_GRAPHICS_PIPELINE
			.renderTargets(screenFormat)
			.depthSettings(false, false);

		blitPipeline = createReloadablePipeline(desc, { "fullscreen_triangle_vs", "blit_ps" });
	}

	// Light culling.
	{
		worldSpaceFrustaPipeline = createReloadablePipeline("world_space_tiled_frusta_cs");
		lightCullingPipeline = createReloadablePipeline("light_culling_cs");
	}

	// Atmosphere.
	{
		atmospherePipeline = createReloadablePipeline("atmosphere_cs");
	}

	// Clear object IDs.
	{
		clearObjectIDsPipeline = createReloadablePipeline("clear_object_ids_cs");
	}

	// Post processing.
	{
		taaPipeline = createReloadablePipeline("taa_composite_cs");
		tonemapPipeline = createReloadablePipeline("tonemap_cs");
		presentPipeline = createReloadablePipeline("present_cs");
	}


	pbr_material::initializePipeline();

	createAllPendingReloadablePipelines();



	{
		cpu_mesh mesh(mesh_creation_flags_with_positions);
		cubeMesh = mesh.pushCube(1.f);
		positionOnlyMesh = mesh.createDXMesh();
	}

	{
		dx_command_list* cl = dxContext.getFreeRenderCommandList();
		brdfTex = integrateBRDF(cl);
		dxContext.executeCommandList(cl);
	}

	for (uint32 i = 0; i < arraysize(haltonSequence); ++i)
	{
		haltonSequence[i] = halton23(i) * 2.f - vec2(1.f);
	}
}

void dx_renderer::initialize(uint32 windowWidth, uint32 windowHeight, bool renderObjectIDs)
{
	this->windowWidth = windowWidth;
	this->windowHeight = windowHeight;

	recalculateViewport(false);

	// HDR render target.
	{
		depthStencilBuffer = createDepthTexture(renderWidth, renderHeight, hdrDepthStencilFormat);
		hdrColorTexture = createTexture(0, renderWidth, renderHeight, hdrFormat, false, true, true, D3D12_RESOURCE_STATE_RENDER_TARGET);
		worldNormalsTexture = createTexture(0, renderWidth, renderHeight, worldNormalsFormat, false, true, false, D3D12_RESOURCE_STATE_RENDER_TARGET);
		screenVelocitiesTexture = createTexture(0, renderWidth, renderHeight, screenVelocitiesFormat, false, true, false, D3D12_RESOURCE_STATE_RENDER_TARGET);

		taaTextures[taaHistoryIndex] = createTexture(0, renderWidth, renderHeight, hdrPostProcessFormat, false, false, true, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		taaTextures[1 - taaHistoryIndex] = createTexture(0, renderWidth, renderHeight, hdrPostProcessFormat, false, false, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		ldrPostProcessingTexture = createTexture(0, renderWidth, renderHeight, ldrPostProcessFormat, false, false, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		SET_NAME(hdrColorTexture->resource, "HDR Color");
		SET_NAME(worldNormalsTexture->resource, "World normals");
		SET_NAME(screenVelocitiesTexture->resource, "Screen velocities");

		SET_NAME(taaTextures[0]->resource, "TAA 0");
		SET_NAME(taaTextures[1]->resource, "TAA 1");

		SET_NAME(ldrPostProcessingTexture->resource, "LDR Post processing");
	}

	// Frame result.
	{
		frameResult = createTexture(0, windowWidth, windowHeight, screenFormat, false, true, true);
		SET_NAME(frameResult->resource, "Frame result");
	}

	if (renderObjectIDs)
	{
		for (uint32 i = 0; i < NUM_BUFFERED_FRAMES; ++i)
		{
			hoveredObjectIDReadbackBuffer[i] = createReadbackBuffer(getFormatSize(objectIDsFormat), 1);
		}

		objectIDsTexture = createTexture(0, renderWidth, renderHeight, objectIDsFormat, false, true, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		SET_NAME(objectIDsTexture->resource, "Object IDs");
	}

	for (uint32 i = 0; i < NUM_BUFFERED_FRAMES; ++i)
	{
		spotLightShadowInfoBuffer[i] = createUploadBuffer(sizeof(spot_shadow_info), 16, 0);
		pointLightShadowInfoBuffer[i] = createUploadBuffer(sizeof(point_shadow_info), 16, 0);
	}
}

void dx_renderer::beginFrameCommon()
{
	checkForChangedPipelines();
}

void dx_renderer::endFrameCommon()
{
	performedSkinning = performSkinning();
}

void dx_renderer::beginFrame(uint32 windowWidth, uint32 windowHeight)
{
	pointLights = 0;
	spotLights = 0;
	numPointLights = 0;
	numSpotLights = 0;

	if (this->windowWidth != windowWidth || this->windowHeight != windowHeight)
	{
		this->windowWidth = windowWidth;
		this->windowHeight = windowHeight;

		// Frame result.
		resizeTexture(frameResult, windowWidth, windowHeight);

		recalculateViewport(true);
	}

	if (objectIDsTexture)
	{
		uint16* id = (uint16*)mapBuffer(hoveredObjectIDReadbackBuffer[dxContext.bufferedFrameID], true);
		hoveredObjectID = *id;
		unmapBuffer(hoveredObjectIDReadbackBuffer[dxContext.bufferedFrameID], false);
	}

	opaqueRenderPass = 0;
	overlayRenderPass = 0;
	sunShadowRenderPass = 0;
	numSpotLightShadowRenderPasses = 0;
	numPointLightShadowRenderPasses = 0;

	pointLights = 0;
	spotLights = 0;
	numPointLights = 0;
	numSpotLights = 0;

	environment = 0;
}

void dx_renderer::recalculateViewport(bool resizeTextures)
{
	if (settings.aspectRatioMode == aspect_ratio_free)
	{
		windowXOffset = 0;
		windowYOffset = 0;
		renderWidth = windowWidth;
		renderHeight = windowHeight;
	}
	else
	{
		const float targetAspect = settings.aspectRatioMode == aspect_ratio_fix_16_9 ? (16.f / 9.f) : (16.f / 10.f);

		float aspect = (float)windowWidth / (float)windowHeight;
		if (aspect > targetAspect)
		{
			renderWidth = (uint32)(windowHeight * targetAspect);
			renderHeight = windowHeight;
			windowXOffset = (windowWidth - renderWidth) / 2;
			windowYOffset = 0;
		}
		else
		{
			renderWidth = windowWidth;
			renderHeight = (uint32)(windowWidth / targetAspect);
			windowXOffset = 0;
			windowYOffset = (windowHeight - renderHeight) / 2;
		}
	}

	//renderWidth = (uint32)windowViewport.Width;
	//renderHeight = (uint32)windowViewport.Height;

	if (resizeTextures)
	{
		resizeTexture(hdrColorTexture, renderWidth, renderHeight, D3D12_RESOURCE_STATE_RENDER_TARGET);
		resizeTexture(worldNormalsTexture, renderWidth, renderHeight, D3D12_RESOURCE_STATE_RENDER_TARGET);
		resizeTexture(screenVelocitiesTexture, renderWidth, renderHeight, D3D12_RESOURCE_STATE_RENDER_TARGET);
		resizeTexture(depthStencilBuffer, renderWidth, renderHeight);

		if (objectIDsTexture)
		{
			resizeTexture(objectIDsTexture, renderWidth, renderHeight, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		}

		resizeTexture(taaTextures[taaHistoryIndex], renderWidth, renderHeight, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		resizeTexture(taaTextures[1 - taaHistoryIndex], renderWidth, renderHeight, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		resizeTexture(ldrPostProcessingTexture, renderWidth, renderHeight, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	}

	allocateLightCullingBuffers();
}

void dx_renderer::allocateLightCullingBuffers()
{
	lightCullingBuffers.numTilesX = bucketize(renderWidth, LIGHT_CULLING_TILE_SIZE);
	lightCullingBuffers.numTilesY = bucketize(renderHeight, LIGHT_CULLING_TILE_SIZE);

	bool firstAllocation = lightCullingBuffers.lightGrid == nullptr;

	if (firstAllocation)
	{
		lightCullingBuffers.lightGrid = createTexture(0, lightCullingBuffers.numTilesX, lightCullingBuffers.numTilesY,
			DXGI_FORMAT_R16G16B16A16_UINT, false, false, true);
		SET_NAME(lightCullingBuffers.lightGrid->resource, "Light grid");

		lightCullingBuffers.lightIndexCounter = createBuffer(sizeof(uint32), 2, 0, true, true);
		lightCullingBuffers.pointLightIndexList = createBuffer(sizeof(uint32),
			lightCullingBuffers.numTilesX * lightCullingBuffers.numTilesY * MAX_NUM_LIGHTS_PER_TILE, 0, true);
		lightCullingBuffers.spotLightIndexList = createBuffer(sizeof(uint32),
			lightCullingBuffers.numTilesX * lightCullingBuffers.numTilesY * MAX_NUM_LIGHTS_PER_TILE, 0, true);
		lightCullingBuffers.tiledFrusta = createBuffer(sizeof(light_culling_view_frustum), lightCullingBuffers.numTilesX * lightCullingBuffers.numTilesY, 0, true);
	}
	else
	{
		resizeTexture(lightCullingBuffers.lightGrid, lightCullingBuffers.numTilesX, lightCullingBuffers.numTilesY);
		resizeBuffer(lightCullingBuffers.pointLightIndexList, lightCullingBuffers.numTilesX * lightCullingBuffers.numTilesY * MAX_NUM_LIGHTS_PER_TILE);
		resizeBuffer(lightCullingBuffers.spotLightIndexList, lightCullingBuffers.numTilesX * lightCullingBuffers.numTilesY * MAX_NUM_LIGHTS_PER_TILE);
		resizeBuffer(lightCullingBuffers.tiledFrusta, lightCullingBuffers.numTilesX * lightCullingBuffers.numTilesY);
	}
}

void dx_renderer::setCamera(const render_camera& camera)
{
	vec2 jitterOffset(0.f, 0.f);
	render_camera c;
	if (settings.enableTemporalAntialiasing)
	{
		jitterOffset = haltonSequence[dxContext.frameID % arraysize(haltonSequence)] / vec2((float)renderWidth, (float)renderHeight) * settings.cameraJitterStrength;
		c = camera.getJitteredVersion(jitterOffset);
	}
	else
	{
		c = camera;
	}

	this->camera.prevFrameViewProj = this->camera.viewProj;
	this->camera.viewProj = c.viewProj;
	this->camera.view = c.view;
	this->camera.proj = c.proj;
	this->camera.invViewProj = c.invViewProj;
	this->camera.invView = c.invView;
	this->camera.invProj = c.invProj;
	this->camera.position = vec4(c.position, 1.f);
	this->camera.forward = vec4(c.rotation * vec3(0.f, 0.f, -1.f), 0.f);
	this->camera.right = vec4(c.rotation * vec3(1.f, 0.f, 0.f), 0.f);
	this->camera.up = vec4(c.rotation * vec3(0.f, 1.f, 0.f), 0.f);
	this->camera.projectionParams = vec4(c.nearPlane, c.farPlane, c.farPlane / c.nearPlane, 1.f - c.farPlane / c.nearPlane);
	this->camera.screenDims = vec2((float)renderWidth, (float)renderHeight);
	this->camera.invScreenDims = vec2(1.f / renderWidth, 1.f / renderHeight);
	this->camera.prevFrameJitter = this->camera.jitter;
	this->camera.jitter = jitterOffset;
}

void dx_renderer::setEnvironment(const ref<pbr_environment>& environment)
{
	this->environment = environment;
}

void dx_renderer::setSun(const directional_light& light)
{
	sun.cascadeDistances = light.cascadeDistances;
	sun.bias = light.bias;
	sun.direction = light.direction;
	sun.blendDistances = light.blendDistances;
	sun.radiance = light.color * light.intensity;
	sun.numShadowCascades = light.numShadowCascades;

	memcpy(sun.vp, light.vp, sizeof(mat4) * light.numShadowCascades);
}

void dx_renderer::setPointLights(const ref<dx_buffer>& lights, uint32 numLights)
{
	pointLights = lights;
	numPointLights = numLights;
}

void dx_renderer::setSpotLights(const ref<dx_buffer>& lights, uint32 numLights)
{
	spotLights = lights;
	numSpotLights = numLights;
}

ref<dx_texture> dx_renderer::getWhiteTexture()
{
	return whiteTexture;
}

ref<dx_texture> dx_renderer::getBlackTexture()
{
	return blackTexture;
}

ref<dx_texture> dx_renderer::getShadowMap()
{
	return shadowMap;
}

void dx_renderer::endFrame(const user_input& input)
{
	bool aspectRatioModeChanged = settings.aspectRatioMode != oldSettings.aspectRatioMode;

	if (aspectRatioModeChanged)
	{
		recalculateViewport(true);
	}

	vec4 sunCPUShadowViewports[MAX_NUM_SUN_SHADOW_CASCADES];
	vec4 spotLightViewports[MAX_NUM_SPOT_LIGHT_SHADOW_PASSES];
	vec4 pointLightViewports[MAX_NUM_POINT_LIGHT_SHADOW_PASSES][2];

	{
		spot_shadow_info spotLightShadowInfos[16];
		point_shadow_info pointLightShadowInfos[16];

		if (sunShadowRenderPass)
		{
			for (uint32 i = 0; i < sun.numShadowCascades; ++i)
			{
				sunCPUShadowViewports[i] = sunShadowRenderPass->viewports[i];
				sun.viewports[i] = sunCPUShadowViewports[i] / vec4((float)SHADOW_MAP_WIDTH, (float)SHADOW_MAP_HEIGHT, (float)SHADOW_MAP_WIDTH, (float)SHADOW_MAP_HEIGHT);
			}
		}

		for (uint32 i = 0; i < numSpotLightShadowRenderPasses; ++i)
		{
			spot_shadow_info& si = spotLightShadowInfos[i];

			spotLightViewports[i] = spotLightShadowRenderPasses[i]->viewport;
			si.viewport = spotLightViewports[i] / vec4((float)SHADOW_MAP_WIDTH, (float)SHADOW_MAP_HEIGHT, (float)SHADOW_MAP_WIDTH, (float)SHADOW_MAP_HEIGHT);
			si.viewProj = spotLightShadowRenderPasses[i]->viewProjMatrix;
			si.bias = 0.000005f;
		}

		for (uint32 i = 0; i < numPointLightShadowRenderPasses; ++i)
		{
			point_shadow_info& si = pointLightShadowInfos[i];

			pointLightViewports[i][0] = pointLightShadowRenderPasses[i]->viewport0;
			pointLightViewports[i][1] = pointLightShadowRenderPasses[i]->viewport1;

			si.viewport0 = pointLightViewports[i][0] / vec4((float)SHADOW_MAP_WIDTH, (float)SHADOW_MAP_HEIGHT, (float)SHADOW_MAP_WIDTH, (float)SHADOW_MAP_HEIGHT);
			si.viewport1 = pointLightViewports[i][1] / vec4((float)SHADOW_MAP_WIDTH, (float)SHADOW_MAP_HEIGHT, (float)SHADOW_MAP_WIDTH, (float)SHADOW_MAP_HEIGHT);
		}


		updateUploadBufferData(spotLightShadowInfoBuffer[dxContext.bufferedFrameID], spotLightShadowInfos, (uint32)(sizeof(spot_shadow_info) * numSpotLightShadowRenderPasses));
		updateUploadBufferData(pointLightShadowInfoBuffer[dxContext.bufferedFrameID], pointLightShadowInfos, (uint32)(sizeof(point_shadow_info) * numPointLightShadowRenderPasses));
	}


	auto cameraCBV = dxContext.uploadDynamicConstantBuffer(camera);
	auto sunCBV = dxContext.uploadDynamicConstantBuffer(sun);

	common_material_info materialInfo;
	if (environment)
	{
		materialInfo.sky = environment->sky;
		materialInfo.environment = environment->environment;
		materialInfo.irradiance = environment->irradiance;
	}
	else
	{
		materialInfo.sky = blackCubeTexture;
		materialInfo.environment = blackCubeTexture;
		materialInfo.irradiance = blackCubeTexture;
	}
	materialInfo.environmentIntensity = settings.environmentIntensity;
	materialInfo.skyIntensity = settings.skyIntensity;
	materialInfo.brdf = brdfTex;
	materialInfo.lightGrid = lightCullingBuffers.lightGrid;
	materialInfo.pointLightIndexList = lightCullingBuffers.pointLightIndexList;
	materialInfo.spotLightIndexList = lightCullingBuffers.spotLightIndexList;
	materialInfo.pointLightBuffer = pointLights;
	materialInfo.spotLightBuffer = spotLights;
	materialInfo.shadowMap = shadowMap;
	materialInfo.pointLightShadowInfoBuffer = pointLightShadowInfoBuffer[dxContext.bufferedFrameID];
	materialInfo.spotLightShadowInfoBuffer = spotLightShadowInfoBuffer[dxContext.bufferedFrameID];
	materialInfo.volumetricsTexture = 0;
	materialInfo.cameraCBV = cameraCBV;
	materialInfo.sunCBV = sunCBV;

	materialInfo.depthBuffer = depthStencilBuffer;
	materialInfo.worldNormals = worldNormalsTexture;



	dx_command_list* cl = dxContext.getFreeRenderCommandList();

	if (mode == renderer_mode_rasterized)
	{
		cl->clearDepthAndStencil(depthStencilBuffer->dsvHandle);

		cl->setPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		D3D12_RESOURCE_STATES frameResultState = D3D12_RESOURCE_STATE_COMMON;

		if (aspectRatioModeChanged)
		{
			cl->transitionBarrier(frameResult, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
			cl->clearRTV(frameResult, 0.f, 0.f, 0.f);
			frameResultState = D3D12_RESOURCE_STATE_RENDER_TARGET;
		}

		barrier_batcher(cl)
			.transitionBegin(frameResult, frameResultState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);


		// ----------------------------------------
		// CLEAR OBJECT IDS
		// ----------------------------------------

		if (objectIDsTexture)
		{
			DX_PROFILE_BLOCK(cl, "Clear object IDs");

			cl->setPipelineState(*clearObjectIDsPipeline.pipeline);
			cl->setComputeRootSignature(*clearObjectIDsPipeline.rootSignature);

			struct clear_ids_cb { uint32 width, height; };
			cl->setCompute32BitConstants(0, clear_ids_cb{ objectIDsTexture->width, objectIDsTexture->height });
			cl->setDescriptorHeapUAV(1, 0, objectIDsTexture);
			cl->dispatch(bucketize(objectIDsTexture->width, 16), bucketize(objectIDsTexture->height, 16));

			barrier_batcher(cl)
				.transition(objectIDsTexture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RENDER_TARGET);
		}



		if (performedSkinning)
		{
			dxContext.renderQueue.waitForOtherQueue(dxContext.computeQueue); // Wait for GPU skinning.
		}


		// ----------------------------------------
		// DEPTH-ONLY PASS
		// ----------------------------------------

		dx_render_target depthOnlyRenderTarget({ screenVelocitiesTexture, objectIDsTexture }, depthStencilBuffer);

		cl->setRenderTarget(depthOnlyRenderTarget);
		cl->setViewport(depthOnlyRenderTarget.viewport);

		{
			DX_PROFILE_BLOCK(cl, "Depth pre-pass");

			// Static.
			if (opaqueRenderPass && opaqueRenderPass->staticDepthOnlyDrawCalls.size() > 0)
			{
				DX_PROFILE_BLOCK(cl, "Static");

				cl->setPipelineState(*depthOnlyPipeline.pipeline);
				cl->setGraphicsRootSignature(*depthOnlyPipeline.rootSignature);

				cl->setGraphicsDynamicConstantBuffer(DEPTH_ONLY_RS_CAMERA, cameraCBV);

				for (const auto& dc : opaqueRenderPass->staticDepthOnlyDrawCalls)
				{
					const mat4& m = dc.transform;
					const submesh_info& submesh = dc.submesh;

					cl->setGraphics32BitConstants(DEPTH_ONLY_RS_OBJECT_ID, (uint32)dc.objectID);
					cl->setGraphics32BitConstants(DEPTH_ONLY_RS_MVP, depth_only_transform_cb{ camera.viewProj * m, camera.prevFrameViewProj * m });

					cl->setVertexBuffer(0, dc.vertexBuffer);
					cl->setIndexBuffer(dc.indexBuffer);
					cl->drawIndexed(submesh.numTriangles * 3, 1, submesh.firstTriangle * 3, submesh.baseVertex, 0);
				}
			}

			// Dynamic.
			if (opaqueRenderPass && opaqueRenderPass->dynamicDepthOnlyDrawCalls.size() > 0)
			{
				DX_PROFILE_BLOCK(cl, "Dynamic");

				cl->setPipelineState(*depthOnlyPipeline.pipeline);
				cl->setGraphicsRootSignature(*depthOnlyPipeline.rootSignature);

				cl->setGraphicsDynamicConstantBuffer(DEPTH_ONLY_RS_CAMERA, cameraCBV);

				for (const auto& dc : opaqueRenderPass->dynamicDepthOnlyDrawCalls)
				{
					const mat4& m = dc.transform;
					const mat4& prevFrameM = dc.prevFrameTransform;
					const submesh_info& submesh = dc.submesh;

					cl->setGraphics32BitConstants(DEPTH_ONLY_RS_OBJECT_ID, (uint32)dc.objectID);
					cl->setGraphics32BitConstants(DEPTH_ONLY_RS_MVP, depth_only_transform_cb{ camera.viewProj * m, camera.prevFrameViewProj * prevFrameM });

					cl->setVertexBuffer(0, dc.vertexBuffer);
					cl->setIndexBuffer(dc.indexBuffer);
					cl->drawIndexed(submesh.numTriangles * 3, 1, submesh.firstTriangle * 3, submesh.baseVertex, 0);
				}
			}

			// Animated.
			if (opaqueRenderPass && opaqueRenderPass->animatedDepthOnlyDrawCalls.size() > 0)
			{
				DX_PROFILE_BLOCK(cl, "Animated");

				cl->setPipelineState(*animatedDepthOnlyPipeline.pipeline);
				cl->setGraphicsRootSignature(*animatedDepthOnlyPipeline.rootSignature);

				cl->setGraphicsDynamicConstantBuffer(DEPTH_ONLY_RS_CAMERA, cameraCBV);

				for (const auto& dc : opaqueRenderPass->animatedDepthOnlyDrawCalls)
				{
					const mat4& m = dc.transform;
					const mat4& prevFrameM = dc.prevFrameTransform;
					const submesh_info& submesh = dc.submesh;
					const submesh_info& prevFrameSubmesh = dc.prevFrameSubmesh;
					const ref<dx_vertex_buffer>& prevFrameVertexBuffer = dc.prevFrameVertexBuffer;

					cl->setGraphics32BitConstants(DEPTH_ONLY_RS_OBJECT_ID, (uint32)dc.objectID);
					cl->setGraphics32BitConstants(DEPTH_ONLY_RS_MVP, depth_only_transform_cb{ camera.viewProj * m, camera.prevFrameViewProj * prevFrameM });
					cl->setRootGraphicsSRV(DEPTH_ONLY_RS_PREV_FRAME_POSITIONS, prevFrameVertexBuffer->gpuVirtualAddress + prevFrameSubmesh.baseVertex * prevFrameVertexBuffer->elementSize);

					cl->setVertexBuffer(0, dc.vertexBuffer);
					cl->setIndexBuffer(dc.indexBuffer);
					cl->drawIndexed(submesh.numTriangles * 3, 1, submesh.firstTriangle * 3, submesh.baseVertex, 0);
				}
			}
		}


		// Copy hovered object id to readback buffer.
		if (objectIDsTexture)
		{
			DX_PROFILE_BLOCK(cl, "Copy hovered object ID");

			barrier_batcher(cl)
				.transition(objectIDsTexture, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);

			if (input.overWindow)
			{
				cl->copyTextureRegionToBuffer(objectIDsTexture, hoveredObjectIDReadbackBuffer[dxContext.bufferedFrameID], (uint32)input.mouse.x, (uint32)input.mouse.y, 1, 1);
			}

			barrier_batcher(cl)
				.transitionBegin(objectIDsTexture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		}


		// ----------------------------------------
		// LIGHT CULLING
		// ----------------------------------------

		if (numPointLights || numSpotLights)
		{
			DX_PROFILE_BLOCK(cl, "Cull lights");

			// Tiled frusta.
			cl->setPipelineState(*worldSpaceFrustaPipeline.pipeline);
			cl->setComputeRootSignature(*worldSpaceFrustaPipeline.rootSignature);
			cl->setComputeDynamicConstantBuffer(WORLD_SPACE_TILED_FRUSTA_RS_CAMERA, cameraCBV);
			cl->setCompute32BitConstants(WORLD_SPACE_TILED_FRUSTA_RS_CB, frusta_cb{ lightCullingBuffers.numTilesX, lightCullingBuffers.numTilesY });
			cl->setRootComputeUAV(WORLD_SPACE_TILED_FRUSTA_RS_FRUSTA_UAV, lightCullingBuffers.tiledFrusta);
			cl->dispatch(bucketize(lightCullingBuffers.numTilesX, 16), bucketize(lightCullingBuffers.numTilesY, 16));

			// Light culling.
			cl->clearUAV(lightCullingBuffers.lightIndexCounter, 0.f);
			//cl->uavBarrier(lightCullingBuffers.lightIndexCounter); // Is this necessary?
			cl->setPipelineState(*lightCullingPipeline.pipeline);
			cl->setComputeRootSignature(*lightCullingPipeline.rootSignature);
			cl->setComputeDynamicConstantBuffer(LIGHT_CULLING_RS_CAMERA, cameraCBV);
			cl->setCompute32BitConstants(LIGHT_CULLING_RS_CB, light_culling_cb{ lightCullingBuffers.numTilesX, numPointLights, numSpotLights });
			cl->setDescriptorHeapSRV(LIGHT_CULLING_RS_SRV_UAV, 0, depthStencilBuffer);
			cl->setDescriptorHeapSRV(LIGHT_CULLING_RS_SRV_UAV, 1, pointLights ? pointLights->defaultSRV : nullBufferSRV);
			cl->setDescriptorHeapSRV(LIGHT_CULLING_RS_SRV_UAV, 2, spotLights ? spotLights->defaultSRV : nullBufferSRV);
			cl->setDescriptorHeapSRV(LIGHT_CULLING_RS_SRV_UAV, 3, lightCullingBuffers.tiledFrusta);
			cl->setDescriptorHeapUAV(LIGHT_CULLING_RS_SRV_UAV, 4, lightCullingBuffers.lightIndexCounter);
			cl->setDescriptorHeapUAV(LIGHT_CULLING_RS_SRV_UAV, 5, lightCullingBuffers.pointLightIndexList);
			cl->setDescriptorHeapUAV(LIGHT_CULLING_RS_SRV_UAV, 6, lightCullingBuffers.spotLightIndexList);
			cl->setDescriptorHeapUAV(LIGHT_CULLING_RS_SRV_UAV, 7, lightCullingBuffers.lightGrid);
			cl->dispatch(lightCullingBuffers.numTilesX, lightCullingBuffers.numTilesY);
		}



		// ----------------------------------------
		// SHADOW MAP PASS
		// ----------------------------------------

		{
			DX_PROFILE_BLOCK(cl, "Shadow map pass");

			dx_render_target shadowRenderTarget({}, shadowMap);

			cl->clearDepth(shadowRenderTarget);

			cl->setPipelineState(*shadowPipeline.pipeline);
			cl->setGraphicsRootSignature(*shadowPipeline.rootSignature);

			cl->setRenderTarget(shadowRenderTarget);
			cl->clearDepth(shadowRenderTarget);

			{
				DX_PROFILE_BLOCK(cl, "Sun");

				if (sunShadowRenderPass)
				{
					for (uint32 i = 0; i < sun.numShadowCascades; ++i)
					{
						DX_PROFILE_BLOCK(cl, (i == 0) ? "First cascade" : (i == 1) ? "Second cascade" : (i == 2) ? "Third cascade" : "Fourth cascade");

						vec4 vp = sunCPUShadowViewports[i];
						cl->setViewport(vp.x, vp.y, vp.z, vp.w);

						for (uint32 cascade = 0; cascade <= i; ++cascade)
						{
							for (const auto& dc : sunShadowRenderPass->drawCalls[cascade])
							{
								const mat4& m = dc.transform;
								const submesh_info& submesh = dc.submesh;
								cl->setGraphics32BitConstants(SHADOW_RS_MVP, sun.vp[i] * m);

								cl->setVertexBuffer(0, dc.vertexBuffer);
								cl->setIndexBuffer(dc.indexBuffer);

								cl->drawIndexed(submesh.numTriangles * 3, 1, submesh.firstTriangle * 3, submesh.baseVertex, 0);
							}
						}
					}
				}
			}

			{
				DX_PROFILE_BLOCK(cl, "Spot lights");

				for (uint32 i = 0; i < numSpotLightShadowRenderPasses; ++i)
				{
					DX_PROFILE_BLOCK(cl, "Single light");

					const mat4& viewProj = spotLightShadowRenderPasses[i]->viewProjMatrix;

					cl->setViewport(spotLightViewports[i].x, spotLightViewports[i].y, spotLightViewports[i].z, spotLightViewports[i].w);

					for (const auto& dc : spotLightShadowRenderPasses[i]->drawCalls)
					{
						const mat4& m = dc.transform;
						const submesh_info& submesh = dc.submesh;
						cl->setGraphics32BitConstants(SHADOW_RS_MVP, viewProj * m);

						cl->setVertexBuffer(0, dc.vertexBuffer);
						cl->setIndexBuffer(dc.indexBuffer);

						cl->drawIndexed(submesh.numTriangles * 3, 1, submesh.firstTriangle * 3, submesh.baseVertex, 0);
					}
				}
			}

			cl->setPipelineState(*pointLightShadowPipeline.pipeline);
			cl->setGraphicsRootSignature(*pointLightShadowPipeline.rootSignature);

			{
				DX_PROFILE_BLOCK(cl, "Point lights");

				for (uint32 i = 0; i < numPointLightShadowRenderPasses; ++i)
				{
					DX_PROFILE_BLOCK(cl, "Single light");

					for (uint32 v = 0; v < 2; ++v)
					{
						DX_PROFILE_BLOCK(cl, (v == 0) ? "First hemisphere" : "Second hemisphere");
						
						cl->setViewport(pointLightViewports[i][v].x, pointLightViewports[i][v].y, pointLightViewports[i][v].z, pointLightViewports[i][v].w);

						float flip = (v == 0) ? 1.f : -1.f;

						for (const auto& dc : pointLightShadowRenderPasses[i]->drawCalls)
						{
							const mat4& m = dc.transform;
							const submesh_info& submesh = dc.submesh;
							cl->setGraphics32BitConstants(SHADOW_RS_MVP,
								point_shadow_transform_cb
								{
									m,
									pointLightShadowRenderPasses[i]->lightPosition,
									pointLightShadowRenderPasses[i]->maxDistance,
									flip
								});

							cl->setVertexBuffer(0, dc.vertexBuffer);
							cl->setIndexBuffer(dc.indexBuffer);

							cl->drawIndexed(submesh.numTriangles * 3, 1, submesh.firstTriangle * 3, submesh.baseVertex, 0);
						}
					}
				}
			}
		}

		barrier_batcher(cl)
			.transition(shadowMap, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);




		// ----------------------------------------
		// VOLUMETRICS
		// ----------------------------------------

#if 0
		cl->setPipelineState(*atmospherePipeline.pipeline);
		cl->setComputeRootSignature(*atmospherePipeline.rootSignature);
		cl->setComputeDynamicConstantBuffer(0, cameraCBV);
		cl->setComputeDynamicConstantBuffer(1, sunCBV);
		cl->setDescriptorHeapSRV(2, 0, depthBuffer);
		for (uint32 i = 0; i < MAX_NUM_SUN_SHADOW_CASCADES; ++i)
		{
			cl->setDescriptorHeapSRV(2, i + 1, sunShadowCascadeTextures[i]);
		}
		cl->setDescriptorHeapUAV(2, 5, volumetricsTexture);

		cl->dispatch(bucketize(renderWidth, 16), bucketize(renderHeight, 16));
#endif


		dx_render_target hdrRenderTarget({ hdrColorTexture, worldNormalsTexture }, depthStencilBuffer);

		cl->setRenderTarget(hdrRenderTarget);
		cl->setViewport(hdrRenderTarget.viewport);

		// ----------------------------------------
		// SKY PASS
		// ----------------------------------------

		{
			DX_PROFILE_BLOCK(cl, "Sky");

			if (environment)
			{
				cl->setPipelineState(*textureSkyPipeline.pipeline);
				cl->setGraphicsRootSignature(*textureSkyPipeline.rootSignature);

				cl->setGraphics32BitConstants(SKY_RS_VP, sky_cb{ camera.proj * createSkyViewMatrix(camera.view) });
				cl->setGraphics32BitConstants(SKY_RS_INTENSITY, sky_intensity_cb{ settings.skyIntensity });
				cl->setDescriptorHeapSRV(SKY_RS_TEX, 0, environment->sky->defaultSRV);

				cl->setVertexBuffer(0, positionOnlyMesh.vertexBuffer);
				cl->setIndexBuffer(positionOnlyMesh.indexBuffer);
				cl->drawIndexed(cubeMesh.numTriangles * 3, 1, cubeMesh.firstTriangle * 3, cubeMesh.baseVertex, 0);
			}
			else
			{
				cl->setPipelineState(*proceduralSkyPipeline.pipeline);
				cl->setGraphicsRootSignature(*proceduralSkyPipeline.rootSignature);

				cl->setGraphics32BitConstants(SKY_RS_VP, sky_cb{ camera.proj * createSkyViewMatrix(camera.view) });
				cl->setGraphics32BitConstants(SKY_RS_INTENSITY, sky_intensity_cb{ settings.skyIntensity });

				cl->setVertexBuffer(0, positionOnlyMesh.vertexBuffer);
				cl->setIndexBuffer(positionOnlyMesh.indexBuffer);
				cl->drawIndexed(cubeMesh.numTriangles * 3, 1, cubeMesh.firstTriangle * 3, cubeMesh.baseVertex, 0);
			}
		}




		// ----------------------------------------
		// LIGHT PASS
		// ----------------------------------------

		if (opaqueRenderPass && opaqueRenderPass->drawCalls.size() > 0)
		{
			DX_PROFILE_BLOCK(cl, "Main light pass");

			material_setup_function lastSetupFunc = 0;

			for (const auto& dc : opaqueRenderPass->drawCalls)
			{
				const mat4& m = dc.transform;
				const submesh_info& submesh = dc.submesh;

				if (dc.materialSetupFunc != lastSetupFunc)
				{
					dc.materialSetupFunc(cl, materialInfo);
					lastSetupFunc = dc.materialSetupFunc;
				}

				dc.material->prepareForRendering(cl);

				cl->setGraphics32BitConstants(0, transform_cb{ camera.viewProj * m, m });

				cl->setVertexBuffer(0, dc.vertexBuffer);
				cl->setIndexBuffer(dc.indexBuffer);
				cl->drawIndexed(submesh.numTriangles * 3, 1, submesh.firstTriangle * 3, submesh.baseVertex, 0);
			}
		}

		barrier_batcher(cl)
			.transitionBegin(worldNormalsTexture, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
			.transitionBegin(screenVelocitiesTexture, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);


		// ----------------------------------------
		// OUTLINES
		// ----------------------------------------

		if (opaqueRenderPass && opaqueRenderPass->outlinedObjects.size() > 0)
		{
			DX_PROFILE_BLOCK(cl, "Outlines");

			cl->setStencilReference(stencil_flag_selected_object);

			cl->setPipelineState(*outlineMarkerPipeline.pipeline);
			cl->setGraphicsRootSignature(*outlineMarkerPipeline.rootSignature);

			// Mark object in stencil.
			auto mark = [](const geometry_render_pass& rp, dx_command_list* cl, const mat4& viewProj)
			{
				for (const auto& outlined : rp.outlinedObjects)
				{
					const submesh_info& submesh = rp.drawCalls[outlined].submesh;
					const mat4& m = rp.drawCalls[outlined].transform;
					const auto& vertexBuffer = rp.drawCalls[outlined].vertexBuffer;
					const auto& indexBuffer = rp.drawCalls[outlined].indexBuffer;

					cl->setGraphics32BitConstants(OUTLINE_RS_MVP, outline_marker_cb{ viewProj * m });

					cl->setVertexBuffer(0, vertexBuffer);
					cl->setIndexBuffer(indexBuffer);
					cl->drawIndexed(submesh.numTriangles * 3, 1, submesh.firstTriangle * 3, submesh.baseVertex, 0);
				}
			};

			mark(*opaqueRenderPass, cl, camera.viewProj);
			//mark(transparentRenderPass, cl, camera.viewProj);

			// Draw outline.
			cl->transitionBarrier(depthStencilBuffer, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_DEPTH_READ);

			cl->setPipelineState(*outlineDrawerPipeline.pipeline);
			cl->setGraphicsRootSignature(*outlineDrawerPipeline.rootSignature);

			cl->setGraphics32BitConstants(OUTLINE_RS_CB, outline_drawer_cb{ (int)renderWidth, (int)renderHeight });
			cl->setDescriptorHeapResource(OUTLINE_RS_STENCIL, 0, 1, depthStencilBuffer->stencilSRV);

			cl->drawFullscreenTriangle();

			cl->transitionBarrier(depthStencilBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		}




		// ----------------------------------------
		// OVERLAYS
		// ----------------------------------------

		if (overlayRenderPass && overlayRenderPass->drawCalls.size())
		{
			DX_PROFILE_BLOCK(cl, "3D Overlays");

			cl->clearDepth(depthStencilBuffer->dsvHandle);

			material_setup_function lastSetupFunc = 0;

			for (const auto& dc : overlayRenderPass->drawCalls)
			{
				const mat4& m = dc.transform;

				if (dc.materialSetupFunc != lastSetupFunc)
				{
					dc.materialSetupFunc(cl, materialInfo);
					lastSetupFunc = dc.materialSetupFunc;
				}

				dc.material->prepareForRendering(cl);

				if (dc.setTransform)
				{
					cl->setGraphics32BitConstants(0, transform_cb{ camera.viewProj * m, m });
				}

				if (dc.drawType == geometry_render_pass::draw_type_default)
				{
					const submesh_info& submesh = dc.submesh;

					cl->setVertexBuffer(0, dc.vertexBuffer);
					cl->setIndexBuffer(dc.indexBuffer);
					cl->drawIndexed(submesh.numTriangles * 3, 1, submesh.firstTriangle * 3, submesh.baseVertex, 0);
				}
				else
				{
					cl->dispatchMesh(dc.dispatchInfo.dispatchX, dc.dispatchInfo.dispatchY, dc.dispatchInfo.dispatchZ);
				}
			}
		}




		// ----------------------------------------
		// POST PROCESSING
		// ----------------------------------------

		barrier_batcher(cl)
			.transitionBegin(shadowMap, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE)
			.transition(hdrColorTexture, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
			.transition(depthStencilBuffer, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
			.transitionEnd(worldNormalsTexture, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
			.transitionEnd(screenVelocitiesTexture, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
			.transitionEnd(frameResult, frameResultState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		ref<dx_texture> hdrResult = hdrColorTexture;

		if (settings.enableTemporalAntialiasing)
		{
			DX_PROFILE_BLOCK(cl, "Temporal anti-aliasing");

			uint32 taaOutputIndex = 1 - taaHistoryIndex;

			cl->setPipelineState(*taaPipeline.pipeline);
			cl->setComputeRootSignature(*taaPipeline.rootSignature);

			cl->setDescriptorHeapUAV(TAA_RS_TEXTURES, 0, taaTextures[taaOutputIndex]);
			cl->setDescriptorHeapSRV(TAA_RS_TEXTURES, 1, hdrColorTexture);
			cl->setDescriptorHeapSRV(TAA_RS_TEXTURES, 2, taaTextures[taaHistoryIndex]);
			cl->setDescriptorHeapSRV(TAA_RS_TEXTURES, 3, screenVelocitiesTexture);
			cl->setDescriptorHeapSRV(TAA_RS_TEXTURES, 4, depthStencilBuffer);

			cl->setCompute32BitConstants(TAA_RS_CB, taa_cb{ camera.projectionParams, vec2((float)renderWidth, (float)renderHeight) });

			cl->dispatch(bucketize(renderWidth, POST_PROCESSING_BLOCK_SIZE), bucketize(renderHeight, POST_PROCESSING_BLOCK_SIZE));

			hdrResult = taaTextures[taaOutputIndex];

			barrier_batcher(cl)
				.uav(taaTextures[taaOutputIndex])
				.transition(taaTextures[taaOutputIndex], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) // Will be read by rest of post processing stack. Can stay in read state, since it is read as history next frame.
				.transition(taaTextures[taaHistoryIndex], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS) // Will be used as UAV next frame.
				.transitionBegin(hdrColorTexture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET); // Unused for the rest of this frame. Start transition for next frame.

			taaHistoryIndex = 1 - taaHistoryIndex;
		}

		{
			DX_PROFILE_BLOCK(cl, "Tonemapping");

			cl->setPipelineState(*tonemapPipeline.pipeline);
			cl->setComputeRootSignature(*tonemapPipeline.rootSignature);

			cl->setDescriptorHeapUAV(TONEMAP_RS_TEXTURES, 0, ldrPostProcessingTexture);
			cl->setDescriptorHeapSRV(TONEMAP_RS_TEXTURES, 1, hdrResult);
			cl->setCompute32BitConstants(TONEMAP_RS_CB, settings.tonemap);

			cl->dispatch(bucketize(renderWidth, POST_PROCESSING_BLOCK_SIZE), bucketize(renderHeight, POST_PROCESSING_BLOCK_SIZE));

			barrier_batcher batcher(cl);
			batcher.uav(ldrPostProcessingTexture);
			batcher.transition(ldrPostProcessingTexture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

			if (!settings.enableTemporalAntialiasing)
			{
				batcher.transitionBegin(hdrColorTexture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			}
		}

		{
			DX_PROFILE_BLOCK(cl, "Present");

			cl->setPipelineState(*presentPipeline.pipeline);
			cl->setComputeRootSignature(*presentPipeline.rootSignature);

			cl->setDescriptorHeapUAV(PRESENT_RS_TEXTURES, 0, frameResult);
			cl->setDescriptorHeapSRV(PRESENT_RS_TEXTURES, 1, ldrPostProcessingTexture);
			cl->setCompute32BitConstants(PRESENT_RS_CB, present_cb{ PRESENT_SDR, 0.f, settings.sharpenStrength, (windowXOffset << 16) | windowYOffset });

			cl->dispatch(bucketize(renderWidth, POST_PROCESSING_BLOCK_SIZE), bucketize(renderHeight, POST_PROCESSING_BLOCK_SIZE));
		}

		barrier_batcher(cl)
			.uav(frameResult)
			.transitionEnd(shadowMap, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE)
			.transitionEnd(hdrColorTexture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET)
			.transition(worldNormalsTexture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET)
			.transition(screenVelocitiesTexture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET)
			.transition(depthStencilBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE)
			.transitionEnd(objectIDsTexture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
			.transition(ldrPostProcessingTexture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
			.transition(frameResult, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);

	}
	else
	{
		barrier_batcher(cl)
			.uav(hdrColorTexture)
			.transition(hdrColorTexture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
			.transition(frameResult, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);


		if (dxContext.raytracingSupported && raytracer)
		{
			DX_PROFILE_BLOCK(cl, "Raytracing");

			dxContext.renderQueue.waitForOtherQueue(dxContext.computeQueue); // TODO: This is not the way to go here. We should wait for the specific value returned by executeCommandList.

			raytracer->render(cl, *tlas, hdrColorTexture, materialInfo);
		}

		cl->resetToDynamicDescriptorHeap();

		barrier_batcher(cl)
			.uav(hdrColorTexture)
			.transition(hdrColorTexture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	}


	dxContext.executeCommandList(cl);

	oldSettings = settings;
}

void dx_renderer::blitResultToScreen(dx_command_list* cl, dx_rtv_descriptor_handle rtv)
{
	CD3DX12_VIEWPORT viewport = CD3DX12_VIEWPORT(0.f, 0.f, (float)windowWidth, (float)windowHeight);
	cl->setViewport(viewport);

	cl->setRenderTarget(&rtv, 1, 0);

	cl->setPipelineState(*blitPipeline.pipeline);
	cl->setGraphicsRootSignature(*blitPipeline.rootSignature);
	cl->setDescriptorHeapSRV(0, 0, frameResult);
	cl->drawFullscreenTriangle();
}
