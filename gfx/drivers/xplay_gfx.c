/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *  Copyright (C) 2011-2017 - Higor Euripedes
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <string.h>

#include <retro_assert.h>
#include <gfx/scaler/scaler.h>
#include <gfx/video_frame.h>
#include <retro_assert.h>
#include "../../verbosity.h"

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#ifdef HAVE_MENU
#include "../../menu/menu_driver.h"
#endif

#include "../font_driver.h"

#include "../../configuration.h"
#include "../../retroarch.h"

#define SCREEN_WIDTH 854
#define SCREEN_HEIGHT 480
#define XPLAY_FRAMEBUF_COUNT 2

typedef enum xplay_framebuf_format {
   PF_NONE,
   PF_RGBA16_4444,
   PF_RGB16_565,
   PF_RGBA32,
   PF_RGB32,
} xplay_framebuf_format_t;

typedef struct xplay_framebuf_texture
{
   GLuint handle;
   int width;
   int height;
   xplay_framebuf_format_t fmt;
   uint8_t *tempbuf;
   float verts[8];
} xplay_framebuf_texture_t;

typedef struct xplay_framebuf_program {
   GLuint handle;
   GLint pos_loc;
   GLint tex_loc;
   GLint sampler_loc;
   GLint alpha_loc;
} xplay_framebuf_program_t;

typedef struct xplay_video
{
   void *ctx_data;
   const gfx_ctx_driver_t *ctx_driver;

   xplay_framebuf_program_t *rgba_program;
   xplay_framebuf_program_t *bgra_program;
   xplay_framebuf_program_t *rgba_program_grid2x;
   xplay_framebuf_program_t *bgra_program_grid2x;
   xplay_framebuf_program_t *rgba_program_grid3x;
   xplay_framebuf_program_t *bgra_program_grid3x;

   float menu_alpha;
   bool menu_rgb32;
   bool frame_rgb32;

   xplay_framebuf_texture_t menu_tex[XPLAY_FRAMEBUF_COUNT];
   int menu_tex_index;

   xplay_framebuf_texture_t frame_tex[XPLAY_FRAMEBUF_COUNT];
   int frame_tex_index;

   GLuint font_atlas_tex;
   int font_atlas_width;
   int font_atlas_height;
   GLuint font_program;
   GLint font_pos_loc;
   GLint font_tex_loc;
   GLint font_sampler_loc;
   GLint font_color_loc;
   GLint font_px_loc;
   float font_r;
   float font_g;
   float font_b;
   void *font;
   const font_renderer_driver_t *font_driver;
   float *text_verts;
   int text_verts_size;
   int text_verts_capacity;
} xplay_video_t;

static bool xplay_check_error() {
   GLenum gl_error = glGetError();
   if (gl_error != GL_NO_ERROR) {
      RARCH_ERR("[XPLAY] OpenGL error: %d", (int)gl_error);
      return false;
   }
   return true;
}

static GLuint xplay_load_shader(const char *src, GLenum type) {
   GLuint shader = glCreateShader(type);
   if(!shader) {
      xplay_check_error();
      return 0;
   }

   glShaderSource(shader, 1, &src, NULL);
   glCompileShader(shader);

   GLint compiled;
   glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
   if (compiled)
      return shader;

   GLint infoLen = 0;
   glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLen);
   if(infoLen > 1) {
      char *infoLog = (char *)malloc(sizeof(char) * infoLen);
      glGetShaderInfoLog(shader, infoLen, NULL, infoLog);
      RARCH_ERR("[XPLAY] Error compiling shader: %s", infoLog);
      free(infoLog);
   }
   glDeleteShader(shader);
   return 0;
}



static GLuint xplay_load_program(const char *vert_shader_text, const char *frag_shader_text) {
   GLuint vert_shader = xplay_load_shader(vert_shader_text, GL_VERTEX_SHADER);
   if (!vert_shader)
      return 0;

   GLuint frag_shader = xplay_load_shader(frag_shader_text, GL_FRAGMENT_SHADER);
   if (!frag_shader) {
      glDeleteShader(vert_shader);
      return 0;
   }

   GLuint program = glCreateProgram();
   if (!program) {
      glDeleteShader(vert_shader);
      glDeleteShader(frag_shader);
      return 0;
   }

   glAttachShader(program, vert_shader);
   glAttachShader(program, frag_shader);
   glLinkProgram(program);

   GLint linked;
   glGetProgramiv(program, GL_LINK_STATUS, &linked);
   if (!linked) {
      GLint infoLen = 0;
      glGetProgramiv(program, GL_INFO_LOG_LENGTH, &infoLen);
      if (infoLen > 1) {
         char *infoLog = (char *)malloc(sizeof(char) * infoLen);
         glGetProgramInfoLog(program, infoLen, NULL, infoLog);
         RARCH_ERR("[XPLAY] Error linking program: %s", infoLog);
         free(infoLog);
      }
      glDeleteProgram(program);
      return 0;
   }

   return program;
}

