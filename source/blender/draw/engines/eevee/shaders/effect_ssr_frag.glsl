
/* Based on Stochastic Screen Space Reflections
 * https://www.ea.com/frostbite/news/stochastic-screen-space-reflections */

#ifndef UTIL_TEX
#define UTIL_TEX
uniform sampler2DArray utilTex;
#endif /* UTIL_TEX */

uniform int rayCount;

#define BRDF_BIAS 0.7

vec3 generate_ray(vec3 V, vec3 N, float a2, vec3 rand, out float pdf)
{
	float NH;
	vec3 T, B;
	make_orthonormal_basis(N, T, B); /* Generate tangent space */

	/* Importance sampling bias */
	rand.x = mix(rand.x, 0.0, BRDF_BIAS);

	/* TODO distribution of visible normals */
	vec3 H = sample_ggx(rand, a2, N, T, B, NH); /* Microfacet normal */
	pdf = min(1024e32, pdf_ggx_reflect(NH, a2)); /* Theoretical limit of 16bit float */
	return reflect(-V, H);
}

#define MAX_MIP 9.0

#ifdef STEP_RAYTRACE

uniform sampler2D depthBuffer;
uniform sampler2D normalBuffer;
uniform sampler2D specroughBuffer;

uniform mat4 ViewProjectionMatrix;

layout(location = 0) out vec4 hitData0;
layout(location = 1) out vec4 hitData1;
layout(location = 2) out vec4 hitData2;
layout(location = 3) out vec4 hitData3;

bool has_hit_backface(vec3 hit_pos, vec3 R, vec3 V)
{
	vec2 hit_co = project_point(ProjectionMatrix, hit_pos).xy * 0.5 + 0.5;
	vec3 hit_N = normal_decode(textureLod(normalBuffer, hit_co, 0.0).rg, V);
	return (dot(-R, hit_N) < 0.0);
}

vec4 do_ssr(sampler2D depthBuffer, vec3 V, vec3 N, vec3 viewPosition, float a2, vec3 rand)
{
	float pdf;
	vec3 R = generate_ray(V, N, a2, rand, pdf);

	float hit_dist = raycast(depthBuffer, viewPosition, R, rand.x);
	vec3 hit_pos = viewPosition + R * abs(hit_dist);

	if (has_hit_backface(hit_pos, R, V) || (hit_dist <= 0.0)) {
		hit_pos.z *= -1.0;
	}

	return vec4(hit_pos, pdf);
}

void main()
{
#ifdef FULLRES
	ivec2 fullres_texel = ivec2(gl_FragCoord.xy);
	ivec2 halfres_texel = fullres_texel;
#else
	ivec2 fullres_texel = ivec2(gl_FragCoord.xy) * 2;
	ivec2 halfres_texel = ivec2(gl_FragCoord.xy);
#endif

	float depth = texelFetch(depthBuffer, fullres_texel, 0).r;

	/* Early out */
	if (depth == 1.0)
		discard;

	vec2 uvs = gl_FragCoord.xy / vec2(textureSize(depthBuffer, 0));
#ifndef FULLRES
	uvs *= 2.0;
#endif

	/* Using view space */
	vec3 viewPosition = get_view_space_from_depth(uvs, depth);
	vec3 V = viewCameraVec;
	vec3 N = normal_decode(texelFetch(normalBuffer, fullres_texel, 0).rg, V);

	/* Retrieve pixel data */
	vec4 speccol_roughness = texelFetch(specroughBuffer, fullres_texel, 0).rgba;

	/* Early out */
	if (dot(speccol_roughness.rgb, vec3(1.0)) == 0.0)
		discard;

	float roughness = speccol_roughness.a;
	float roughnessSquared = max(1e-3, roughness * roughness);
	float a2 = roughnessSquared * roughnessSquared;

	vec3 rand = texelFetch(utilTex, ivec3(halfres_texel % LUT_SIZE, 2), 0).rba;

	/* TODO : Raytrace together if textureGather is supported. */
	hitData0 = do_ssr(depthBuffer, V, N, viewPosition, a2, rand);
	if (rayCount > 1) hitData1 = do_ssr(depthBuffer, V, N, viewPosition, a2, rand.xyz * vec3(1.0, -1.0, -1.0));
	if (rayCount > 2) hitData2 = do_ssr(depthBuffer, V, N, viewPosition, a2, rand.xzy * vec3(1.0,  1.0, -1.0));
	if (rayCount > 3) hitData3 = do_ssr(depthBuffer, V, N, viewPosition, a2, rand.xzy * vec3(1.0, -1.0,  1.0));
}

#else /* STEP_RESOLVE */

uniform sampler2D colorBuffer; /* previous frame */
uniform sampler2D depthBuffer;
uniform sampler2D normalBuffer;
uniform sampler2D specroughBuffer;

