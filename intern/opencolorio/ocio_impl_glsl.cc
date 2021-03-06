/*
 * Adapted from OpenColorIO with this license:
 *
 * Copyright (c) 2003-2010 Sony Pictures Imageworks Inc., et al.
 * All Rights Reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * * Neither the name of Sony Pictures Imageworks nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Modifications Copyright 2013, Blender Foundation.
 */

#include <limits>
#include <sstream>
#include <string.h>

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable : 4251 4275)
#endif
#include <OpenColorIO/OpenColorIO.h>
#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#include "GPU_immediate.h"
#include "GPU_shader.h"
#include "GPU_uniform_buffer.h"

using namespace OCIO_NAMESPACE;

#include "MEM_guardedalloc.h"

#include "ocio_impl.h"

static const int LUT3D_EDGE_SIZE = 64;
static const int LUT3D_TEXTURE_SIZE = sizeof(float) * 3 * LUT3D_EDGE_SIZE * LUT3D_EDGE_SIZE *
                                      LUT3D_EDGE_SIZE;
static const int SHADER_CACHE_SIZE = 4;

#define UBO_BIND_LOC 0

extern "C" char datatoc_gpu_shader_display_transform_glsl[];
extern "C" char datatoc_gpu_shader_display_transform_vertex_glsl[];

/* **** OpenGL drawing routines using GLSL for color space transform ***** */

/* Curve mapping parameters
 *
 * See documentation for OCIO_CurveMappingSettings to get fields descriptions.
 * (this ones pretty much copies stuff from C structure.)
 */
struct OCIO_GLSLCurveMappingParameters {
  float curve_mapping_mintable[4];
  float curve_mapping_range[4];
  float curve_mapping_ext_in_x[4];
  float curve_mapping_ext_in_y[4];
  float curve_mapping_ext_out_x[4];
  float curve_mapping_ext_out_y[4];
  float curve_mapping_first_x[4];
  float curve_mapping_first_y[4];
  float curve_mapping_last_x[4];
  float curve_mapping_last_y[4];
  float curve_mapping_black[4];
  float curve_mapping_bwmul[4];
  int curve_mapping_lut_size;
  int curve_mapping_use_extend_extrapolate;
  int _pad[2];
  /** WARNING: Needs to be 16byte aligned. Used as UBO data. */
};

struct OCIO_GLSLShader {
  /** Cache IDs */
  std::string cacheId;

  struct GPUShader *shader;
  /** Uniform locations. */
  int dither_loc;
  int overlay_loc;
  int predivide_loc;
  int curve_mapping_loc;
  int ubo_bind;
  /** Error checking. */
  bool valid;
};

struct OCIO_GLSLLut3d {
  /** Cache IDs */
  std::string cacheId;
  /** OpenGL Texture handles. NULL if not allocated. */
  GPUTexture *texture;
  GPUTexture *texture_display;
  GPUTexture *texture_dummy;
  /** Error checking. */
  bool valid;
};

struct OCIO_GLSLCurveMappping {
  /** Cache IDs */
  size_t cacheId;
  /** GPU Uniform Buffer handle. 0 if not allocated. */
  GPUUniformBuf *buffer;
  /** OpenGL Texture handles. 0 if not allocated. */
  GPUTexture *texture;
  /** Error checking. */
  bool valid;
};

struct OCIO_GLSLCacheHandle {
  size_t cache_id;
  void *data;
};

struct OCIO_GLSLDrawState {
  /* Shader Cache */
  OCIO_GLSLCacheHandle shader_cache[SHADER_CACHE_SIZE];
  OCIO_GLSLCacheHandle lut3d_cache[SHADER_CACHE_SIZE];
  OCIO_GLSLCacheHandle curvemap_cache[SHADER_CACHE_SIZE];
};

static OCIO_GLSLDrawState *allocateOpenGLState(void)
{
  return (OCIO_GLSLDrawState *)MEM_callocN(sizeof(OCIO_GLSLDrawState), "OCIO OpenGL State struct");
}