static xplay_framebuf_program_t *xplay_load_framebuf_program(bool bgr, int hlines, int vlines) {
   static const char *vert_shader =
      "attribute vec4 a_position; \n"
      "attribute vec2 a_texCoord; \n"
      "varying vec2 v_texCoord; \n"
      "void main() \n"
      "{ \n"
      " gl_Position = a_position; \n"
      " v_texCoord = a_texCoord; \n"
      "} \n";

   char grid[128] = "";
   if (hlines || vlines) {
      char modx[128] = "";
      char mody[128] = "";
      if (hlines)
         sprintf(mody, "floor(mod(gl_FragCoord.y, %d.0))", hlines);
      else
         sprintf(mody, "1.0");
      if (vlines)
         sprintf(modx, "floor(mod(gl_FragCoord.x, %d.0))", vlines);
      else
         sprintf(modx, "1.0");
      sprintf(grid, "min(1.0, min(%s, %s))", mody, modx);
   } else {
      sprintf(grid, "1.0");
   }


   float hue = 0.0f; // 0.0 is neutral
   float saturation = 0.6f; // 0.5 is neutral
   float contrast = 0.55f; // 0.5 is neutral

   // 0.5 is neutral
   char brightness[128] = "";
   if (hlines == 2) {
      sprintf(brightness, "0.5 + (grid * 0.05)");
      contrast = 0.57f;
   } else if (hlines == 3) {
      sprintf(brightness, "0.5 - ((1.0 - grid) * 0.03)");
   } else {
      sprintf(brightness, "0.5");
   }


   char frag_shader[32*1024];
   sprintf(frag_shader,
           "precision mediump float; \n"
           "varying vec2 v_texCoord; \n"
           "uniform sampler2D s_texture; \n"
           "uniform float f_alpha; \n"
           "vec3 applyHue(vec3 aColor, float aHue) { \n"
           "  float angle = radians(aHue); \n"
           "  vec3 k = vec3(0.57735, 0.57735, 0.57735); \n"
           "  float cosAngle = cos(angle); \n"
           "  return aColor * cosAngle + cross(k, aColor) * sin(angle) + k * dot(k, aColor) * (1.0 - cosAngle); \n"
           "} \n"
           "vec4 applyHSBCEffect(vec4 startColor, vec4 hsbc) { \n"
           "  float _Hue = 360.0 * hsbc.r; \n"
           "  float _Saturation = hsbc.g * 2.0; \n"
           "  float _Brightness = hsbc.b * 2.0 - 1.0; \n"
           "  float _Contrast = hsbc.a * 2.0; \n"
           "  vec4 outputColor = startColor; \n"
           "  outputColor.rgb = applyHue(outputColor.rgb, _Hue); \n"
           "  outputColor.rgb = (outputColor.rgb - 0.5) * (_Contrast) + 0.5; \n"
           "  outputColor.rgb = outputColor.rgb + _Brightness; \n"
           "  vec3 intensity = vec3(dot(outputColor.rgb, vec3(0.299, 0.587, 0.114))); \n"
           "  outputColor.rgb = mix(intensity, outputColor.rgb, _Saturation); \n"
           "  return outputColor; \n"
           "} \n"
           "void main() { \n"
           "  vec4 c = texture2D(s_texture, v_texCoord); \n"
           "  float grid = %s; \n"
           "  float hue = %.2f; \n"
           "  float saturation = %.2f; \n"
           "  float brightness = %s; \n"
           "  float contrast = %.2f; \n"
           "  gl_FragColor = applyHSBCEffect(vec4(c.%s, c.a * f_alpha), vec4(hue, saturation, brightness, contrast));\n"
           "} \n", grid, hue, saturation, brightness, contrast, bgr ? "bgr" : "rgb");

   GLuint program = xplay_load_program(vert_shader, frag_shader);
   if (!program) {
      return NULL;
   }

   GLint pos_loc = glGetAttribLocation(program, "a_position");
   if (pos_loc < 0) goto error;

   GLint tex_loc = glGetAttribLocation(program, "a_texCoord");
   if (tex_loc < 0) goto error;

   GLint sampler_loc = glGetUniformLocation(program, "s_texture");
   if (sampler_loc < 0) goto error;

   GLint alpha_loc = glGetUniformLocation(program, "f_alpha");
   if (alpha_loc < 0) goto error;

   xplay_framebuf_program_t *prog = calloc(1, sizeof(xplay_framebuf_program_t));
   prog->handle = program;
   prog->pos_loc = pos_loc;
   prog->tex_loc = tex_loc;
   prog->sampler_loc = sampler_loc;
   prog->alpha_loc = alpha_loc;
   return prog;
error:
   glDeleteProgram(program);
   return NULL;
}

