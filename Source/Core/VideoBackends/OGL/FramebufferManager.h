// Copyright 2009 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/GL/GLUtil.h"
#include "VideoBackends/OGL/ProgramShaderCache.h"
#include "VideoBackends/OGL/Render.h"
#include "VideoCommon/FramebufferManagerBase.h"

// On the GameCube, the game sends a request for the graphics processor to
// transfer its internal EFB (Embedded Framebuffer) to an area in GameCube RAM
// called the XFB (External Framebuffer). The size and location of the XFB is
// decided at the time of the copy, and the format is always YUYV. The video
// interface is given a pointer to the XFB, which will be decoded and
// displayed on the TV.
//
// There are two ways for Dolphin to emulate this:
//
// Real XFB mode:
//
// Dolphin will behave like the GameCube and encode the EFB to
// a portion of GameCube RAM. The emulated video interface will decode the data
// for output to the screen.
//
// Advantages: Behaves exactly like the GameCube.
// Disadvantages: Resolution will be limited.
//
// Virtual XFB mode:
//
// When a request is made to copy the EFB to an XFB, Dolphin
// will remember the RAM location and size of the XFB in a Virtual XFB list.
// The video interface will look up the XFB in the list and use the enhanced
// data stored there, if available.
//
// Advantages: Enables high resolution graphics, better than real hardware.
// Disadvantages: If the GameCube CPU writes directly to the XFB (which is
// possible but uncommon), the Virtual XFB will not capture this information.

// There may be multiple XFBs in GameCube RAM. This is the maximum number to
// virtualize.

namespace OGL
{
class FramebufferManager : public FramebufferManagerBase
{
public:
  FramebufferManager(int targetWidth, int targetHeight, int msaaSamples,
                     bool enable_stencil_buffer);
  ~FramebufferManager();

  // TODO: This should be removed as all it does is make using the
  //       global framebuffer manager instance nicer. This will be
  //       able to be removed when the globals are eliminated and replaced
  //       with per-backend members that use the proper type, eliminating
  //       the need for functions to downcast to the correct type.
  static FramebufferManager* GetInstance();

  // To get the EFB in texture form, these functions may have to transfer
  // the EFB to a resolved texture first.
  GLuint GetEFBColorTexture(const EFBRectangle& sourceRc);
  GLuint GetEFBDepthTexture(const EFBRectangle& sourceRc);
  void ResolveEFBStencilTexture();

  GLuint GetEFBFramebuffer(unsigned int layer = 0) const
  {
    return (layer < m_EFBLayers) ? m_efbFramebuffer[layer] : m_efbFramebuffer.back();
  }
  // Resolved framebuffer is only used in MSAA mode.
  GLuint GetResolvedFramebuffer() const;
  void SetFramebuffer(GLuint fb);
  void FramebufferTexture(GLenum target, GLenum attachment, GLenum textarget, GLuint texture,
                          GLint level);

  // If in MSAA mode, this will perform a resolve of the specified rectangle, and return the resolve
  // target as a texture ID.
  // Thus, this call may be expensive. Don't repeat it unnecessarily.
  // If not in MSAA mode, will just return the render target texture ID.
  // After calling this, before you render anything else, you MUST bind the framebuffer you want to
  // draw to.
  GLuint ResolveAndGetRenderTarget(const EFBRectangle& source_rect);

  // Same as above but for the depth Target.
  // After calling this, before you render anything else, you MUST bind the framebuffer you want to
  // draw to.
  GLuint ResolveAndGetDepthTarget(const EFBRectangle& source_rect);

  // Convert EFB content on pixel format change.
  // convtype=0 -> rgb8->rgba6, convtype=2 -> rgba6->rgb8
  void ReinterpretPixelData(unsigned int convtype);

  void PokeEFB(EFBAccessType type, const EfbPokeData* points, size_t num_points);
  bool HasStencilBuffer() const;

private:
  GLuint CreateTexture(GLenum texture_type, GLenum internal_format, GLenum pixel_format,
                       GLenum data_type) const;
  void BindLayeredTexture(GLuint texture, const std::vector<GLuint>& framebuffers,
                          GLenum attachment, GLenum texture_type);

  int m_targetWidth = 0;
  int m_targetHeight = 0;
  int m_msaaSamples = 0;

  GLenum m_textureType = 0;
  std::vector<GLuint> m_efbFramebuffer;
  GLuint m_efbColor = 0;
  GLuint m_efbDepth = 0;
  // will be hot swapped with m_efbColor when reinterpreting EFB pixel formats
  GLuint m_efbColorSwap = 0;

  bool m_enable_stencil_buffer = false;

  // Only used in MSAA mode, TODO: try to avoid them
  std::vector<GLuint> m_resolvedFramebuffer;
  GLuint m_resolvedColorTexture = 0;
  GLuint m_resolvedDepthTexture = 0;

  // For pixel format draw
  SHADER m_pixel_format_shaders[2];

  // For EFB pokes
  GLuint m_EfbPokes_VBO = 0;
  GLuint m_EfbPokes_VAO = 0;
  SHADER m_EfbPokes;
};

}  // namespace OGL
