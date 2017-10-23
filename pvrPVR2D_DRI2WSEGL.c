#include <X11/Xlib.h>
#include <X11/Xproto.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/dri2proto.h>

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/shm.h>
#include <unistd.h>

#include "services.h"
#include "pvr2d.h"
#include "dri2.h"

typedef Window NativeWindowType;
typedef Display * NativeDisplayType;
typedef Drawable NativePixmapType;

#if 0
#define LOG(format, ...)   fprintf(stderr, "%s %d" format " \n", __FUNCTION__, __LINE__, ##__VA_ARGS__)
#else
#define LOG(...)
#endif


#include "wsegl.h"

typedef struct _wsegldri2_display wsegldri2_display;
struct _wsegldri2_display
{
  unsigned long ref_cnt;
  Display *dpy;
  Bool default_dpy;
  PVR2DCONTEXTHANDLE pvr_context;
  WSEGLConfig *configs;
};

typedef struct _wsegldri2_drawable wsegldri2_drawable;
struct _wsegldri2_drawable
{
  int drawable_type;
  NativePixmapType nativePixmap;
  PVR2DMEMINFO *pvr_meminfo;
  int name;
  void *shmaddr;
  int is_pixmap;
  unsigned int width;
  unsigned int height;
  int pixel_format;
  int stride;
  wsegldri2_display *display;
};


static wsegldri2_display wsegl_display;
static int bpp[] = {2, 2, 4};
static WSEGLCaps caps_hw_sync[] =
{
  {WSEGL_CAP_WINDOWS_USE_HW_SYNC, 1},
  {WSEGL_NO_CAPS, 0}
};

static WSEGLCaps caps_no_hw_sync[] =
{
  {WSEGL_NO_CAPS, 0}
};

static WSEGLError
WSEGLDRI2IsDisplayValid(NativeDisplayType nativeDisplay)
{
  Display *dpy = nativeDisplay;
  WSEGLError rv;
  LOG();
  if (!nativeDisplay)
  {
    nativeDisplay = XOpenDisplay(0);

    if (!nativeDisplay)
      return WSEGL_BAD_NATIVE_DISPLAY;
  }

  if (*DisplayString(nativeDisplay) == ':')
    rv = WSEGL_SUCCESS;
  else
    rv = WSEGL_BAD_NATIVE_DISPLAY;

  if (!dpy)
    XCloseDisplay(nativeDisplay);

  return rv;
}