static void xplay_destroy_program(xplay_framebuf_program_t *prog) {
   if (prog) {
      glDeleteProgram(prog->handle);
      free(prog);
   }
}

static bool xplay_init_texture(GLuint handle) {
   if (!handle) return false;

   glBindTexture(GL_TEXTURE_2D, handle);
   if (!xplay_check_error()) return false;

   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   if (!xplay_check_error()) return false;
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   if (!xplay_check_error()) return false;

   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   if (!xplay_check_error()) return false;
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   if (!xplay_check_error()) return false;

   return true;
}

static void xplay_blit_framebuf(xplay_framebuf_texture_t *tex, const void *frame, unsigned width, unsigned height, unsigned pitch, xplay_framebuf_format_t fmt) {
   unsigned pixel_size;
   GLenum format;
   GLenum type;
   switch (fmt) {
      default:
         return;
      case PF_RGBA16_4444:
         pixel_size = 2;
         format = GL_RGBA;
         type = GL_UNSIGNED_SHORT_4_4_4_4;
         break;
      case PF_RGB16_565:
         pixel_size = 2;
         format = GL_RGB;
         type = GL_UNSIGNED_SHORT_5_6_5;
         break;
      case PF_RGBA32:
      case PF_RGB32:
         pixel_size = 4;
         format = GL_RGBA;
         type = GL_UNSIGNED_BYTE;
         break;
   }
   unsigned expected_pitch = pixel_size * width;

   glBindTexture(GL_TEXTURE_2D, tex->handle);

   if (tex->width != width || tex->height != height || tex->fmt != fmt) {
      tex->width = width;
      tex->height = height;
      tex->fmt = fmt;
      free(tex->tempbuf);
      tex->tempbuf = NULL;
      glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, format, type, NULL);
   }

   const void *source = frame;
   if (pitch != expected_pitch) {
      if (!tex->tempbuf) {
         tex->tempbuf = malloc(expected_pitch * height);
      }
      source = tex->tempbuf;
      for (unsigned y = 0; y < height; ++y) {
         memcpy(tex->tempbuf + y*expected_pitch, (const uint8_t *)frame + y*pitch, expected_pitch);
      }
   }

   glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, type, source);
}

static void xplay_get_scale_factor(int width, int height, int *sx, int *sy) {
   int x = SCREEN_WIDTH / width;
   int y = SCREEN_HEIGHT / height;
   *sx = *sy = MIN(x, y);
   if (width / height >= 2 && height * (*sy + 1) <= 480) {
      *sy += 1;
   }
}

static xplay_framebuf_program_t *xplay_get_framebuf_program(xplay_video_t *vid, bool rgba32, int width, int height, bool allow_grid) {
   int sx, sy;
   xplay_get_scale_factor(width, height, &sx, &sy);
   if (!allow_grid || sx != sy) {
      return rgba32 ? vid->bgra_program : vid->rgba_program;
   }
   switch (sx) {
      default: return rgba32 ? vid->bgra_program : vid->rgba_program;
      case 2: return rgba32 ? vid->bgra_program_grid2x : vid->rgba_program_grid2x;
      case 3: return rgba32 ? vid->bgra_program_grid3x : vid->rgba_program_grid3x;
   }
}

static void xplay_draw_framebuf(xplay_framebuf_program_t *prog, xplay_framebuf_texture_t *tex, bool integer_scale, float alpha, bool bgr) {
   static float tex_coords[] = {
      0.0, 0.0,
      0.0, 1.0,
      1.0, 0.0,
      1.0, 1.0
   };

   static GLushort indices[] = { 0, 1, 2, 1, 2, 3 };

   if (tex->width < 1 || tex->height < 1) {
      return;
   }

   float dx = 1;
   float dy = 1;
   if (integer_scale) {
      int sx, sy;
      xplay_get_scale_factor(tex->width, tex->height, &sx, &sy);
      int w = sx * tex->width;
      int h = sy * tex->height;
      dx = (w / 2.f) / (SCREEN_WIDTH / 2.f);
      dy = (h / 2.f) / (SCREEN_HEIGHT / 2.f);
   }

   float *v = tex->verts;

   // top left
   v[0] = -dx;
   v[1] = dy;

   // bottom left
   v[2] = -dx;
   v[3] = -dy;

   // top right
   v[4] = dx;
   v[5] = dy;

   // bottom right
   v[6] = dx;
   v[7] = -dy;

   glUseProgram(prog->handle);
   glVertexAttribPointer(prog->pos_loc, 2, GL_FLOAT, GL_FALSE, 2*sizeof(GLfloat), v);
   glVertexAttribPointer(prog->tex_loc, 2, GL_FLOAT, GL_FALSE, 2*sizeof(GLfloat), tex_coords);
   glEnableVertexAttribArray(prog->pos_loc);
   glEnableVertexAttribArray(prog->tex_loc);
   glBindTexture(GL_TEXTURE_2D, tex->handle);
   glUniform1i(prog->sampler_loc, 0);
   glUniform1f(prog->alpha_loc, alpha);
   glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, indices);
}

