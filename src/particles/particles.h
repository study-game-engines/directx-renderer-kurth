#pragma once

#include "core/math.h"
#include "rendering/render_pass.h"
#include "dx/dx_command_list.h"

#include "particles_rs.hlsli"

struct dx_command_list;

enum sort_mode
{
	sort_mode_none,
	sort_mode_front_to_back,
	sort_mode_back_to_front,
};

struct particle_system
{
	float emitRate;

protected:

	struct particle_parameter_setter
	{
		virtual void setRootParameters(dx_command_list* cl) = 0;
	};


	void initializeAsBillboard(uint32 particleStructSize, uint32 maxNumParticles, float emitRate, sort_mode sortMode = sort_mode_none);
	void initializeAsMesh(uint32 particleStructSize, dx_mesh mesh, submesh_info submesh, uint32 maxNumParticles, float emitRate, sort_mode sortMode = sort_mode_none);

	void update(float dt, const dx_pipeline& emitPipeline, const dx_pipeline& simulatePipeline, particle_parameter_setter* parameterSetter);
	void emit(dx_command_list* cl, float count, float dt, const struct dx_pipeline& emitPipeline, particle_parameter_setter* parameterSetter);
	void simulate(dx_command_list* cl, float dt, const struct dx_pipeline& simulatePipeline, particle_parameter_setter* parameterSetter);

	particle_draw_info getDrawInfo(const struct dx_pipeline& renderPipeline);

	static dx_mesh billboardMesh;
	submesh_info submesh;

	uint32 numBursts = 0;

private:
	static void initializePipeline();

	void initializeInternal(uint32 particleStructSize, uint32 maxNumParticles, float emitRate, submesh_info submesh, sort_mode sortMode);

	void setStartResources(dx_command_list* cl, const struct particle_start_cb& cb, uint32 offset, particle_parameter_setter* parameterSetter);
	void setSimResources(dx_command_list* cl, const struct particle_sim_cb& cb, uint32 offset, particle_parameter_setter* parameterSetter);
	void setResources(dx_command_list* cl, uint32 offset, particle_parameter_setter* parameterSetter);

	uint32 getAliveListOffset(uint32 alive);
	uint32 getDeadListOffset();
	uint32 getNumUserRootParameters(const struct dx_pipeline& pipeline);

	uint32 maxNumParticles;
	uint32 currentAlive = 0;

	ref<dx_buffer> particlesBuffer;
	ref<dx_buffer> listBuffer; // Counters, dead, alive 0, alive 1.
	ref<dx_buffer> dispatchBuffer;
	ref<dx_buffer> sortBuffer;

	sort_mode sortMode;

	uint32 index;



	static ref<dx_buffer> particleDrawCommandBuffer;
	static dx_command_signature particleCommandSignature;

	friend void initializeRenderUtils();
	template <typename T> friend struct particle_render_pipeline;
};

template <typename render_data_t>
struct particle_render_pipeline
{
	static void render(dx_command_list* cl, const mat4& viewProj, const particle_render_command<render_data_t>& rc);
};

template<typename render_data_t>
inline void particle_render_pipeline<render_data_t>::render(dx_command_list* cl, const mat4& viewProj, const particle_render_command<render_data_t>& rc)
{
	const particle_draw_info& info = rc.drawInfo;

	cl->setRootGraphicsSRV(info.rootParameterOffset + PARTICLE_RENDERING_RS_PARTICLES, info.particleBuffer->gpuVirtualAddress);
	cl->setRootGraphicsSRV(info.rootParameterOffset + PARTICLE_RENDERING_RS_ALIVE_LIST, info.aliveList->gpuVirtualAddress + info.aliveListOffset);

	cl->setVertexBuffer(0, rc.vertexBuffer.positions);
	if (rc.vertexBuffer.others)
	{
		cl->setVertexBuffer(1, rc.vertexBuffer.others);
	}
	cl->setIndexBuffer(rc.indexBuffer);

	cl->drawIndirect(particle_system::particleCommandSignature, 1, info.commandBuffer, info.commandBufferOffset);
}

#define BUILD_PARTICLE_SHADER_NAME(name, suffix) name##suffix

#define EMIT_PIPELINE_NAME(name) BUILD_PARTICLE_SHADER_NAME(name, "_emit_cs")
#define SIMULATE_PIPELINE_NAME(name) BUILD_PARTICLE_SHADER_NAME(name, "_sim_cs")
#define VERTEX_SHADER_NAME(name) BUILD_PARTICLE_SHADER_NAME(name, "_vs")
#define PIXEL_SHADER_NAME(name) BUILD_PARTICLE_SHADER_NAME(name, "_ps")


struct particle_system_component
{
	ref<particle_system> system;
};