uniform sampler2D hitBuffer0;
uniform sampler2D hitBuffer1;
uniform sampler2D hitBuffer2;
uniform sampler2D hitBuffer3;

uniform int probe_count;

uniform float borderFadeFactor;
uniform float fireflyFactor;

uniform mat4 ViewProjectionMatrix;
uniform mat4 PastViewProjectionMatrix;

out vec4 fragColor;

void fallback_cubemap(vec3 N, vec3 V, vec3 W, float roughness, float roughnessSquared, inout vec4 spec_accum)
{
	/* Specular probes */
	vec3 spec_dir = get_specular_dominant_dir(N, V, roughnessSquared);

	/* Starts at 1 because 0 is world probe */
	for (int i = 1; i < MAX_PROBE && i < probe_count && spec_accum.a < 0.999; ++i) {
		CubeData cd = probes_data[i];

		float fade = probe_attenuation_cube(cd, W);

		if (fade > 0.0) {
			vec3 spec = probe_evaluate_cube(float(i), cd, W, spec_dir, roughness);
			accumulate_light(spec, fade, spec_accum);
		}
	}

	/* World Specular */
	if (spec_accum.a < 0.999) {
		vec3 spec = probe_evaluate_world_spec(spec_dir, roughness);
		accumulate_light(spec, 1.0, spec_accum);
	}
}

#if 0 /* Finish reprojection with motion vectors */
vec3 get_motion_vector(vec3 pos)
{
}

/* http://bitsquid.blogspot.fr/2017/06/reprojecting-reflections_22.html */
vec3 find_reflection_incident_point(vec3 cam, vec3 hit, vec3 pos, vec3 N)
{
	float d_cam = point_plane_projection_dist(cam, pos, N);
	float d_hit = point_plane_projection_dist(hit, pos, N);

	if (d_hit < d_cam) {
		/* Swap */
		float tmp = d_cam;
		d_cam = d_hit;
		d_hit = tmp;
	}

	vec3 proj_cam = cam - (N * d_cam);
	vec3 proj_hit = hit - (N * d_hit);

	return (proj_hit - proj_cam) * d_cam / (d_cam + d_hit) + proj_cam;
}
#endif

float brightness(vec3 c)
{
	return max(max(c.r, c.g), c.b);
}

float screen_border_mask(vec2 past_hit_co, vec3 hit)
{
	/* Fade on current and past screen edges */
	vec4 hit_co = ViewProjectionMatrix * vec4(hit, 1.0);
	hit_co.xy = (hit_co.xy / hit_co.w) * 0.5 + 0.5;
	hit_co.zw = past_hit_co;

	const float margin = 0.003;
	float atten = borderFadeFactor + margin; /* Screen percentage */
	hit_co = smoothstep(margin, atten, hit_co) * (1 - smoothstep(1.0 - atten, 1.0 - margin, hit_co));
	vec2 atten_fac = min(hit_co.xy, hit_co.zw);

	float screenfade = atten_fac.x * atten_fac.y;

	return screenfade;
}

float view_facing_mask(vec3 V, vec3 R)
{
	/* Fade on viewing angle (strange deformations happens at R == V) */
	return smoothstep(0.95, 0.80, dot(V, R));
}

vec2 get_reprojected_reflection(vec3 hit, vec3 pos, vec3 N)
{
	/* TODO real reprojection with motion vectors, etc... */
	return project_point(PastViewProjectionMatrix, hit).xy * 0.5 + 0.5;
}

vec4 get_ssr_sample(
        sampler2D hitBuffer, vec3 worldPosition, vec3 N, vec3 V, float roughnessSquared,
        float cone_tan, vec2 source_uvs, vec2 texture_size, ivec2 target_texel,
        inout float weight_acc)
{
	vec4 hit_co_pdf = texelFetch(hitBuffer, target_texel, 0).rgba;
	bool has_hit = (hit_co_pdf.z < 0.0);
	hit_co_pdf.z = -abs(hit_co_pdf.z);

	/* Hit position in world space. */
	vec3 hit_pos = (ViewMatrixInverse * vec4(hit_co_pdf.xyz, 1.0)).xyz;

	/* Find hit position in previous frame. */
	vec2 ref_uvs = get_reprojected_reflection(hit_pos, worldPosition, N);

	/* Estimate a cone footprint to sample a corresponding mipmap level. */
	/* Compute cone footprint Using UV distance because we are using screen space filtering. */
	float cone_footprint = 1.5 * cone_tan * distance(ref_uvs, source_uvs);
	float mip = BRDF_BIAS * clamp(log2(cone_footprint * max(texture_size.x, texture_size.y)), 0.0, MAX_MIP);

	/* Slide 54 */
	vec3 L = normalize(hit_pos - worldPosition);
	float bsdf = bsdf_ggx(N, L, V, roughnessSquared);
	float weight = step(0.001, hit_co_pdf.w) * bsdf / hit_co_pdf.w;
	weight_acc += weight;

	vec3 sample = textureLod(colorBuffer, ref_uvs, mip).rgb;

	/* Do not add light if ray has failed. */
	sample *= float(has_hit);

	/* Firefly removal */
	sample /= 1.0 + fireflyFactor * brightness(sample);

	float mask = screen_border_mask(ref_uvs, hit_pos);
	mask *= view_facing_mask(V, N);
	mask *= float(has_hit);

	return vec4(sample, mask) * weight;
}