static void xplay_destroy_framebuf(xplay_framebuf_texture_t *tex) {
   glDeleteTextures(1, &tex->handle);
   free(tex->tempbuf);
}

static void xplay_gfx_free(void *data) {
   xplay_video_t *vid = (xplay_video_t*)data;
   if (!vid)
      return;

   if (vid->font_program)
      glDeleteProgram(vid->font_program);
   if (vid->font_atlas_tex)
      glDeleteTextures(1, &vid->font_atlas_tex);
   if (vid->font)
      vid->font_driver->free(vid->font);
   free(vid->text_verts);

   xplay_destroy_program(vid->rgba_program);
   xplay_destroy_program(vid->bgra_program);
   xplay_destroy_program(vid->rgba_program_grid2x);
   xplay_destroy_program(vid->bgra_program_grid2x);
   xplay_destroy_program(vid->rgba_program_grid3x);
   xplay_destroy_program(vid->bgra_program_grid3x);

   for (int i = 0; i < XPLAY_FRAMEBUF_COUNT; ++i) {
      xplay_destroy_framebuf(&vid->menu_tex[i]);
      xplay_destroy_framebuf(&vid->frame_tex[i]);
   }

   if (vid->ctx_driver && vid->ctx_driver->destroy)
      vid->ctx_driver->destroy(vid->ctx_data);
   video_context_driver_free();

   free(vid);
}

static void xplay_maybe_init_font_texture(xplay_video_t *vid) {
   if (!vid->font)
      return;
   struct font_atlas *atlas = vid->font_driver->get_atlas(vid->font);
   if (atlas->dirty) {
      RARCH_LOG("[XPLAY]: Updating font atlas texture (%dx%d)...\n", atlas->width, atlas->height);
      if (!vid->font_atlas_tex) {
         glGenTextures(1, &vid->font_atlas_tex);
         xplay_check_error();
         xplay_init_texture(vid->font_atlas_tex);
      }
      glBindTexture(GL_TEXTURE_2D, vid->font_atlas_tex);
      if (vid->font_atlas_width != atlas->width || vid->font_atlas_height != atlas->height) {
         vid->font_atlas_width = atlas->width;
         vid->font_atlas_height = atlas->height;
         glTexImage2D(GL_TEXTURE_2D, 0,  GL_ALPHA, atlas->width, atlas->height, 0, GL_ALPHA,  GL_UNSIGNED_BYTE, NULL);
         xplay_check_error();
      }
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, atlas->width, atlas->height, GL_ALPHA,  GL_UNSIGNED_BYTE, atlas->buffer);
      xplay_check_error();
      atlas->dirty = false;
      RARCH_LOG("[XPLAY]: Font atlas texture updated.\n");
   }
}

static void xplay_load_font_program(xplay_video_t *vid) {
   static const char *vert_shader =
      "attribute vec2 a_position; \n"
      "attribute vec2 a_texCoord; \n"
      "varying vec2 v_texCoord; \n"
      "void main() \n"
      "{ \n"
      " gl_Position = vec4(a_position, 0.0, 1.0); \n"
      " v_texCoord = a_texCoord; \n"
      "} \n";

   static const char *frag_shader =
      "precision mediump float; \n"
      "varying vec2 v_texCoord; \n"
      "uniform sampler2D s_texture; \n"
      "uniform vec3 v_color; \n"
      "uniform vec2 v_px; \n"
      "void main() { \n"
      "  float a = texture2D(s_texture, v_texCoord).a; \n"
      "  float b = texture2D(s_texture, v_texCoord - v_px).a; \n"
      "  gl_FragColor = a * vec4(v_color, 1.0) + (1.0 - a) *  b * vec4(v_color * 0.25, 1.0);\n"
      "} \n";

   GLuint program = xplay_load_program(vert_shader, frag_shader);
   if (!program) {
      RARCH_ERR("[XPLAY] Error loading font program.");
      return;
   }

   GLint pos_loc = glGetAttribLocation(program, "a_position");
   if (pos_loc < 0) goto error;

   GLint tex_loc = glGetAttribLocation(program, "a_texCoord");
   if (tex_loc < 0) goto error;

   GLint sampler_loc = glGetUniformLocation(program, "s_texture");
   if (sampler_loc < 0) goto error;

   GLint color_loc = glGetUniformLocation(program, "v_color");
   if (color_loc < 0) goto error;

   GLint px_loc = glGetUniformLocation(program, "v_px");
   if (px_loc < 0) goto error;

   vid->font_program = program;
   vid->font_pos_loc = pos_loc;
   vid->font_tex_loc = tex_loc;
   vid->font_sampler_loc = sampler_loc;
   vid->font_color_loc = color_loc;
   vid->font_px_loc = px_loc;
   return;
error:
   RARCH_ERR("[XPLAY] Error looking up font program names.");
   glDeleteProgram(program);
}

