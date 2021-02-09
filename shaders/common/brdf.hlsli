#ifndef BRDF_HLSLI
#define BRDF_HLSLI

#include "math.hlsli"
#include "random.hlsli"
#include "light_source.hlsli"


struct surface_info
{
	// Set from outside.
	float3 P;
	float3 N;
	float3 V;

	float4 albedo;
	float roughness;
	float metallic;
	float3 emission;



	// Inferred from properties above.
	float alphaRoughness;
	float alphaRoughnessSquared;
	float NdotV;
	float3 F0;


	inline void inferRemainingProperties()
	{
		alphaRoughness = roughness * roughness;
		alphaRoughnessSquared = alphaRoughness * alphaRoughness;
		NdotV = saturate(dot(N, V));
		F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo.xyz, metallic);
	}
};

struct light_info
{
	float3 L;
	float3 H;
	float NdotL;
	float NdotH;
	float LdotH;
	float VdotH;

	float3 radiance;

	inline void initialize(surface_info surface, float3 L_, float3 rad)
	{
		L = L_;
		H = normalize(L + surface.V);

		NdotL = saturate(dot(surface.N, L));
		NdotH = saturate(dot(surface.N, H));
		LdotH = saturate(dot(L, H));
		VdotH = saturate(dot(surface.V, H));

		radiance = rad;
	}

	inline void initializeFromPointLight(surface_info surface, point_light_cb pl)
	{
		float3 L = pl.position - surface.P;
		float distanceToLight = length(L);
		L /= distanceToLight;

		initialize(surface, L, pl.radiance * getAttenuation(distanceToLight, pl.radius) * LIGHT_RADIANCE_SCALE);
	}

	inline void initializeFromSpotLight(surface_info surface, spot_light_cb sl)
	{
		float3 L = (sl.position - surface.P);
		float distanceToLight = length(L);
		L /= distanceToLight;

		float innerCutoff = getInnerCutoff(sl.innerAndOuterCutoff);
		float outerCutoff = getOuterCutoff(sl.innerAndOuterCutoff);
		float epsilon = innerCutoff - outerCutoff;

		float theta = dot(-L, sl.direction);
		float attenuation = getAttenuation(distanceToLight, sl.maxDistance);
		float intensity = saturate((theta - outerCutoff) / epsilon) * attenuation;

		initialize(surface, L, sl.radiance * intensity * LIGHT_RADIANCE_SCALE);
	}
};



// http://graphicrants.blogspot.com/2013/08/specular-brdf-reference.html





// ----------------------------------------
// FRESNEL (Surface becomes more reflective when seen from a grazing angle).
// ----------------------------------------

static float3 fresnelSchlick(float LdotH, float3 F0)
{
	return F0 + (float3(1.f, 1.f, 1.f) - F0) * pow(1.f - LdotH, 5.f);
}

static float3 fresnelSchlickRoughness(float LdotH, float3 F0, float roughness)
{
	float v = 1.f - roughness;
	return F0 + (max(float3(v, v, v), F0) - F0) * pow(1.f - LdotH, 5.f);
}




// ----------------------------------------
// DISTRIBUTION (Microfacets' orientation based on roughness).
// ----------------------------------------

static float distributionGGX(float NdotH, float roughness)
{
	float a = roughness * roughness;
	float a2 = a * a;
	float NdotH2 = NdotH * NdotH;

	float d = (NdotH2 * (a2 - 1.f) + 1.f);
	return a2 / max(d * d * pi, 0.001f);
}

static float distributionGGX(surface_info surface, light_info light)
{
	float NdotH = light.NdotH;
	float NdotH2 = NdotH * NdotH;
	float a2 = surface.alphaRoughnessSquared;
	float d = (NdotH2 * (a2 - 1.f) + 1.f);
	return a2 / max(d * d * pi, 0.001f);
}




// ----------------------------------------
// GEOMETRIC MASKING (Microfacets may shadow each-other).
// ----------------------------------------

static float geometrySmith(float NdotL, float NdotV, float roughness)
{
	float k = (roughness * roughness) * 0.5f;

	float ggx2 = NdotV / (NdotV * (1.f - k) + k);
	float ggx1 = NdotL / (NdotL * (1.f - k) + k);

	return ggx1 * ggx2;
}

static float geometrySmith(surface_info surface, light_info light)
{
	float k = surface.alphaRoughness * 0.5f;

	float ggx2 = surface.NdotV / (surface.NdotV * (1.f - k) + k);
	float ggx1 = light.NdotL / (light.NdotL * (1.f - k) + k);

	return ggx1 * ggx2;
}






// ----------------------------------------
// IMPORTANCE SAMPLING
// ----------------------------------------