static WSEGLError
WSEGLDRI2InitialiseDisplay(NativeDisplayType dpy, WSEGLDisplayHandle* handle,
                           const WSEGLCaps **caps, WSEGLConfig **configs)
{
  wsegldri2_display **display = (wsegldri2_display **)handle;
  WSEGLCaps *wsegldri2_caps;
  WSEGLError rv;
  int num_devs;
  PVR2DDEVICEINFO *dev_info;
  PVR2DDISPLAYINFO pDisplayInfo;
  unsigned int dev_id;
  int i;
  XVisualInfo *visuals;
  int minor;
  int major;
  int errorBase;
  int eventBase;
  void *state;
  int use_hw_sync;
  unsigned int pvDefault = 1;
  int num_visuals;
  LOG();

  PVRSRVCreateAppHintState(IMG_EGL, 0, &state);
  PVRSRVGetAppHint(state, "WSEGL_UseHWSync", IMG_UINT_TYPE, &pvDefault,
                   &use_hw_sync);
  PVRSRVFreeAppHintState(IMG_EGL, state);

  if (use_hw_sync)
    wsegldri2_caps = caps_hw_sync;
  else
    wsegldri2_caps = caps_no_hw_sync;

  wsegl_display.ref_cnt++;

  if (wsegl_display.ref_cnt != 1 && wsegl_display.ref_cnt < ULONG_MAX)
  {
    *display = &wsegl_display;
    *caps = wsegldri2_caps;
    *configs = wsegl_display.configs;
    return WSEGL_SUCCESS;
  }

  rv = WSEGL_CANNOT_INITIALISE;

  if (!dpy)
  {
    dpy = XOpenDisplay(WSEGL_DEFAULT_DISPLAY);

    if (!dpy)
      return rv;

    wsegl_display.default_dpy = WSEGL_TRUE;
  }
  else
    wsegl_display.default_dpy = WSEGL_FALSE;

  num_devs = PVR2DEnumerateDevices(NULL);

  if (num_devs <= 0)
    goto err;

  dev_info = (PVR2DDEVICEINFO *)malloc(24 * num_devs);

  if (!dev_info)
  {
    rv = WSEGL_OUT_OF_MEMORY;
    goto err;
  }

  if (PVR2DEnumerateDevices(dev_info))
  {
    free(dev_info);
    goto err;
  }

  dev_id = dev_info->ulDevID;
  free(dev_info);

  if (PVR2DCreateDeviceContext(dev_id, &wsegl_display.pvr_context, 0))
    goto err;

  wsegl_display.dpy = dpy;

  if (PVR2DGetDeviceInfo(wsegl_display.pvr_context, &pDisplayInfo))
    goto err;

  if(!DRI2QueryExtension(dpy, &eventBase, &errorBase))
    goto context_err;

  if(!DRI2QueryVersion(wsegl_display.dpy, &major, &minor))
    goto context_err;

  if (major != WSEGL_VERSION || minor != 0)
    goto context_err;

  visuals = XGetVisualInfo(dpy, 0, 0, &num_visuals);

  if (!visuals)
  {
    rv = WSEGL_BAD_NATIVE_DISPLAY;
    fputs("XGetVisualInfo() returned NULL!\n", stderr);
    goto context_err;
  }

  wsegl_display.configs =
      (WSEGLConfig *)calloc(num_visuals + 1, sizeof(WSEGLConfig));

  if (!wsegl_display.configs)
  {
    XFree(visuals);
    rv = WSEGL_OUT_OF_MEMORY;
    goto context_err;
  }

  for (i = 0; i < num_visuals; i++)
  {
    WSEGLConfig *config = &wsegl_display.configs[i];
    XVisualInfo *visual = &visuals[i];

    switch (visual->depth)
    {
      case 24:
      case 32:
          config->ePixelFormat = WSEGL_PIXELFORMAT_8888;
          break;
      case 16:
        config->ePixelFormat = WSEGL_PIXELFORMAT_565;
        break;
      default:
        continue;
    }

    config->ui32DrawableType = WSEGL_DRAWABLE_WINDOW | WSEGL_DRAWABLE_PIXMAP;
    config->ulNativeRenderable = WSEGL_TRUE;
    config->ulNativeVisualID = visual->visualid;
  }

  XFree(visuals);
  wsegl_display.ref_cnt = 1;
  *caps = wsegldri2_caps;
  *configs = wsegl_display.configs;
  *display = &wsegl_display;

  return WSEGL_SUCCESS;

context_err:
  PVR2DDestroyDeviceContext(wsegl_display.pvr_context);

err:
  if (wsegl_display.default_dpy == WSEGL_TRUE)
    XCloseDisplay(dpy);

  return rv;
}

static WSEGLError
WSEGLDRI2CloseDisplay(WSEGLDisplayHandle handle)
{
  wsegldri2_display *wsegl_dpy = (wsegldri2_display *)handle;
  LOG();

  if (wsegl_dpy->ref_cnt-- == 1)
  {
    PVR2DDestroyDeviceContext(wsegl_dpy->pvr_context);

    if (wsegl_dpy->default_dpy)
      XCloseDisplay(wsegl_dpy->dpy);

    free(wsegl_display.configs);
  }

  return WSEGL_SUCCESS;
}

static WSEGLError
WSEGLDRI2GetDrawableInfo(wsegldri2_display *display, WSEGLConfig *config,
                         WSEGLDrawableHandle *drawable,
                         NativePixmapType nativePixmap,
                         WSEGLRotationAngle *rotationAngle, int drawable_type)
{
  WSEGLError rv;
  wsegldri2_drawable *handle;
  Window window;
  int tmp;
  unsigned int depth;
  unsigned int border_width;
  Status status;

  LOG();

  if (!nativePixmap)
  {
    if (drawable_type == WSEGL_DRAWABLE_WINDOW)
      return WSEGL_BAD_NATIVE_WINDOW;
    else
      return WSEGL_BAD_NATIVE_PIXMAP;
  }

  if (!config || !(drawable_type & config->ui32DrawableType) )
    return WSEGL_BAD_CONFIG;

  handle = (wsegldri2_drawable *)calloc(1, sizeof(*handle));

  if (!handle)
    return WSEGL_OUT_OF_MEMORY;

  handle->drawable_type = drawable_type;
  handle->display = display;
  handle->nativePixmap = nativePixmap;

  if ((status =
       XGetGeometry(display->dpy, nativePixmap, &window, &tmp, &tmp,
                    &handle->width, &handle->height, &border_width, &depth)))
  {
    Bool is_supported;

    if (depth == 24 || depth == 32)
      is_supported = config->ePixelFormat == WSEGL_PIXELFORMAT_8888;
    else
    {
      if (depth != 16)
      {
        rv = WSEGL_BAD_DRAWABLE;
        goto err;
      }

      is_supported = config->ePixelFormat == WSEGL_PIXELFORMAT_565;
    }

    if (is_supported)
    {
      handle->pixel_format = config->ePixelFormat;
      handle->stride = (handle->width + 0x1F) & ~0x1Fu;
      *drawable = handle;
      *rotationAngle = WSEGL_ROTATE_0;

      DRI2CreateDrawable(display->dpy, nativePixmap);

      return WSEGL_SUCCESS;
    }

    rv = WSEGL_BAD_CONFIG;
  }
  else
  {
    rv = WSEGL_BAD_DRAWABLE;
    fprintf(stderr,
            "WSEGLDRI2GetDrawableInfo: Failed to get X drawable geometry - status %d\n",
            status);
    fputs("WSEGL_CreateWindowDrawable: Bad drawable\n", stderr);
  }

err:

  free(handle);

  return rv;
}

