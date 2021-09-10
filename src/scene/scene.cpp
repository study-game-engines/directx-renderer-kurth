#include "pch.h"
#include "scene.h"
#include "physics/physics.h"
#include "physics/collision_broad.h"

scene::scene()
{
	// Construct groups early. Ingore the return types.
	(void) registry.group<collider_component, sap_endpoint_indirection_component>(); // Colliders and SAP endpoints are always sorted in the same order.
	(void) registry.group<trs, dynamic_geometry_component, rigid_body_component>();
	(void) registry.group<trs, rigid_body_component>();

	registry.on_construct<collider_component>().connect<&onColliderAdded>();
	registry.on_destroy<collider_component>().connect<&onColliderRemoved>();
}

void scene::clearAll()
{
	registry.clear();
}

void scene::deleteEntity(scene_entity e)
{
	if (e.hasComponent<physics_reference_component>())
	{
		auto colliderView = view<collider_component>();

		physics_reference_component& reference = e.getComponent<physics_reference_component>();

		scene_entity colliderEntity = { reference.firstColliderEntity, &registry };
		while (colliderEntity)
		{
			collider_component& collider = colliderEntity.getComponent<collider_component>();
			entt::entity next = collider.nextEntity;
		
			registry.destroy(colliderEntity.handle);
		
			colliderEntity = { next, &registry };
		}
	}

	registry.destroy(e.handle);
}