/* -------------------------------------------------------------------- */
/** \name Shader
 * \{ */

static void updateGLSLShader(OCIO_GLSLShader *shader,
                             ConstProcessorRcPtr *processor_scene_to_ui,
                             ConstProcessorRcPtr *processpr_ui_to_display,
                             GpuShaderDesc *shader_desc,
                             const std::string &cache_id)
{
  if (shader->cacheId == cache_id) {
    return;
  }

  /* Delete any previous shader. */
  if (shader->shader) {
    GPU_shader_free(shader->shader);
  }

  std::ostringstream os;
  {
    /* Fragment shader */

    /* Work around OpenColorIO not supporting latest GLSL yet. */
    os << "#define texture2D texture\n";
    os << "#define texture3D texture\n";

    shader_desc->setFunctionName("OCIO_to_display_linear_with_look");
    os << (*processor_scene_to_ui)->getGpuShaderText(*shader_desc) << "\n";

    shader_desc->setFunctionName("OCIO_to_display_encoded");
    os << (*processpr_ui_to_display)->getGpuShaderText(*shader_desc) << "\n";

    os << datatoc_gpu_shader_display_transform_glsl;
  }

  shader->shader = GPU_shader_create(datatoc_gpu_shader_display_transform_vertex_glsl,
                                     os.str().c_str(),
                                     NULL,
                                     NULL,
                                     NULL,
                                     "OCIOShader");

  if (shader->shader) {
    shader->dither_loc = GPU_shader_get_uniform(shader->shader, "dither");
    shader->overlay_loc = GPU_shader_get_uniform(shader->shader, "overlay");
    shader->predivide_loc = GPU_shader_get_uniform(shader->shader, "predivide");
    shader->curve_mapping_loc = GPU_shader_get_uniform(shader->shader, "curve_mapping");
    shader->ubo_bind = GPU_shader_get_uniform_block_binding(shader->shader,
                                                            "OCIO_GLSLCurveMappingParameters");

    GPU_shader_bind(shader->shader);

    /* Set texture bind point uniform once. This is saved by the shader. */
    GPUShader *sh = shader->shader;
    GPU_shader_uniform_int(sh, GPU_shader_get_uniform(sh, "image_texture"), 0);
    GPU_shader_uniform_int(sh, GPU_shader_get_uniform(sh, "overlay_texture"), 1);
    GPU_shader_uniform_int(sh, GPU_shader_get_uniform(sh, "lut3d_texture"), 2);
    GPU_shader_uniform_int(sh, GPU_shader_get_uniform(sh, "lut3d_display_texture"), 3);
    GPU_shader_uniform_int(sh, GPU_shader_get_uniform(sh, "curve_mapping_texture"), 4);
  }

  shader->cacheId = cache_id;
  shader->valid = (shader->shader != NULL);
}

static void ensureGLSLShader(OCIO_GLSLShader **shader_ptr,
                             ConstProcessorRcPtr *processor_scene_to_ui,
                             ConstProcessorRcPtr *processpr_ui_to_display,
                             GpuShaderDesc *shader_desc,
                             const std::string &cache_id)
{
  if (*shader_ptr != NULL) {
    return;
  }

  OCIO_GLSLShader *shader = OBJECT_GUARDED_NEW(OCIO_GLSLShader);

  updateGLSLShader(shader, processor_scene_to_ui, processpr_ui_to_display, shader_desc, cache_id);

  *shader_ptr = shader;
}