static WSEGLError
WSEGLDRI2CreateWindowDrawable(WSEGLDisplayHandle display, WSEGLConfig *config,
                              WSEGLDrawableHandle *drawable,
                              NativeWindowType nativeWindow,
                              WSEGLRotationAngle *rotationAngle)
{
  LOG();

  return WSEGLDRI2GetDrawableInfo((wsegldri2_display *)display, config,
                                  drawable, nativeWindow, rotationAngle,
                                  WSEGL_DRAWABLE_WINDOW);
}

static WSEGLError
WSEGLDRI2CreatePixmapDrawable(WSEGLDisplayHandle display, WSEGLConfig *config,
                              WSEGLDrawableHandle *drawable,
                              NativePixmapType nativePixmap,
                              WSEGLRotationAngle *rotationAngle)
{
  LOG();

  return WSEGLDRI2GetDrawableInfo((wsegldri2_display *)display, config,
                                  drawable, nativePixmap, rotationAngle,
                                  WSEGL_DRAWABLE_PIXMAP);
}

static void
WSEGLDRI2FreeSharedMemory(wsegldri2_drawable *drawable)
{
  PVR2DQueryBlitsComplete(
        drawable->display->pvr_context, drawable->pvr_meminfo, PVR2D_TRUE);
  PVR2DMemFree(drawable->display->pvr_context, drawable->pvr_meminfo);
  shmdt(drawable->shmaddr);
}

static
WSEGLError WSEGLDRI2DeleteDrawable(WSEGLDrawableHandle handle)
{
  wsegldri2_drawable *drawable = (wsegldri2_drawable *)handle;
  LOG();

  DRI2DestroyDrawable(drawable->display->dpy, drawable->nativePixmap);

  if (drawable->pvr_meminfo)
    WSEGLDRI2FreeSharedMemory(drawable);

  free(drawable);

  return WSEGL_SUCCESS;
}

static WSEGLError
WSEGLDRI2SwapDrawable(WSEGLDrawableHandle handle, unsigned long data)
{
  wsegldri2_drawable *drawable = (wsegldri2_drawable *)handle;
  LOG();

  XserverRegion region;
  XRectangle rectangle;

  rectangle.width = drawable->width;
  rectangle.height = drawable->height;
  rectangle.x = 0;
  rectangle.y = 0;
  region = XFixesCreateRegion(drawable->display->dpy, &rectangle, 1);

  DRI2CopyRegion(drawable->display->dpy, drawable->nativePixmap, region, 0, 1);
  XFixesDestroyRegion(drawable->display->dpy, region);
  drawable->is_pixmap = False;

  return WSEGL_SUCCESS;
}

static WSEGLError
WSEGLDRI2SwapControlInterval()
{
  return WSEGL_SUCCESS;
}

static WSEGLError
WSEGLDRI2WaitNative(WSEGLDrawableHandle handle, unsigned long engine)
{
  wsegldri2_drawable *drawable = (wsegldri2_drawable *)handle;
  LOG();

  if (engine != WSEGL_DEFAULT_NATIVE_ENGINE)
    return WSEGL_BAD_NATIVE_ENGINE;

  XSync(drawable->display->dpy, 0);

  return WSEGL_SUCCESS;
}