// When using this function to sample, the probability density is:
//      pdf = D * NdotH / (4 * HdotV)
float3 importanceSampleGGX(inout uint randSeed, float3 N, float roughness)
{
	// Get our uniform random numbers.
	float2 randVal = float2(nextRand(randSeed), nextRand(randSeed));

	// Get an orthonormal basis from the normal.
	float3 B = getPerpendicularVector(N);
	float3 T = cross(B, N);

	// GGX NDF sampling.
	float a2 = roughness * roughness;
	float cosThetaH = sqrt(max(0.f, (1.f - randVal.x) / ((a2 - 1.f) * randVal.x + 1.f)));
	float sinThetaH = sqrt(max(0.f, 1.f - cosThetaH * cosThetaH));
	float phiH = randVal.y * pi * 2.f;

	// Get our GGX NDF sample (i.e., the half vector).
	return T * (sinThetaH * cos(phiH)) +
		B * (sinThetaH * sin(phiH)) +
		N * cosThetaH;
}

// Call this with a hammersley distribution as Xi.
static float3 importanceSampleGGX(float2 Xi, float3 N, float roughness)
{
	float a = roughness * roughness;

	float phi = 2.f * pi * Xi.x;
	float cosTheta = sqrt((1.f - Xi.y) / (1.f + (a * a - 1.f) * Xi.y));
	float sinTheta = sqrt(1.f - cosTheta * cosTheta);

	// From spherical coordinates to cartesian coordinates.
	float3 H;
	H.x = cos(phi) * sinTheta;
	H.y = sin(phi) * sinTheta;
	H.z = cosTheta;

	// From tangent-space vector to world-space sample vector.
	float3 up = abs(N.z) < 0.999 ? float3(0.f, 0.f, 1.f) : float3(1.f, 0.f, 0.f);
	float3 tangent = normalize(cross(up, N));
	float3 bitangent = cross(N, tangent);

	float3 sampleVec = tangent * H.x + bitangent * H.y + N * H.z;
	return normalize(sampleVec);
}




// ----------------------------------------
// LIGHTING COMPUTATION.
// ----------------------------------------

static float3 calculateAmbientLighting(float3 albedo, float3 irradiance,
	TextureCube<float4> environmentTexture, Texture2D<float4> brdf, SamplerState clampSampler,
	float3 N, float3 V, float3 F0, float roughness, float metallic)
{
	// Common.
	float NdotV = saturate(dot(N, V));
	float3 F = fresnelSchlickRoughness(NdotV, F0, roughness);
	float3 kS = F;
	float3 kD = float3(1.f, 1.f, 1.f) - kS;
	kD *= 1.f - metallic;

	// Diffuse.
	float3 diffuse = irradiance * albedo;

	// Specular.
	float3 R = reflect(-V, N);
	uint width, height, numMipLevels;
	environmentTexture.GetDimensions(0, width, height, numMipLevels);
	float lod = roughness * float(numMipLevels - 1);

	float3 prefilteredColor = environmentTexture.SampleLevel(clampSampler, R, lod).rgb;
	float2 envBRDF = brdf.SampleLevel(clampSampler, float2(roughness, NdotV), 0).rg;
	float3 specular = prefilteredColor * (F * envBRDF.x + envBRDF.y);

	float3 ambient = kD * diffuse + specular;

	return ambient;
}

static float3 calculateAmbientLighting(float3 albedo,
	TextureCube<float4> irradianceTexture, TextureCube<float4> environmentTexture, Texture2D<float4> brdf, SamplerState clampSampler,
	float3 N, float3 V, float3 F0, float roughness, float metallic)
{
	float3 irradiance = irradianceTexture.SampleLevel(clampSampler, N, 0).rgb;
	return calculateAmbientLighting(albedo, irradiance, environmentTexture, brdf, clampSampler, N, V, F0, roughness, metallic);
}

static float3 calculateDirectLighting(float3 albedo, float3 radiance, float3 N, float3 L, float3 V, float3 F0, float roughness, float metallic)
{
#if 0
	float3 H = normalize(V + L);
	float NdotV = saturate(dot(N, V));
	float NdotH = saturate(dot(N, H));
	float NdotL = saturate(dot(N, L));
	float HdotV = saturate(dot(H, V));

	// Cook-Torrance BRDF.
	float NDF = distributionGGX(NdotH, roughness);
	float G = geometrySmith(NdotL, NdotV, roughness);
	float3 F = fresnelSchlick(HdotV, F0);

	float3 kS = F;
	float3 kD = float3(1.f, 1.f, 1.f) - kS;
	kD *= 1.f - metallic;

	float3 numerator = NDF * G * F;
	float denominator = 4.f * NdotV * NdotL;
	float3 specular = numerator / max(denominator, 0.001f);

	return (kD * albedo * invPI + specular) * radiance * NdotL;
#endif

	return 0.xxx;
}

static float3 calculateDirectLighting(surface_info surface, light_info light)
{
	float D = distributionGGX(surface, light);
	float G = geometrySmith(surface, light);
	float3 F = fresnelSchlick(light.VdotH, surface.F0);

	float3 kD = float3(1.f, 1.f, 1.f) - F;
	kD *= 1.f - surface.metallic;
	float3 diffuse = kD * surface.albedo.rgb * invPI;

	float3 numerator = D * G * F;
	float denominator = 4.f * surface.NdotV * light.NdotL;
	float3 specular = numerator / max(denominator, 0.001f);

	return (diffuse + specular) * light.radiance * light.NdotL;
}

#endif