static void xplay_init_font(xplay_video_t *vid,
      bool video_font_enable,
      const char *path_font,
      float video_font_size,
      float msg_color_r,
      float msg_color_g,
      float msg_color_b
      )
{
   if (!video_font_enable)
      return;

   if (!font_renderer_create_default(
            &vid->font_driver, &vid->font,
            *path_font ? path_font : NULL,
            video_font_size))
   {
      RARCH_LOG("[XPLAY]: Could not initialize font.\n");
      return;
   }

   vid->font_r = msg_color_r;
   vid->font_g = msg_color_g;
   vid->font_b = msg_color_b;

   xplay_maybe_init_font_texture(vid);
   if (!vid->font_atlas_tex) {
      RARCH_ERR("[XPLAY] Error initializing font atlas texture.");
      return;
   }

   xplay_load_font_program(vid);
   RARCH_LOG("[XPLAY] Font init complete.");
}

static void xplay_render_msg(
      xplay_video_t *vid,
      const char *msg,
      unsigned width,
      unsigned height,
      float msg_pos_x,
      float msg_pos_y
      )
{
   if (!vid->font)
      return;

   xplay_maybe_init_font_texture(vid);
   if (!vid->font_atlas_tex) {
      return;
   }

   int msg_base_x = msg_pos_x * width;
   int msg_base_y = msg_pos_y * height;

   vid->text_verts_size = 0;

   const char *ch = msg;
   for (; *ch; ++ch) {
      const struct font_glyph *glyph = vid->font_driver->get_glyph(vid->font, (uint8_t)*ch);
      if (!glyph)
         continue;

      int base_x = msg_base_x + glyph->draw_offset_x;
      int base_y = msg_base_y - glyph->draw_offset_y;
      msg_base_x += glyph->advance_x;
      msg_base_y += glyph->advance_y;

      int glyph_width = glyph->width + 1;
      int glyph_height = glyph->height + 1;

      float x0 = -1.0f + 2.0f * (base_x / (float)width);
      float y0 = -1.0f + 2.0f * (base_y / (float)height);
      float x1 = -1.0f + 2.0f * ((base_x + glyph_width) / (float)width);
      float y1 = -1.0f + 2.0f * ((base_y - glyph_height) / (float)height);

      float tx0 = glyph->atlas_offset_x / (float)vid->font_atlas_width;
      float ty0 = glyph->atlas_offset_y / (float)vid->font_atlas_height;
      float tx1 = (glyph->atlas_offset_x + glyph_width) / (float)vid->font_atlas_width;
      float ty1 = (glyph->atlas_offset_y + glyph_height) / (float)vid->font_atlas_height;

      if (vid->text_verts_size + 24 > vid->text_verts_capacity) {
         vid->text_verts_capacity = vid->text_verts_capacity ? vid->text_verts_capacity * 2 : 4096;
         vid->text_verts = realloc(vid->text_verts, sizeof(float) * vid->text_verts_capacity);
      }
      float *v = vid->text_verts + vid->text_verts_size;

      v[0]  = x0; v[1]  = y0; v[2]  = tx0; v[3]  = ty0; // bottom left
      v[4]  = x0; v[5]  = y1; v[6]  = tx0; v[7]  = ty1; // top left
      v[8]  = x1; v[9]  = y0; v[10] = tx1; v[11] = ty0; // bottom right

      v[12] = x0; v[13] = y1; v[14] = tx0; v[15] = ty1; // top left
      v[16] = x1; v[17] = y1; v[18] = tx1; v[19] = ty1; // top right
      v[20] = x1; v[21] = y0; v[22] = tx1; v[23] = ty0; // bottom right

      vid->text_verts_size += 24;
   }

   glUseProgram(vid->font_program);
   glVertexAttribPointer(vid->font_pos_loc, 2, GL_FLOAT, GL_FALSE, 4*sizeof(GLfloat), vid->text_verts);
   glVertexAttribPointer(vid->font_tex_loc, 2, GL_FLOAT, GL_FALSE, 4*sizeof(GLfloat), vid->text_verts + 2);
   glEnableVertexAttribArray(vid->font_pos_loc);
   glEnableVertexAttribArray(vid->font_tex_loc);
   glBindTexture(GL_TEXTURE_2D, vid->font_atlas_tex);
   glUniform1i(vid->font_sampler_loc, 0);
   glUniform3f(vid->font_color_loc, vid->font_r, vid->font_g, vid->font_b);
   glUniform2f(vid->font_px_loc, 1.0f / vid->font_atlas_width, 1.0f / vid->font_atlas_height);
   glDrawArrays(GL_TRIANGLES, 0, vid->text_verts_size / 4);
}


