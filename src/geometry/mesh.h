#pragma once

#include "asset/asset.h"
#include "physics/bounding_volumes.h"
#include "dx/dx_buffer.h"
#include "animation/animation.h"
#include "mesh_builder.h"

struct pbr_material;


struct submesh
{
	submesh_info info;
	bounding_box aabb; // In multi's local space.
	trs transform;

	ref<pbr_material> material;
	std::string name;
};

struct multi_mesh
{
	std::vector<submesh> submeshes;
	animation_skeleton skeleton;
	dx_mesh mesh;
	bounding_box aabb = { vec3(0.f), vec3(0.f) };

	asset_handle handle;
	uint32 flags;

	std::atomic<asset_load_state> loadState = asset_loaded;
};


using mesh_load_callback = std::function<void(mesh_builder& builder, std::vector<submesh>& submeshes, const bounding_box& boundingBox)>;

ref<multi_mesh> loadMeshFromFile(const fs::path& filename, uint32 flags = mesh_creation_flags_default, mesh_load_callback cb = nullptr);
ref<multi_mesh> loadMeshFromHandle(asset_handle handle, uint32 flags = mesh_creation_flags_default, mesh_load_callback cb = nullptr);

// Same function but with different default flags (includes skin).
inline ref<multi_mesh> loadAnimatedMeshFromFile(const fs::path& filename, uint32 flags = mesh_creation_flags_animated, mesh_load_callback cb = nullptr)
{
	return loadMeshFromFile(filename, flags, cb);
}

struct mesh_component
{
	ref<multi_mesh> mesh;
};
