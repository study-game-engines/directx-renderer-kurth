#include "pch.h"
#include "collision_broad.h"
#include "scene/scene.h"
#include "physics.h"
#include "core/cpu_profiling.h"


struct sap_endpoint
{
	float value;
	entt::entity entity = entt::null;
	bool start;
	uint16 colliderIndex; // Set each frame.

	sap_endpoint(entt::entity entity, bool start) : entity(entity), start(start) { }
	sap_endpoint(const sap_endpoint&) = default;
};

struct sap_context
{
	std::vector<sap_endpoint> endpoints;
};


void addColliderToBroadphase(scene_entity entity)
{
	sap_context& context = entity.registry->ctx_or_set<sap_context>();

	sap_endpoint_indirection_component endpointIndirection;

	endpointIndirection.startEndpoint = (uint16)context.endpoints.size();
	context.endpoints.emplace_back(entity.handle, true);

	endpointIndirection.endEndpoint = (uint16)context.endpoints.size();
	context.endpoints.emplace_back(entity.handle, false);

	entity.addComponent<sap_endpoint_indirection_component>(endpointIndirection);
}

static void removeEndpoint(uint16 endpointIndex, entt::registry& registry, sap_context& context)
{
	sap_endpoint last = context.endpoints.back();
	context.endpoints[endpointIndex] = last;

	// Point moved entity to correct slot.
	sap_endpoint_indirection_component& in = registry.get<sap_endpoint_indirection_component>(last.entity);
		
	if (last.start) 
	{ 
		in.startEndpoint = endpointIndex; 
	}
	else 
	{ 
		in.endEndpoint = endpointIndex; 
	}

	context.endpoints.pop_back();
}

void removeColliderFromBroadphase(scene_entity entity)
{
	sap_endpoint_indirection_component& endpointIndirection = entity.getComponent<sap_endpoint_indirection_component>();

	sap_context& context = entity.registry->ctx<sap_context>();

	removeEndpoint(endpointIndirection.startEndpoint, *entity.registry, context);
	removeEndpoint(endpointIndirection.endEndpoint, *entity.registry, context);

	if (entity.hasComponent<sap_endpoint_indirection_component>())
	{
		entity.removeComponent<sap_endpoint_indirection_component>();
	}
}

uint32 broadphase(game_scene& scene, uint32 sortingAxis, bounding_box* worldSpaceAABBs, memory_arena& arena, broadphase_collision* outCollisions)
{
	CPU_PROFILE_BLOCK("Broad phase");

	uint32 numColliders = scene.numberOfComponentsOfType<collider_component>();
	if (numColliders == 0)
	{
		return 0;
	}

	sap_context& context = scene.getContextVariable<sap_context>();
	auto& endpoints = context.endpoints;

	uint32 numEndpoints = numColliders * 2;

	assert(numEndpoints == endpoints.size());

	uint32 numCollisions = 0;

#if 0
	// Disable broadphase.

	uint16 collider0Index = 0;
	for (auto [entityHandle0, collider0] : scene.view<collider_component>().each())
	{
		uint16 collider1Index = 0;
		for (auto [entityHandle1, collider1] : scene.view<collider_component>().each())
		{
			if (entityHandle0 == entityHandle1)
			{
				break;
			}
			if (collider0.parentEntity != collider1.parentEntity)
			{
				outCollisions[numCollisions++] = { collider0Index, collider1Index };
			}

			++collider1Index;
		}
		++collider0Index;
	}
	return numCollisions;

#endif


	{
		CPU_PROFILE_BLOCK("Update endpoints");

		// Index of each collider in the scene. 
		// We iterate over the endpoint indirections, which are sorted the exact same way as the colliders.
		uint16 index = 0;

		for (auto [entityHandle, indirection] : scene.view<sap_endpoint_indirection_component>().each())
		{
			bounding_box& aabb = worldSpaceAABBs[index];

			uint16 start = indirection.startEndpoint;
			uint16 end = indirection.endEndpoint;

			float lo = aabb.minCorner.data[sortingAxis];
			float hi = aabb.maxCorner.data[sortingAxis];
			endpoints[start].value = lo;
			endpoints[end].value = hi;

			endpoints[start].colliderIndex = index;
			endpoints[end].colliderIndex = index;

			assert(endpoints[start].entity == entityHandle);
			assert(endpoints[end].entity == entityHandle);

			++index;
		}
	}

	{
		CPU_PROFILE_BLOCK("Sort endpoints");

#if 1
		// Insertion sort.
		for (uint32 i = 1; i < numEndpoints; ++i)
		{
			sap_endpoint key = endpoints[i];
			uint32 j = i - 1;

			while (j != UINT32_MAX && endpoints[j].value > key.value)
			{
				endpoints[j + 1] = endpoints[j];
				j = j - 1;
			}
			endpoints[j + 1] = key;
		}
#else
		std::sort(endpoints, endpoints + numEndpoints, [](sap_endpoint a, sap_endpoint b) { return a.value < b.value; });
#endif
	}


	memory_marker marker = arena.getMarker();

	{
		CPU_PROFILE_BLOCK("Determine overlaps");

		uint32 numActive = 0;
		uint16* activeList = arena.allocate<uint16>(numColliders); // Conservative estimate.
		uint16* positionInActiveList = arena.allocate<uint16>(numColliders);

		for (uint32 i = 0; i < numEndpoints; ++i)
		{
			sap_endpoint ep = endpoints[i];
			if (ep.start)
			{
				for (uint32 active = 0; active < numActive; ++active)
				{
					bounding_box& a = worldSpaceAABBs[ep.colliderIndex];
					bounding_box& b = worldSpaceAABBs[activeList[active]];

					if (aabbVsAABB(a, b))
					{
						outCollisions[numCollisions++] = { ep.colliderIndex, activeList[active] };
					}
				}

				assert(ep.colliderIndex < numColliders);
				positionInActiveList[ep.colliderIndex] = numActive;
				activeList[numActive++] = ep.colliderIndex;
			}
			else
			{
#if 0
				for (uint32 a = 0; a < numActive; ++a)
				{
					if (ep.colliderIndex == activeList[a])
					{
						activeList[a] = activeList[numActive - 1];
						--numActive;
						break;
					}
				}
#else
				uint16 pos = positionInActiveList[ep.colliderIndex];

				uint16 lastColliderInActiveList = activeList[numActive - 1];
				positionInActiveList[lastColliderInActiveList] = pos;

				activeList[pos] = activeList[numActive - 1];
				--numActive;
#endif
			}
		}

		assert(numActive == 0);
	}

	arena.resetToMarker(marker);



	// Fix up indirections.
	{
		CPU_PROFILE_BLOCK("Fix up indirections");

		for (uint32 i = 0; i < numEndpoints; ++i)
		{
			sap_endpoint ep = endpoints[i];
			scene_entity entity = { ep.entity, scene };
			sap_endpoint_indirection_component& in = entity.getComponent<sap_endpoint_indirection_component>();

			if (ep.start)
			{
				in.startEndpoint = i;
			}
			else
			{
				in.endEndpoint = i;
			}
		}
	}


	return numCollisions;
}