static void freeGLSLShader(OCIO_GLSLShader *shader)
{
  if (shader->shader) {
    GPU_shader_free(shader->shader);
  }

  OBJECT_GUARDED_DELETE(shader, OCIO_GLSLShader);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Lut3D
 * \{ */

static void updateGLSLLut3d(OCIO_GLSLLut3d *lut3d,
                            ConstProcessorRcPtr *processor_scene_to_ui,
                            ConstProcessorRcPtr *processpr_ui_to_display,
                            GpuShaderDesc *shader_desc,
                            const std::string &cache_id)
{
  if (lut3d->cacheId == cache_id)
    return;

  float *lut_data = (float *)MEM_mallocN(LUT3D_TEXTURE_SIZE, __func__);

  ConstProcessorRcPtr *ocio_processors[2] = {processor_scene_to_ui, processpr_ui_to_display};

  for (int i = 0; i < 2; i++) {
    ConstProcessorRcPtr *processor = ocio_processors[i];
    GPUTexture *texture = (&lut3d->texture)[i];

    (*processor)->getGpuLut3D(lut_data, *shader_desc);

    int offset[3] = {0, 0, 0};
    int extent[3] = {LUT3D_EDGE_SIZE, LUT3D_EDGE_SIZE, LUT3D_EDGE_SIZE};
    GPU_texture_update_sub(texture, GPU_DATA_FLOAT, lut_data, UNPACK3(offset), UNPACK3(extent));
  }

  MEM_freeN(lut_data);

  lut3d->cacheId = cache_id;
}

static void ensureGLSLLut3d(OCIO_GLSLLut3d **lut3d_ptr,
                            ConstProcessorRcPtr *processor_scene_to_ui,
                            ConstProcessorRcPtr *processpr_ui_to_display,
                            GpuShaderDesc *shaderDesc,
                            const std::string &cache_id)
{
  if (*lut3d_ptr != NULL) {
    return;
  }

  OCIO_GLSLLut3d *lut3d = OBJECT_GUARDED_NEW(OCIO_GLSLLut3d);

  int extent[3] = {LUT3D_EDGE_SIZE, LUT3D_EDGE_SIZE, LUT3D_EDGE_SIZE};

  lut3d->texture = GPU_texture_create_3d(
      "OCIOLut", UNPACK3(extent), 1, GPU_RGB16F, GPU_DATA_FLOAT, NULL);
  GPU_texture_filter_mode(lut3d->texture, true);
  GPU_texture_wrap_mode(lut3d->texture, false, true);

  lut3d->texture_display = GPU_texture_create_3d(
      "OCIOLutDisplay", UNPACK3(extent), 1, GPU_RGB16F, GPU_DATA_FLOAT, NULL);
  GPU_texture_filter_mode(lut3d->texture_display, true);
  GPU_texture_wrap_mode(lut3d->texture_display, false, true);

  lut3d->texture_dummy = GPU_texture_create_error(2, false);

  updateGLSLLut3d(lut3d, processor_scene_to_ui, processpr_ui_to_display, shaderDesc, cache_id);

  lut3d->valid = (lut3d->texture && lut3d->texture_display);

  *lut3d_ptr = lut3d;
}

static void freeGLSLLut3d(OCIO_GLSLLut3d *lut3d)
{
  GPU_texture_free(lut3d->texture);
  GPU_texture_free(lut3d->texture_display);
  GPU_texture_free(lut3d->texture_dummy);

  OBJECT_GUARDED_DELETE(lut3d, OCIO_GLSLLut3d);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Curve Mapping
 * \{ */
static void allocateCurveMappingTexture(OCIO_GLSLCurveMappping *curvemap,
                                        OCIO_CurveMappingSettings *curve_mapping_settings)
{
  int lut_size = curve_mapping_settings ? curve_mapping_settings->lut_size : 1;
  /* Do not initialize. Only if used. */
  curvemap->texture = GPU_texture_create_1d("OCIOCurveMap", lut_size, 1, GPU_RGBA16F, NULL);
  GPU_texture_filter_mode(curvemap->texture, false);
  GPU_texture_wrap_mode(curvemap->texture, false, true);
}

/* curve_mapping_settings can be null. In this case we alloc a dummy curvemap. */
static void ensureGLSLCurveMapping(OCIO_GLSLCurveMappping **curvemap_ptr,
                                   OCIO_CurveMappingSettings *curve_mapping_settings)
{
  if (*curvemap_ptr != NULL) {
    return;
  }

  OCIO_GLSLCurveMappping *curvemap = OBJECT_GUARDED_NEW(OCIO_GLSLCurveMappping);

  /* Texture. */
  allocateCurveMappingTexture(curvemap, curve_mapping_settings);

  /* Uniform buffer object. */
  curvemap->buffer = GPU_uniformbuf_create(sizeof(OCIO_GLSLCurveMappingParameters));

  curvemap->valid = (curvemap->texture != 0);
  curvemap->cacheId = 0;

  *curvemap_ptr = curvemap;
}

static void freeGLSLCurveMapping(OCIO_GLSLCurveMappping *curvemap)
{
  GPU_texture_free(curvemap->texture);
  GPU_uniformbuf_free(curvemap->buffer);

  OBJECT_GUARDED_DELETE(curvemap, OCIO_GLSLCurveMappping);
}

static void updateGLSLCurveMapping(OCIO_GLSLCurveMappping *curvemap,
                                   OCIO_CurveMappingSettings *curve_mapping_settings,
                                   size_t cacheId)
{
  /* No need to continue if curvemapping is not used. Just use whatever is in this cache. */
  if (curve_mapping_settings == NULL)
    return;

  if (curvemap->cacheId == cacheId)
    return;

  if (curvemap->cacheId == 0) {
    /* This cache was previously used as dummy. Recreate the texture. */
    GPU_texture_free(curvemap->texture);
    allocateCurveMappingTexture(curvemap, curve_mapping_settings);
  }

  /* Update texture. */
  int offset[3] = {0, 0, 0};
  int extent[3] = {curve_mapping_settings->lut_size, 0, 0};
  const float *pixels = curve_mapping_settings->lut;
  GPU_texture_update_sub(
      curvemap->texture, GPU_DATA_FLOAT, pixels, UNPACK3(offset), UNPACK3(extent));

  /* Update uniforms. */
  OCIO_GLSLCurveMappingParameters data;
  for (int i = 0; i < 4; i++) {
    data.curve_mapping_range[i] = curve_mapping_settings->range[i];
    data.curve_mapping_mintable[i] = curve_mapping_settings->mintable[i];
    data.curve_mapping_ext_in_x[i] = curve_mapping_settings->ext_in_x[i];
    data.curve_mapping_ext_in_y[i] = curve_mapping_settings->ext_in_y[i];
    data.curve_mapping_ext_out_x[i] = curve_mapping_settings->ext_out_x[i];
    data.curve_mapping_ext_out_y[i] = curve_mapping_settings->ext_out_y[i];
    data.curve_mapping_first_x[i] = curve_mapping_settings->first_x[i];
    data.curve_mapping_first_y[i] = curve_mapping_settings->first_y[i];
    data.curve_mapping_last_x[i] = curve_mapping_settings->last_x[i];
    data.curve_mapping_last_y[i] = curve_mapping_settings->last_y[i];
  }
  for (int i = 0; i < 3; i++) {
    data.curve_mapping_black[i] = curve_mapping_settings->black[i];
    data.curve_mapping_bwmul[i] = curve_mapping_settings->bwmul[i];
  }
  data.curve_mapping_lut_size = curve_mapping_settings->lut_size;
  data.curve_mapping_use_extend_extrapolate = curve_mapping_settings->use_extend_extrapolate;

  GPU_uniformbuf_update(curvemap->buffer, &data);

  curvemap->cacheId = cacheId;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name LRU cache
 * \{ */

static size_t hash_string(const char *str)
{
  size_t i = 0, c;
  while ((c = *str++)) {
    i = i * 37 + c;
  }
  return i;
}

static OCIO_GLSLCacheHandle *cacheSearch(OCIO_GLSLCacheHandle cache[SHADER_CACHE_SIZE],
                                         size_t cache_id)
{
  OCIO_GLSLCacheHandle *cached_item = &cache[0];
  for (int i = 0; i < SHADER_CACHE_SIZE; i++, cached_item++) {
    if (cached_item->data == NULL) {
      continue;
    }
    else if (cached_item->cache_id == cache_id) {
      /* LRU cache, so move to front. */
      OCIO_GLSLCacheHandle found_item = *cached_item;
      for (int j = i; j > 0; j--) {
        cache[j] = cache[j - 1];
      }
      cache[0] = found_item;
      return &cache[0];
    }
  }
  /* LRU cache, shift other items back so we can insert at the front. */
  OCIO_GLSLCacheHandle last_item = cache[SHADER_CACHE_SIZE - 1];
  for (int j = SHADER_CACHE_SIZE - 1; j > 0; j--) {
    cache[j] = cache[j - 1];
  }
  /* Copy last to front and let the caller initialize it. */
  cache[0] = last_item;
  return &cache[0];
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name OCIO GLSL Implementation
 * \{ */

/* Detect if we can support GLSL drawing */
bool OCIOImpl::supportGLSLDraw()
{
  /* Minimum supported version 3.3 does meet all requirements. */
  return true;
}

/**
 * Setup OpenGL contexts for a transform defined by processor using GLSL
 * All LUT allocating baking and shader compilation happens here.
 *
 * Once this function is called, callee could start drawing images
 * using regular 2D texture.
 *
 * When all drawing is finished, finishGLSLDraw shall be called to
 * restore OpenGL context to its pre-GLSL draw state.
 */
bool OCIOImpl::setupGLSLDraw(OCIO_GLSLDrawState **state_r,
                             OCIO_ConstProcessorRcPtr *ocio_processor_scene_to_ui,
                             OCIO_ConstProcessorRcPtr *ocio_processor_ui_to_display,
                             OCIO_CurveMappingSettings *curve_mapping_settings,
                             float dither,
                             bool use_predivide,
                             bool use_overlay)
{
  ConstProcessorRcPtr processor_scene_to_ui = *(ConstProcessorRcPtr *)ocio_processor_scene_to_ui;
  ConstProcessorRcPtr processpr_ui_to_display = *(
      ConstProcessorRcPtr *)ocio_processor_ui_to_display;
  bool use_curve_mapping = curve_mapping_settings != NULL;

  if (!processor_scene_to_ui || !processor_scene_to_ui) {
    return false;
  }

  /* Create state if needed. */
  OCIO_GLSLDrawState *state;
  if (!*state_r)
    *state_r = allocateOpenGLState();
  state = *state_r;

  /* Compute cache IDs. */
  GpuShaderDesc shaderDesc;
  shaderDesc.setLanguage(GPU_LANGUAGE_GLSL_1_3);
  shaderDesc.setFunctionName("OCIODisplay");
  shaderDesc.setLut3DEdgeLen(LUT3D_EDGE_SIZE);

  const char *shader_cache_str = processor_scene_to_ui->getGpuShaderTextCacheID(shaderDesc);
  const char *lut3d_cache_str = processor_scene_to_ui->getGpuLut3DCacheID(shaderDesc);
  /* Used for comparison. */
  std::string shaderCacheID = shader_cache_str;
  std::string lut3dCacheID = lut3d_cache_str;

  size_t shader_cache_id = hash_string(shader_cache_str);
  size_t lut3d_cache_id = hash_string(lut3d_cache_str);
  size_t curvemap_cache_id = curve_mapping_settings ? curve_mapping_settings->cache_id : 0;

  OCIO_GLSLCacheHandle *shader_handle = cacheSearch(state->shader_cache, shader_cache_id);
  OCIO_GLSLCacheHandle *lut3d_handle = cacheSearch(state->lut3d_cache, lut3d_cache_id);
  /* We cannot keep more than one cache for curvemap because their cache id is a pointer.
   * The pointer cannot be the same for one update but can be the same after a second update. */
  OCIO_GLSLCacheHandle *curvemap_handle = &state->curvemap_cache[0];

  OCIO_GLSLShader **shader_ptr = (OCIO_GLSLShader **)&shader_handle->data;
  OCIO_GLSLLut3d **lut3d_ptr = (OCIO_GLSLLut3d **)&lut3d_handle->data;
  OCIO_GLSLCurveMappping **curvemap_ptr = (OCIO_GLSLCurveMappping **)&curvemap_handle->data;

  ensureGLSLShader(
      shader_ptr, &processor_scene_to_ui, &processpr_ui_to_display, &shaderDesc, shaderCacheID);
  ensureGLSLLut3d(
      lut3d_ptr, &processor_scene_to_ui, &processpr_ui_to_display, &shaderDesc, shaderCacheID);
  ensureGLSLCurveMapping(curvemap_ptr, curve_mapping_settings);

  OCIO_GLSLShader *shader = (OCIO_GLSLShader *)shader_handle->data;
  OCIO_GLSLLut3d *shader_lut = (OCIO_GLSLLut3d *)lut3d_handle->data;
  OCIO_GLSLCurveMappping *shader_curvemap = (OCIO_GLSLCurveMappping *)curvemap_handle->data;

  updateGLSLShader(
      shader, &processor_scene_to_ui, &processpr_ui_to_display, &shaderDesc, shaderCacheID);
  updateGLSLLut3d(
      shader_lut, &processor_scene_to_ui, &processpr_ui_to_display, &shaderDesc, lut3dCacheID);
  updateGLSLCurveMapping(shader_curvemap, curve_mapping_settings, curvemap_cache_id);

  /* Update handles cache keys. */
  shader_handle->cache_id = shader_cache_id;
  lut3d_handle->cache_id = lut3d_cache_id;
  curvemap_handle->cache_id = curvemap_cache_id;

  if (shader->valid && shader_lut->valid && shader_curvemap->valid) {
    /* Bind textures to sampler units. Texture 0 is set by caller.
     * Uniforms have already been set for texture bind points.*/

    if (!use_overlay) {
      /* Avoid missing binds. */
      GPU_texture_bind(shader_lut->texture_dummy, 1);
    }
    GPU_texture_bind(shader_lut->texture, 2);
    GPU_texture_bind(shader_lut->texture_display, 3);
    GPU_texture_bind(shader_curvemap->texture, 4);

    /* Bind UBO. */
    GPU_uniformbuf_bind(shader_curvemap->buffer, shader->ubo_bind);

    /* TODO(fclem): remove remains of IMM. */
    immBindShader(shader->shader);

    /* Bind Shader and set uniforms. */
    // GPU_shader_bind(shader->shader);
    GPU_shader_uniform_float(shader->shader, shader->dither_loc, dither);
    GPU_shader_uniform_int(shader->shader, shader->overlay_loc, use_overlay);
    GPU_shader_uniform_int(shader->shader, shader->predivide_loc, use_predivide);
    GPU_shader_uniform_int(shader->shader, shader->curve_mapping_loc, use_curve_mapping);

    return true;
  }

  return false;
}

void OCIOImpl::finishGLSLDraw(OCIO_GLSLDrawState * /*state*/)
{
  immUnbindProgram();
}

void OCIOImpl::freeGLState(OCIO_GLSLDrawState *state)
{
  for (int i = 0; i < SHADER_CACHE_SIZE; i++) {
    if (state->shader_cache[i].data) {
      freeGLSLShader((OCIO_GLSLShader *)state->shader_cache[i].data);
    }
    if (state->lut3d_cache[i].data) {
      freeGLSLLut3d((OCIO_GLSLLut3d *)state->lut3d_cache[i].data);
    }
    if (state->curvemap_cache[i].data) {
      freeGLSLCurveMapping((OCIO_GLSLCurveMappping *)state->curvemap_cache[i].data);
    }
  }

  MEM_freeN(state);
}

/** \} */
