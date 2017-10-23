#include <stdint.h>
#include <X11/Xlibint.h>
#include <X11/Xproto.h>
#include <X11/extensions/extutil.h>
#include <X11/extensions/dri2proto.h>
#include <X11/extensions/Xfixes.h>
#include <drm.h>

#include "dri2.h"

static char dri2ExtensionName[] = DRI2_NAME;

static XExtensionInfo *dri2Info = NULL;

static XEXT_GENERATE_CLOSE_DISPLAY(DRI2CloseDisplay, dri2Info);

static Bool
DRI2Error(Display *dpy, xError *err, XExtCodes *codes, int *ret_code)
{
  *ret_code = 0;

  if (err->majorCode == codes->major_opcode &&
      err->minorCode == X_DRI2DestroyDrawable &&
      err->errorCode == BadDrawable)
  {
    return True;
  }

  return False;
}

static XExtensionHooks dri2ExtensionHooks = {
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  DRI2CloseDisplay,
  NULL,
  NULL,
  DRI2Error,
  NULL
};

static XEXT_GENERATE_FIND_DISPLAY(DRI2FindDisplay,
                                  dri2Info,
                                  dri2ExtensionName,
                                  &dri2ExtensionHooks,
                                  0, NULL);

void
DRI2DestroyDrawable(Display *dpy, XID drawable)
{

  XExtDisplayInfo *info = DRI2FindDisplay(dpy);
  xDRI2DestroyDrawableReq *req;

  XextSimpleCheckExtension(dpy, info, dri2ExtensionName);

  XSync(dpy, False);

  LockDisplay(dpy);
  GetReq(DRI2DestroyDrawable, req);
  req->reqType = info->codes->major_opcode;
  req->dri2ReqType = X_DRI2DestroyDrawable;
  req->drawable = drawable;
  UnlockDisplay(dpy);

  SyncHandle();
}

Bool
DRI2QueryExtension(Display * dpy, int *eventBase, int *errorBase)
{
   XExtDisplayInfo *info = DRI2FindDisplay(dpy);

   if (XextHasExtension(info))
   {
      *eventBase = info->codes->first_event;
      *errorBase = info->codes->first_error;
      return True;
   }

   return False;
}

void
DRI2CopyRegion(Display * dpy, XID drawable, XserverRegion region,
               CARD32 dest, CARD32 src)
{
   XExtDisplayInfo *info = DRI2FindDisplay(dpy);
   xDRI2CopyRegionReq *req;
   xDRI2CopyRegionReply rep;

   XextSimpleCheckExtension(dpy, info, dri2ExtensionName);

   LockDisplay(dpy);
   GetReq(DRI2CopyRegion, req);
   req->reqType = info->codes->major_opcode;
   req->dri2ReqType = X_DRI2CopyRegion;
   req->drawable = drawable;
   req->region = region;
   req->dest = dest;
   req->src = src;

   _XReply(dpy, (xReply *) & rep, 0, xFalse);

   UnlockDisplay(dpy);
   SyncHandle();
}

void
DRI2CreateDrawable(Display * dpy, XID drawable)
{
   XExtDisplayInfo *info = DRI2FindDisplay(dpy);
   xDRI2CreateDrawableReq *req;

   XextSimpleCheckExtension(dpy, info, dri2ExtensionName);

   LockDisplay(dpy);
   GetReq(DRI2CreateDrawable, req);
   req->reqType = info->codes->major_opcode;
   req->dri2ReqType = X_DRI2CreateDrawable;
   req->drawable = drawable;
   UnlockDisplay(dpy);
   SyncHandle();
}

Bool
DRI2QueryVersion(Display * dpy, int *major, int *minor)
{
   XExtDisplayInfo *info = DRI2FindDisplay(dpy);
   xDRI2QueryVersionReply rep;
   xDRI2QueryVersionReq *req;

   XextCheckExtension(dpy, info, dri2ExtensionName, False);

   LockDisplay(dpy);
   GetReq(DRI2QueryVersion, req);
   req->reqType = info->codes->major_opcode;
   req->dri2ReqType = X_DRI2QueryVersion;
   req->majorVersion = DRI2_MAJOR;
   req->minorVersion = DRI2_MINOR;

   if (!_XReply(dpy, (xReply *) & rep, 0, xFalse)) {
      UnlockDisplay(dpy);
      SyncHandle();
      return False;
   }

   *major = rep.majorVersion;
   *minor = rep.minorVersion;
   UnlockDisplay(dpy);
   SyncHandle();

   return True;
}

DRI2Buffer *
DRI2GetBuffers(Display * dpy, XID drawable,
               int *width, int *height,
               unsigned int *attachments, int count, int *outCount)
{
   XExtDisplayInfo *info = DRI2FindDisplay(dpy);
   xDRI2GetBuffersReply rep;
   xDRI2GetBuffersReq *req;
   DRI2Buffer *buffers;
   xDRI2Buffer repBuffer;
   CARD32 *p;
   int i;

   XextCheckExtension(dpy, info, dri2ExtensionName, False);

   LockDisplay(dpy);
   GetReqExtra(DRI2GetBuffers, count * 4, req);
   req->reqType = info->codes->major_opcode;
   req->dri2ReqType = X_DRI2GetBuffers;
   req->drawable = drawable;
   req->count = count;
   p = (CARD32 *) & req[1];
   for (i = 0; i < count; i++)
      p[i] = attachments[i];

   if (!_XReply(dpy, (xReply *) & rep, 0, xFalse)) {
      UnlockDisplay(dpy);
      SyncHandle();
      return NULL;
   }

   *width = rep.width;
   *height = rep.height;
   *outCount = rep.count;

   buffers = calloc(rep.count, sizeof buffers[0]);
   for (i = 0; i < rep.count; i++) {
      _XReadPad(dpy, (char *) &repBuffer, sizeof repBuffer);
      if (buffers) {
         buffers[i].attachment = repBuffer.attachment;
         buffers[i].name = repBuffer.name;
         buffers[i].pitch = repBuffer.pitch;
         buffers[i].cpp = repBuffer.cpp;
         buffers[i].flags = repBuffer.flags;
      }
   }

   UnlockDisplay(dpy);
   SyncHandle();

   return buffers;
}