static WSEGLError
WSEGLDRI2CopyFromDrawable(WSEGLDrawableHandle handle,
                          NativePixmapType nativePixmap)
{
  wsegldri2_drawable *drawable = (wsegldri2_drawable *)handle;
  wsegldri2_display *display = drawable->display;
  int bytes_per_pixel;
  int bits_per_pixel;
  unsigned int red_mask;
  unsigned int green_mask;
  unsigned int blue_mask;
  GC gc;
  XImage image;
  Window window;
  int tmp1;
  unsigned int tmp2;
  unsigned int depth;
  LOG();

  memset(&image, 0, sizeof(image));
  bytes_per_pixel = bpp[drawable->pixel_format];

  if (!XGetGeometry(display->dpy, nativePixmap, &window, &tmp1, &tmp1, &tmp2,
                     &tmp2, &tmp2, &depth))
  {
    return WSEGL_BAD_CONFIG;
  }

  bits_per_pixel = 8 * bytes_per_pixel;

  if (bits_per_pixel != depth )
    return WSEGL_BAD_CONFIG;

  switch (drawable->pixel_format)
  {
    case WSEGL_PIXELFORMAT_4444:
    {
      red_mask = 0xF00;
      green_mask = 0xF0;
      blue_mask = 0xF;
      break;
    }
    case WSEGL_PIXELFORMAT_8888:
    {
      red_mask = 0xFF0000;
      green_mask = 0xFF00;
      blue_mask = 0xFF;
      break;
    }
    default:
    {
      red_mask = 0xF800;
      green_mask = 0x7E0;
      blue_mask = 0x1F;
      break;
    }
  }

  image.red_mask = red_mask;
  image.green_mask = green_mask;
  image.blue_mask = blue_mask;
  image.width = drawable->width;
  image.height = drawable->height;
  image.format = ZPixmap;
  image.bytes_per_line = drawable->stride * bpp[drawable->pixel_format];
  image.data = (char *)drawable->pvr_meminfo;
  image.bitmap_pad = bits_per_pixel;
  image.depth = bits_per_pixel;
  image.bits_per_pixel = bits_per_pixel;
  image.bitmap_unit = bits_per_pixel;

  XInitImage(&image);
  gc = XCreateGC(display->dpy, nativePixmap, 0, 0);
  XPutImage(display->dpy, nativePixmap, gc, &image, 0, 0, 0, 0,
            drawable->width, drawable->height);
  XFreeGC(display->dpy, gc);

  return WSEGL_SUCCESS;
}

static WSEGLError
WSEGLDRI2CopyFromPBuffer(void *address, unsigned long width,
                         unsigned long height, unsigned long stride,
                         WSEGLPixelFormat format, NativePixmapType nativePixmap)
{
  int bytes_per_pixel;
  int bytes_per_line;
  int bits_per_pixel;
  GC gc;
  unsigned long red_mask;
  unsigned long green_mask;
  unsigned long blue_mask;
  XImage image;
  LOG();

  memset(&image, 0, sizeof(image));

  bytes_per_pixel = bpp[format];
  bytes_per_line = stride * bytes_per_pixel;
  bits_per_pixel = 8 * bytes_per_pixel;

  gc = XCreateGC(wsegl_display.dpy, nativePixmap, 0, 0);

  switch (format)
  {
    case WSEGL_PIXELFORMAT_4444:
    {
      red_mask = 0xF00;
      green_mask = 0xF0;
      blue_mask = 0xF;
      break;
    }
    case WSEGL_PIXELFORMAT_8888:
    {
      red_mask = 0xFF0000;
      green_mask = 0xFF00;
      blue_mask = 0xFF;
      break;
    }
    default:
    {
      red_mask = 0xF800;
      green_mask = 0x7E0;
      blue_mask = 0x1F;
      break;
    }
  }

  image.red_mask = red_mask;
  image.green_mask = green_mask;
  image.blue_mask = blue_mask;
  image.depth = bits_per_pixel;
  image.bitmap_pad = bits_per_pixel;
  image.bitmap_unit = bits_per_pixel;
  image.bits_per_pixel = bits_per_pixel;
  image.data = (char *)address + (height - 1) * bytes_per_line;
  image.bytes_per_line = bytes_per_line;
  image.width = width;
  image.height = height;
  image.format = ZPixmap;
  XInitImage(&image);

  image.bytes_per_line = -image.bytes_per_line;

  XPutImage(wsegl_display.dpy, nativePixmap, gc, &image, 0, 0, 0, 0,
            width, height);
  XFreeGC(wsegl_display.dpy, gc);

  return WSEGL_SUCCESS;
}

