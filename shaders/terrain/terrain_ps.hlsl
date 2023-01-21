#include "terrain_rs.hlsli"
#include "normal.hlsli"
#include "random.hlsli"
#include "camera.hlsli"
#include "brdf.hlsli"
#include "lighting.hlsli"

Texture2D<float2> normals					: register(t1);
SamplerState clampSampler					: register(s0);
SamplerState wrapSampler					: register(s1);

ConstantBuffer<terrain_cb> terrain			: register(b1);

ConstantBuffer<camera_cb> camera			: register(b1, space1);
ConstantBuffer<lighting_cb> lighting		: register(b2, space1);

Texture2D<float4> groundAlbedoTexture		: register(t0, space1);
Texture2D<float3> groundNormalTexture		: register(t1, space1);
Texture2D<float1> groundRoughnessTexture	: register(t2, space1);
Texture2D<float4> rockAlbedoTexture			: register(t3, space1);
Texture2D<float3> rockNormalTexture			: register(t4, space1);
Texture2D<float1> rockRoughnessTexture		: register(t5, space1);


TextureCube<float4> irradianceTexture					: register(t0, space2);
TextureCube<float4> prefilteredRadianceTexture			: register(t1, space2);

Texture2D<float2> brdf									: register(t2, space2);


struct ps_input
{
	float2 uv				: TEXCOORDS;
	float3 worldPosition	: POSITION;
};

struct ps_output
{
	float4 hdrColor				: SV_Target0;
	float4 worldNormalRoughness	: SV_Target1;
};

struct triplanar_coefficients
{
	float2 uvX, uvY, uvZ;
	float wX, wY, wZ;
};

static triplanar_coefficients triplanar(float3 position, float3 normal, float textureScale, float sharpness)
{
	triplanar_coefficients result;

	float3 scaledPosition = position * textureScale;
	result.uvX = scaledPosition.zy;
	result.uvY = scaledPosition.xz;
	result.uvZ = scaledPosition.xy;

	float3 blendWeights = pow(abs(normal), sharpness);
	blendWeights /= (blendWeights.x + blendWeights.y + blendWeights.z);

	result.wX = blendWeights.x;
	result.wY = blendWeights.y;
	result.wZ = blendWeights.z;

	return result;
}

[RootSignature(TERRAIN_RS)]
ps_output main(ps_input IN)
{
	float2 n = normals.Sample(clampSampler, IN.uv) * terrain.amplitudeScale;
	float3 N = normalize(float3(n.x, 1.f, n.y));
	
	float texScale = 0.1f;

	triplanar_coefficients tri = triplanar(IN.worldPosition, N, texScale, 15.f);
	
	float4 albedo =
		rockAlbedoTexture.Sample(wrapSampler, tri.uvX) * tri.wX +
		groundAlbedoTexture.Sample(wrapSampler, tri.uvY) * tri.wY +
		rockAlbedoTexture.Sample(wrapSampler, tri.uvZ) * tri.wZ;

	float roughness =
		rockRoughnessTexture.Sample(wrapSampler, tri.uvX) * tri.wX +
		groundRoughnessTexture.Sample(wrapSampler, tri.uvY) * tri.wY +
		rockRoughnessTexture.Sample(wrapSampler, tri.uvZ) * tri.wZ;


	float3 tnormalX = sampleNormalMap(rockNormalTexture, wrapSampler, tri.uvX);
	float3 tnormalY = sampleNormalMap(groundNormalTexture, wrapSampler, tri.uvY);
	float3 tnormalZ = sampleNormalMap(rockNormalTexture, wrapSampler, tri.uvZ);

	tnormalX = float3(
		tnormalX.xy + N.zy,
		abs(tnormalX.z) * N.x
		);
	tnormalY = float3(
		tnormalY.xy + N.xz,
		abs(tnormalY.z) * N.y
		);
	tnormalZ = float3(
		tnormalZ.xy + N.xy,
		abs(tnormalZ.z) * N.z
		);

	float3 normalmapN = normalize(
		tnormalX.zyx * tri.wX +
		tnormalY.xzy * tri.wY +
		tnormalZ.xyz * tri.wZ
	);

	N = normalmapN;



	surface_info surface;

	surface.albedo = albedo;
	surface.N = N;
	surface.roughness = roughness;
	surface.roughness = clamp(surface.roughness, 0.01f, 0.99f);
	surface.metallic = 0.f;
	surface.emission = 0.f;

	surface.P = IN.worldPosition;
	float3 camToP = surface.P - camera.position.xyz;
	surface.V = -normalize(camToP);

	surface.inferRemainingProperties();






	light_contribution totalLighting = { float3(0.f, 0.f, 0.f), float3(0.f, 0.f, 0.f) };


	// Sun.
	{
		float3 L = -lighting.sun.direction;

		light_info light;
		light.initialize(surface, L, lighting.sun.radiance);

		float visibility = 1.f;

		[branch]
		if (visibility > 0.f)
		{
			totalLighting.add(calculateDirectLighting(surface, light), visibility);
		}
	}


	totalLighting.add(calculateAmbientIBL(surface, irradianceTexture, prefilteredRadianceTexture, brdf, clampSampler), 1.f);


	//float value = step(0.9f, N.y);
	//col = value.xxx;


	ps_output OUT;
	OUT.hdrColor = totalLighting.evaluate(surface.albedo);
	OUT.worldNormalRoughness = float4(packNormal(N), surface.roughness, 0.f);

	//OUT.hdrColor.rgb = roughness.xxx;

	OUT.hdrColor.rgb *= 0.3f;

	return OUT;
}