#define NUM_NEIGHBORS 9

void main()
{
	ivec2 fullres_texel = ivec2(gl_FragCoord.xy);
#ifdef FULLRES
	ivec2 halfres_texel = fullres_texel;
#else
	ivec2 halfres_texel = ivec2(gl_FragCoord.xy / 2.0);
#endif
	vec2 texture_size = vec2(textureSize(depthBuffer, 0));
	vec2 uvs = gl_FragCoord.xy / texture_size;
	vec3 rand = texelFetch(utilTex, ivec3(fullres_texel % LUT_SIZE, 2), 0).rba;

	float depth = textureLod(depthBuffer, uvs, 0.0).r;

	/* Early out */
	if (depth == 1.0)
		discard;

	/* Using world space */
	vec3 viewPosition = get_view_space_from_depth(uvs, depth); /* Needed for viewCameraVec */
	vec3 worldPosition = transform_point(ViewMatrixInverse, viewPosition);
	vec3 V = cameraVec;
	vec3 N = mat3(ViewMatrixInverse) * normal_decode(texelFetch(normalBuffer, fullres_texel, 0).rg, viewCameraVec);
	vec4 speccol_roughness = texelFetch(specroughBuffer, fullres_texel, 0).rgba;

	/* Early out */
	if (dot(speccol_roughness.rgb, vec3(1.0)) == 0.0)
		discard;

	float roughness = speccol_roughness.a;
	float roughnessSquared = max(1e-3, roughness * roughness);

	vec4 spec_accum = vec4(0.0);

	/* Resolve SSR */
	float cone_cos = cone_cosine(roughnessSquared);
	float cone_tan = sqrt(1 - cone_cos * cone_cos) / cone_cos;
	cone_tan *= mix(saturate(dot(N, V) * 2.0), 1.0, roughness); /* Elongation fit */

	vec2 source_uvs = project_point(PastViewProjectionMatrix, worldPosition).xy * 0.5 + 0.5;

	vec4 ssr_accum = vec4(0.0);
	float weight_acc = 0.0;
	const ivec2 neighbors[9] = ivec2[9](
		ivec2(0, 0),

		               ivec2(0,  1),
		ivec2(-1, -1),               ivec2(1, -1),

		ivec2(-1,  1),               ivec2(1,  1),
		               ivec2(0, -1),

		ivec2(-1,  0),               ivec2(1,  0)
	);
	ivec2 invert_neighbor;
	invert_neighbor.x = ((fullres_texel.x & 0x1) == 0) ? 1 : -1;
	invert_neighbor.y = ((fullres_texel.y & 0x1) == 0) ? 1 : -1;

	for (int i = 0; i < NUM_NEIGHBORS; i++) {
		ivec2 target_texel = halfres_texel + neighbors[i] * invert_neighbor;

		ssr_accum += get_ssr_sample(hitBuffer0, worldPosition, N, V,
		                            roughnessSquared, cone_tan, source_uvs,
		                            texture_size, target_texel, weight_acc);
		if (rayCount > 1) {
			ssr_accum += get_ssr_sample(hitBuffer1, worldPosition, N, V,
			                            roughnessSquared, cone_tan, source_uvs,
			                            texture_size, target_texel, weight_acc);
		}
		if (rayCount > 2) {
			ssr_accum += get_ssr_sample(hitBuffer2, worldPosition, N, V,
			                            roughnessSquared, cone_tan, source_uvs,
			                            texture_size, target_texel, weight_acc);
		}
		if (rayCount > 3) {
			ssr_accum += get_ssr_sample(hitBuffer3, worldPosition, N, V,
			                            roughnessSquared, cone_tan, source_uvs,
			                            texture_size, target_texel, weight_acc);
		}
	}

	/* Compute SSR contribution */
	if (weight_acc > 0.0) {
		ssr_accum /= weight_acc;
		/* fade between 0.5 and 1.0 roughness */
		ssr_accum.a *= saturate(2.0 - roughness * 2.0);
		accumulate_light(ssr_accum.rgb, ssr_accum.a, spec_accum);
	}

	/* If SSR contribution is not 1.0, blend with cubemaps */
	if (spec_accum.a < 1.0) {
		fallback_cubemap(N, V, worldPosition, roughness, roughnessSquared, spec_accum);
	}

	fragColor = vec4(spec_accum.rgb * speccol_roughness.rgb, 1.0);
}

#endif