static const gfx_ctx_driver_t *xplay_get_context(xplay_video_t *vid) {
   vid->ctx_data = NULL;
   return video_context_driver_init_first(vid, "", GFX_CTX_OPENGL_ES_API, 2, 0, false, &vid->ctx_data);
}

static void *xplay_gfx_init(const video_info_t *video,
      input_driver_t **input, void **input_data)
{
   xplay_video_t *vid = NULL;
   settings_t *settings = config_get_ptr();

   vid = (xplay_video_t*)calloc(1, sizeof(*vid));
   if (!vid)
      return NULL;

   vid->frame_rgb32 = video->rgb32;

   vid->ctx_driver = xplay_get_context(vid);
   if (!vid->ctx_driver)
      goto error;
   RARCH_LOG("[XPLAY]: Found GL context: \"%s\".\n", vid->ctx_driver->ident);

   video_context_driver_set(vid->ctx_driver);
   video_driver_set_size(SCREEN_WIDTH, SCREEN_HEIGHT);

   int interval    = 0;
   if (video->vsync)
      interval = video->swap_interval;
   if (vid->ctx_driver->swap_interval)
   {
      bool adaptive_vsync_enabled            = video_driver_test_all_flags(
            GFX_CTX_FLAGS_ADAPTIVE_VSYNC) && video->adaptive_vsync;
      if (adaptive_vsync_enabled && interval == 1)
         interval = -1;
      vid->ctx_driver->swap_interval(vid->ctx_data, interval);
   }

   if (     !vid->ctx_driver->set_video_mode
         || !vid->ctx_driver->set_video_mode(vid->ctx_data,
            SCREEN_WIDTH, SCREEN_HEIGHT, true))
   {
      RARCH_ERR("[XPLAY] Error setting video mode.\n");
      goto error;
   }

   rglgen_resolve_symbols(vid->ctx_driver->get_proc_address);

   /* Clear out potential error flags in case we use cached context. */
   glGetError();

   const char *vendor   = (const char*)glGetString(GL_VENDOR);
   const char *renderer = (const char*)glGetString(GL_RENDERER);
   const char *version  = (const char*)glGetString(GL_VERSION);

   RARCH_LOG("[XPLAY]: Vendor: %s, Renderer: %s.\n", vendor, renderer);
   RARCH_LOG("[XPLAY]: Version: %s.\n", version);

   if (string_is_equal(vid->ctx_driver->ident, "null"))
      goto error;

   {
      char device_str[128];

      device_str[0] = '\0';

      if (!string_is_empty(vendor))
      {
        strlcpy(device_str, vendor, sizeof(device_str));
        strlcat(device_str, " ", sizeof(device_str));
      }

      if (!string_is_empty(renderer))
        strlcat(device_str, renderer, sizeof(device_str));

      video_driver_set_gpu_device_string(device_str);

      if (!string_is_empty(version))
        video_driver_set_gpu_api_version_string(version);
   }

   RARCH_LOG("[XPLAY] Trying to get hardware context.\n");
   struct retro_hw_render_callback *hwr = video_driver_get_hw_context();
   RARCH_LOG("[XPLAY] Got context type: %d\n", (int)hwr->context_type);

   glDepthFunc(GL_ALWAYS);
   if (!xplay_check_error()) goto error;

   glDisable(GL_DEPTH_TEST);
   if (!xplay_check_error()) goto error;

   glDisable(GL_STENCIL_TEST);
   if (!xplay_check_error()) goto error;

   glDisable(GL_CULL_FACE);
   if (!xplay_check_error()) goto error;

   glDisable(GL_BLEND);
   if (!xplay_check_error()) goto error;

   glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
   if (!xplay_check_error()) goto error;

   glBlendEquation(GL_FUNC_ADD);
   if (!xplay_check_error()) goto error;

   vid->rgba_program = xplay_load_framebuf_program(false, 0, 0);
   if (!vid->rgba_program) goto error;

   vid->bgra_program = xplay_load_framebuf_program(true, 0, 0);
   if (!vid->bgra_program) goto error;

   vid->rgba_program_grid2x = xplay_load_framebuf_program(false, 2, 2);
   if (!vid->rgba_program_grid2x) goto error;

   vid->bgra_program_grid2x = xplay_load_framebuf_program(true, 2, 2);
   if (!vid->bgra_program_grid2x) goto error;

   vid->rgba_program_grid3x = xplay_load_framebuf_program(false, 3, 3);
   if (!vid->rgba_program_grid3x) goto error;

   vid->bgra_program_grid3x = xplay_load_framebuf_program(true, 3, 3);
   if (!vid->bgra_program_grid3x) goto error;

   glClearColor(0, 0, 0, 1);
   if (!xplay_check_error()) goto error;

   for (int i = 0; i < XPLAY_FRAMEBUF_COUNT; ++i) {
      glGenTextures(1, &vid->menu_tex[i].handle);
      if (!xplay_init_texture(vid->menu_tex[i].handle)) goto error;

      glGenTextures(1, &vid->frame_tex[i].handle);
      if (!xplay_init_texture(vid->frame_tex[i].handle)) goto error;
   }

   {
      xplay_init_font(vid,
            settings->bools.video_font_enable,
            settings->paths.path_font,
            settings->floats.video_font_size,
            settings->floats.video_msg_color_r,
            settings->floats.video_msg_color_g,
            settings->floats.video_msg_color_b);
   }

   if (vid->ctx_driver->input_driver)
   {
      const char *joypad_name = settings->arrays.input_joypad_driver;
      vid->ctx_driver->input_driver(
            vid->ctx_data, joypad_name,
            input, input_data);
   }

   RARCH_LOG("[XPLAY] Video driver init complete.\n");
   return vid;

error:
   xplay_gfx_free(vid);
   return NULL;
}

