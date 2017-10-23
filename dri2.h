#ifndef _DRI2_H_
#define _DRI2_H_

typedef struct
{
   unsigned int attachment;
   unsigned int name;
   unsigned int pitch;
   unsigned int cpp;
   unsigned int flags;
} DRI2Buffer;

void DRI2DestroyDrawable(Display *dpy, XID drawable);
void DRI2CopyRegion(Display * dpy, XID drawable, XserverRegion region, CARD32 dest, CARD32 src);
void DRI2CreateDrawable(Display * dpy, XID drawable);
Bool DRI2QueryExtension(Display * dpy, int *eventBase, int *errorBase);
Bool DRI2QueryVersion(Display * dpy, int *major, int *minor);
DRI2Buffer *DRI2GetBuffers(Display * dpy, XID drawable, int *width, int *height, unsigned int *attachments, int count, int *outCount);
#endif