static WSEGLError
WSEGLDRI2GetDrawableParameters(WSEGLDrawableHandle handle,
                               WSEGLDrawableParams *sourceParams,
                               WSEGLDrawableParams *renderParams)
{
  wsegldri2_drawable *drawable = (wsegldri2_drawable *)handle;
  int count;
  DRI2Buffer *buffer;
  WSEGLError rv;
  PVR2DMEMINFO *pvr_meminfo;
  unsigned long size;
  unsigned int attachments[2];
  int outCount;
  int height;
  int width;

  LOG();

  if (drawable->drawable_type == WSEGL_DRAWABLE_WINDOW)
  {
    attachments[0] = WSEGL_DRAWABLE_WINDOW;
    attachments[1] = 0;
    count = 2;
  }
  else
  {
    attachments[0] = 0;
    count = 1;
  }

  buffer = DRI2GetBuffers(drawable->display->dpy,
                          drawable->nativePixmap,
                          &width,
                          &height,
                          attachments,
                          count,
                          &outCount);

  if ( !buffer )
    return WSEGL_OUT_OF_MEMORY;

  if (drawable->is_pixmap)
  {
    free(buffer);

ok:
    renderParams->ui32Width = drawable->width;
    renderParams->ui32Height = drawable->height;
    renderParams->ePixelFormat = drawable->pixel_format;
    renderParams->ui32Stride = drawable->stride;
    renderParams->pvLinearAddress = drawable->pvr_meminfo->pBase;
    renderParams->ui32HWAddress = drawable->pvr_meminfo->ui32DevAddr;
    renderParams->hPrivateData = drawable->pvr_meminfo->hPrivateData;

    sourceParams->ui32Width = renderParams->ui32Width;
    sourceParams->ui32Height = renderParams->ui32Height;
    sourceParams->ui32Stride = renderParams->ui32Stride;
    sourceParams->ePixelFormat = renderParams->ePixelFormat;
    sourceParams->pvLinearAddress = renderParams->pvLinearAddress;
    sourceParams->ui32HWAddress = renderParams->ui32HWAddress;
    sourceParams->hPrivateData = renderParams->hPrivateData;

    return WSEGL_SUCCESS;
  }

  if (outCount != count || width != drawable->width ||
      height != drawable->height)
  {
    free(buffer);
    return WSEGL_BAD_DRAWABLE;
  }

  pvr_meminfo = drawable->pvr_meminfo;
  drawable->stride = buffer->pitch/ buffer->cpp;

  if ( !pvr_meminfo || drawable->name != buffer->name )
  {
    size = buffer->pitch * height;

    if (!size)
    {
      free(buffer);
      return WSEGL_BAD_DRAWABLE;
    }

    if ( drawable->name != -1 && pvr_meminfo )
    {
      WSEGLDRI2FreeSharedMemory(drawable);
      drawable->shmaddr = NULL;
      drawable->pvr_meminfo = NULL;
    }

    drawable->name = buffer->name;

    if (buffer->name == -1)
    {
      if (PVR2DGetFrameBuffer(drawable->display->pvr_context, 0,
                              &drawable->pvr_meminfo))
      {
        rv = WSEGL_OUT_OF_MEMORY;
        goto err;
      }
    }
    else
    {
      unsigned long flags;
      int pagesize;

      drawable->shmaddr = shmat(buffer->name, 0, 0);

      if (!drawable->shmaddr)
      {
        free(buffer);
        return WSEGL_OUT_OF_MEMORY;
      }

      pagesize = getpagesize();
      flags = (size + pagesize - 1) / pagesize;

      if (PVR2DMemWrap(drawable->display->pvr_context, drawable->shmaddr,
                       flags == 1, size, NULL, &drawable->pvr_meminfo))
      {
        drawable->pvr_meminfo = NULL;
        rv = WSEGL_OUT_OF_MEMORY;
        goto err;
      }
    }
  }

  rv = WSEGL_SUCCESS;

err:
  free(buffer);

  if (drawable->drawable_type == WSEGL_DRAWABLE_PIXMAP)
    drawable->is_pixmap = WSEGL_TRUE;

  if ( rv == WSEGL_SUCCESS )
    goto ok;

  return rv;
}

static WSEGL_FunctionTable const wseglFunctions = {
  WSEGL_VERSION,
  WSEGLDRI2IsDisplayValid,
  WSEGLDRI2InitialiseDisplay,
  WSEGLDRI2CloseDisplay,
  WSEGLDRI2CreateWindowDrawable,
  WSEGLDRI2CreatePixmapDrawable,
  WSEGLDRI2DeleteDrawable,
  WSEGLDRI2SwapDrawable,
  WSEGLDRI2SwapControlInterval,
  WSEGLDRI2WaitNative,
  WSEGLDRI2CopyFromDrawable,
  WSEGLDRI2CopyFromPBuffer,
  WSEGLDRI2GetDrawableParameters
};

/* Return the table of WSEGL functions to the EGL implementation */
const WSEGL_FunctionTable *
WSEGL_GetFunctionTablePointer(void)
{
    return &wseglFunctions;
}