static bool xplay_gfx_frame(void *data, const void *frame, unsigned width,
      unsigned height, uint64_t frame_count,
      unsigned pitch, const char *msg, video_frame_info_t *video_info)
{
   xplay_video_t *vid = (xplay_video_t*)data;
   if (!frame)
      return true;

   settings_t *settings = config_get_ptr();
   bool integer_scale = settings->bools.video_scale_integer;
   bool want_grid = settings->bools.video_notch_write_over_enable;

   xplay_framebuf_format_t fmt =
         vid->frame_rgb32 ? (video_info->use_rgba ? PF_RGBA32 : PF_RGB32) :
         video_info->use_rgba ? PF_RGBA16_4444 : PF_RGB16_565;

   glClear(GL_COLOR_BUFFER_BIT);

   //if ((frame_count % 100) == 0)
   //   RARCH_LOG("[XPLAY] Frame %dx%d, pitch %d, rgb32: %d, rgba: %d, nonblock: %d, msg: %s\n", width, height, pitch, (int)vid->frame_rgb32, (int)video_info->use_rgba, (int)video_info->input_driver_nonblock_state, msg ? msg : "<no message>");

   int prev_frame_tex = vid->frame_tex_index;
   int next_frame_tex = (prev_frame_tex + 1) % XPLAY_FRAMEBUF_COUNT;
   vid->frame_tex_index = next_frame_tex;

   xplay_blit_framebuf(&vid->frame_tex[next_frame_tex], frame, width, height, pitch, fmt);

   xplay_framebuf_program_t *frame_prog = xplay_get_framebuf_program(vid, vid->frame_rgb32, width, height, integer_scale && want_grid);
   xplay_draw_framebuf(frame_prog, &vid->frame_tex[next_frame_tex], integer_scale, 1.f, vid->frame_rgb32);

#ifdef HAVE_MENU
   bool menu_is_alive = video_info->menu_is_alive;
   menu_driver_frame(menu_is_alive, video_info);
   if (menu_is_alive) {
      glEnable(GL_BLEND);
      xplay_framebuf_texture_t  *menu_tex = &vid->menu_tex[vid->menu_tex_index];
      xplay_framebuf_program_t *menu_prog = xplay_get_framebuf_program(vid, vid->menu_rgb32, menu_tex->width, menu_tex->height, false);
      xplay_draw_framebuf(menu_prog, menu_tex, integer_scale, vid->menu_alpha, vid->menu_rgb32);
      glDisable(GL_BLEND);
   }
#endif

   if (msg) {
      glEnable(GL_BLEND);
      xplay_render_msg(vid,
            msg, SCREEN_WIDTH, SCREEN_HEIGHT,
            video_info->font_msg_pos_x,
            video_info->font_msg_pos_y);
      glDisable(GL_BLEND);
   }

   if (vid->ctx_driver->swap_buffers)
      vid->ctx_driver->swap_buffers(vid->ctx_data);

   return true;
}

static void xplay_gfx_set_nonblock_state(
      void *data, bool state,
      bool adaptive_vsync_enabled,
      unsigned swap_interval)
{
   int interval = 0;
   xplay_video_t *vid = (xplay_video_t *)data;

   if (!state)
      interval = swap_interval;

   if (vid->ctx_driver->swap_interval)
   {
      if (adaptive_vsync_enabled && interval == 1)
         interval = -1;
      vid->ctx_driver->swap_interval(vid->ctx_data, interval);
   }
}

static bool xplay_gfx_alive(void *data)
{
   (void)data;
   return true;
}

static bool xplay_gfx_focus(void *data)
{
   xplay_video_t *vid = (xplay_video_t*)data;
   if (vid && vid->ctx_driver && vid->ctx_driver->has_focus)
      return vid->ctx_driver->has_focus(vid->ctx_data);
   return true;
}

static bool xplay_gfx_suppress_screensaver(void *data, bool enable)
{
   xplay_video_t *vid = (xplay_video_t *)data;
   if (vid->ctx_data && vid->ctx_driver->suppress_screensaver)
      return vid->ctx_driver->suppress_screensaver(vid->ctx_data, enable);
   return false;
}

/* TODO/FIXME - implement */
static bool xplay_gfx_has_windowed(void *data) {
   xplay_video_t *vid = (xplay_video_t *)data;
   if (vid && vid->ctx_driver)
      return vid->ctx_driver->has_windowed;
   return false;
}

static void xplay_gfx_viewport_info(void *data, struct video_viewport *vp)
{
   vp->x      = 0;
   vp->y      = 0;
   vp->width  = vp->full_width  = SCREEN_WIDTH;
   vp->height = vp->full_height = SCREEN_HEIGHT;
}

static void xplay_set_filtering(void *data, unsigned index, bool smooth, bool ctx_scaling)
{
   //xplay_video_t *vid = (xplay_video_t*)data;
}

static void xplay_apply_state_changes(void *data)
{
   (void)data;
}

static void xplay_set_texture_frame(void *data, const void *frame, bool rgb32,
      unsigned width, unsigned height, float alpha)
{
   xplay_video_t *vid = (xplay_video_t*)data;
   vid->menu_alpha = alpha;
   vid->menu_rgb32 = rgb32;
   //RARCH_LOG("[XPLAY] Menu %dx%d, alpha: %.2f, rgb32: %d\n", width, height, alpha, (int)rgb32);
   xplay_framebuf_format_t fmt = rgb32 ? (video_driver_supports_rgba() ? PF_RGBA32 : PF_RGB32) : PF_RGBA16_4444;
   int pixel_size = rgb32 ? 4 : 2;
   vid->menu_tex_index = (vid->menu_tex_index + 1) % XPLAY_FRAMEBUF_COUNT;
   xplay_blit_framebuf(&vid->menu_tex[vid->menu_tex_index], frame, width, height, width*pixel_size, fmt);
}


static void xplay_set_texture_enable(void *data, bool state, bool full_screen)
{
   xplay_video_t *vid = (xplay_video_t*)data;

   (void)full_screen;
}

static uint32_t xplay_get_flags(void *data)
{
   (void)data;
   uint32_t flags = 0;
   return flags;
}

static const video_poke_interface_t xplay_poke_interface = {
   xplay_get_flags,
   NULL,
   NULL,
   NULL,
   NULL, /* get_refresh_rate */
   xplay_set_filtering,
   NULL, /* get_video_output_size */
   NULL, /* get_video_output_prev */
   NULL, /* get_video_output_next */
   NULL, /* get_current_framebuffer */
   NULL, /* get_proc_address */
   NULL,
   xplay_apply_state_changes,
   xplay_set_texture_frame,
   xplay_set_texture_enable,
   NULL,
   NULL,
   NULL,
   NULL,                         /* get_current_shader */
   NULL,                         /* get_current_software_framebuffer */
   NULL,                         /* get_hw_render_interface */
   NULL,                         /* set_hdr_max_nits */
   NULL,                         /* set_hdr_paper_white_nits */
   NULL,                         /* set_hdr_contrast */
   NULL                          /* set_hdr_expand_gamut */
};

static void xplay_get_poke_interface(void *data, const video_poke_interface_t **iface)
{
   (void)data;
   *iface = &xplay_poke_interface;
}

video_driver_t video_xplay = {
   xplay_gfx_init,
   xplay_gfx_frame,
   xplay_gfx_set_nonblock_state,
   xplay_gfx_alive,
   xplay_gfx_focus,
   xplay_gfx_suppress_screensaver,
   xplay_gfx_has_windowed,
   NULL,
   xplay_gfx_free,
   "xplay",
   NULL,
   NULL, /* set_rotation */
   xplay_gfx_viewport_info,
   NULL, /* read_viewport  */
   NULL, /* read_frame_raw */
#ifdef HAVE_OVERLAY
   NULL,
#endif
#ifdef HAVE_VIDEO_LAYOUT
  NULL,
#endif
   xplay_get_poke_interface
};
