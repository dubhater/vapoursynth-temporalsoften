#include <stdlib.h>
#include <vapoursynth/VapourSynth.h>

// Computes the sum of absolute differences between plane1 and plane2.
int64_t there_is_only_c_scenechange(const uint8_t* plane1, const uint8_t* plane2, int height, int width, int stride1, int stride2) {
   int wp = (width / 32) * 32;
   int hp = height;

   int x, y;
   int64_t sum = 0;
   for (y = 0; y < hp; y++) {
      for (x = 0; x < wp; x++) {
         int diff = abs(plane2[x + y * stride2] - plane1[x + y * stride1]);
         sum += diff;
      }
   }
   return sum;
}


// Saturated addition. Shamelessly stolen from stackoverflow.
uint8_t sadd8(uint8_t a, uint8_t b) {
   return (a > 0xFF - b) ? 0xFF : a + b;
}

uint16_t sadd16(uint16_t a, uint16_t b) {
   return (a > 0xFFFF - b) ? 0xFFFF : a + b;
}

uint32_t sadd32(uint32_t a, uint32_t b) {
   return (a > 0xFFFFFFFF - b) ? 0xFFFFFFFF : a + b;
}

// Saturated subtraction.
uint8_t ssub8(uint8_t a, uint8_t b) {
   return (b > a) ? 0 : a - b;
}

// This function processes only one line. The dstp and srcp pointers are modified externally to point to the "current" line.
// I'm not sure my translation of this function would do the right thing to rgb data. [...] I think it would.
void there_is_only_c_accumulate_line_mode2(uint8_t* dstp, const uint8_t** srcp, int planes, int width, int threshold, int div) {
   // dstp: pointer to the destination line. This gets "softened".
   // srcp: array of pointers to the source lines.
   // planes: the number of elements in the srcp array.
   // width: width of a line.
   // threshold: the luma or chroma threshold (whichever we're working on).
   // div: seems to be 32768/(planes+1). Must be magic.


   // loop over the pixels in dstp
   // eax -> i
   for (int i = 0; i < width; i++) { // testplane loop
      // load 8 pixels from (dstp + i) into mm0
      uint8_t dstp_pixel8 = dstp[i];
      // unpack the lower 4 and interleave with zeroes in mm6
      // unpack the higher 4 and interleave with zeroes in mm7
      uint16_t dstp_pixel16 = dstp_pixel8;

      // For some reason it wants to start with the last frame.
      for (int j = planes - 1; j >= 0; j--) { // kernel_loop
         const uint8_t* current_plane = srcp[j];
         // load 8 pixels from (current_plane + i) into mm1
         uint8_t current_plane_pixel = current_plane[i];

         // subtract current_plane pixels from dstp pixels, saturate the result (0-255)
         // subtract dstp pixels from current_plane pixels, saturate the result (0-255)
         // bitwise OR the two results.
         // in other words, absolute difference.
         uint8_t absolute = abs(dstp_pixel8 - current_plane_pixel);
         //uint8_t absolute = ssub8(dstp_pixel8, current_plane_pixel)
         //                 | ssub8(current_plane_pixel, dstp_pixel8);

         // subtract threshold from the absolute difference and saturate the result
         // threshold fits in uint8_t.
         uint8_t absolute_minus_threshold = ssub8(absolute, (uint8_t)threshold);
         absolute_minus_threshold = (absolute_minus_threshold == 0) ? 255 : 0;
         // current_plane_pixel & absolute_minus_threshold
         // dstp_pixel & ~absolute_minus_threshold
         // bitwise OR the two results above
         uint16_t bitwise_or = (current_plane_pixel & absolute_minus_threshold)
                             | (dstp_pixel8 & ~absolute_minus_threshold);
         // unpack the lower 4 and interleave with zeroes
         // add to mm6, saturate the result (0-65535)
         // unpack the higher 4 and interleave with zeroes
         // add to mm7, saturate the result (0-65535)
         dstp_pixel16 = sadd16(dstp_pixel16, bitwise_or);
      }
      int32_t dstp_pixel32 = dstp_pixel16;
      dstp_pixel32 = dstp_pixel32 * div;
      dstp_pixel32 = dstp_pixel32 + 16384; // half the bits of add64
      dstp_pixel32 = dstp_pixel32 >> 15;
      /* Skip this.
       *
      // Clamp dstp_pixel32 to signed 16 bits.
      if (dstp_pixel32 > 32767) {
         dstp_pixel32 = 32767;
      } else if (dstp_pixel32 < -32768) {
         dstp_pixel32 = -32768;
      }
      */
      // Clamp the result to unsigned 8 bits.
      if (dstp_pixel32 > 255) {
         dstp_pixel32 = 255;
      } else if (dstp_pixel32 < 0) {
         dstp_pixel32 = 0;
      }
      dstp[i] = (uint8_t)dstp_pixel32;
   }
}


enum yuv_planes {
   Y = 0,
   U,
   V
};


typedef struct {
   const VSNodeRef *node;
   const VSVideoInfo *vi;

   // Filter parameters.
   int radius;
   int luma_threshold;
   int chroma_threshold;
   int scenechange;
   int mode;
} TemporalSoftenData;


static void VS_CC temporalSoftenInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
   TemporalSoftenData *d = (TemporalSoftenData *) * instanceData;
   vsapi->setVideoInfo(d->vi, node);

   d->scenechange *= ((d->vi->width/32)*32) * d->vi->height;
}


static inline int min(int a, int b) {
   return (((a) < (b)) ? (a) : (b));
}


static inline int max(int a, int b) {
   return (((a) > (b)) ? (a) : (b));
}


static const VSFrameRef *VS_CC temporalSoftenGetFrame(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
   TemporalSoftenData *d = (TemporalSoftenData *) * instanceData;

   if (activationReason == arInitial) {
      // Avoid requesting the first and last frames several times.
      int first = n - d->radius;
      if (first < 0)
         first = 0;

      int last = n + d->radius;
      if (last > d->vi->numFrames - 1)
         last = d->vi->numFrames - 1;

      // Request the frames.
      for (int i = first; i <= last; i++) {
         vsapi->requestFrameFilter(i, d->node, frameCtx);
      }
   } else if (activationReason == arAllFramesReady) {
      // Not sure why 16... the most we can have is 7*2+1
      const VSFrameRef* src[16];
      // Get the frames.
      for (int i = n - d->radius; i <= n + d->radius; i++) {
         src[i - n + d->radius] = vsapi->getFrameFilter(min(d->vi->numFrames - 1, max(i, 0)), d->node, frameCtx);
      }
      
      // Used to be bool, but we don't have bool in C.
      int planeDisabled[16] = { 0 };

      // src[radius] contains frame number n. Copy it to dst.
      VSFrameRef *dst = vsapi->copyFrame(src[d->radius], core);

      const VSFormat *fi = d->vi->format;


      // It's processing loop time!
      // Loop over all the planes
      int plane;
      for (plane = 0; plane < fi->numPlanes; plane++) {
         if (plane == 0 && d->luma_threshold == 0) {
            // Skip the luma plane if luma_threshold is 0.
            continue;
         }
         if (plane == 1 && d->chroma_threshold == 0) {
            // Skip the chroma planes if chroma_threshold is 0.
            break;
         }
         // ^ I like this better than the "planes[c++]" stuff in the original.

         int current_threshold = (plane == 0) ? d->luma_threshold : d->chroma_threshold;
         int dd = 0;
         int src_stride[16];
         int src_stride_trimmed[16];
         const uint8_t *srcp[16];
         const uint8_t *srcp_trimmed[16];

         // Get the plane pointers and strides.
         for (int i = 0; i < d->radius; i++) {
            src_stride[dd] = vsapi->getStride(src[i], plane);
            srcp[dd] = vsapi->getReadPtr(src[i], plane);
            dd++;
         }
         for (int i = 1; i <= d->radius; i++) {
            src_stride[dd] = vsapi->getStride(src[d->radius + i], plane);
            srcp[dd] = vsapi->getReadPtr(src[d->radius + i], plane);
            dd++;
         }
         int dst_stride = vsapi->getStride(dst, plane);
         uint8_t *dstp = vsapi->getWritePtr(dst, plane);

         // Since planes may be subsampled you have to query the height of them individually
         int h = vsapi->getFrameHeight(src[d->radius], plane);
         int y;
         int w = vsapi->getFrameWidth(src[d->radius], plane);
         //int x;

         if (d->scenechange > 0) {
            int dd2 = 0;
            int skiprest = 0;

            for (int i = d->radius - 1; i >= 0; i--) {
               if (!skiprest && !planeDisabled[i]) {
                  int scenevalues = there_is_only_c_scenechange(dstp, srcp[i], h, w, dst_stride, src_stride[i]);
                  if (scenevalues < d->scenechange) {
                     src_stride_trimmed[dd2] = src_stride[i];
                     srcp_trimmed[dd2] = srcp[i];
                     dd2++;
                  } else {
                     skiprest = 1;
                  }
                  planeDisabled[i] = skiprest;
               } else {
                  planeDisabled[i] = 1;
               }
            }
            skiprest = 0;

            for (int i = 0; i < d->radius; i++) {
               if (!skiprest && !planeDisabled[i + d->radius]) {
                  int scenevalues = there_is_only_c_scenechange(dstp, srcp[i + d->radius], h, w, dst_stride, src_stride[i + d->radius]);
                  if (scenevalues < d->scenechange) {
                     src_stride_trimmed[dd2] = src_stride[i + d->radius];
                     srcp_trimmed[dd2] = srcp[i + d->radius];
                     dd2++;
                  } else {
                     skiprest = 1;
                  }
                  planeDisabled[i + d->radius] = skiprest;
               } else {
                  planeDisabled[i + d->radius] = 1;
               }
            }

            for (int i = 0; i < dd2; i++) {
               srcp[i] = srcp_trimmed[i];
               src_stride[i] = src_stride_trimmed[i];
            }
            dd = dd2;
         }

         if (dd < 1) {
            return dst;
         }

         int c_div = 32768 / (dd + 1);

         // There was a "if (current_threshold)" in the original at this point, but current_threshold can't be zero here.
         for (y = 0; y < h; y++) {
            // if (mode == 1) {
            //    do_mode1_stuff();
            //    Yeah, this is where the mode 1 code would go
            //    if someone were to translate the mmx_accumulate_line()
            //    function. qtgmc doesn't use mode 1 so I don't care.
            // } else {
            //    there_is_only_c_accumulate_line_mode2(...);
            // }
            there_is_only_c_accumulate_line_mode2(dstp, srcp, dd, w, current_threshold, c_div);

            for (int i = 0; i < dd; i++) {
               srcp[i] += src_stride[i];
            }
            dstp += dst_stride;
         }
      }


      // Release the source frames.
      for (int i = 0; i < d->radius * 2 + 1; i++) {
         vsapi->freeFrame(src[i]);
      }

      // A reference is consumed when it is returned so saving the dst ref somewhere
      // and reusing it is not allowed.
      return dst;
   }

   return 0;
}


static void VS_CC temporalSoftenFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
   TemporalSoftenData *d = (TemporalSoftenData *)instanceData;
   vsapi->freeNode(d->node);
   free(d);
}


static void VS_CC temporalSoftenCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
   TemporalSoftenData d;
   TemporalSoftenData *data;
   const VSNodeRef *cref;
   int err;

   // Get a clip reference from the input arguments. This must be freed later.
   d.node = vsapi->propGetNode(in, "clip", 0, 0);
   d.vi = vsapi->getVideoInfo(d.node);

   if (!d.vi->format || d.vi->format->sampleType != stInteger || d.vi->format->bitsPerSample != 8 || d.vi->format->colorFamily != cmYUV) {
      vsapi->setError(out, "TemporalSoften: only constant format 8bit integer YUV input supported");
      vsapi->freeNode(d.node);
      return;
   }

   // Get the parameters.
   d.radius = vsapi->propGetInt(in, "radius", 0, 0);
   d.luma_threshold = vsapi->propGetInt(in, "luma_threshold", 0, 0);
   d.chroma_threshold = vsapi->propGetInt(in, "chroma_threshold", 0, 0);
   // Unused optional parameters default to 0,
   // which happens to be fine for scenechange.
   d.scenechange = vsapi->propGetInt(in, "scenechange", 0, &err);
   d.mode = vsapi->propGetInt(in, "mode", 0, &err);
   if (err) {
      d.mode = 2;
   }

   // Check the values.
   if (d.radius < 1 || d.radius > 7) {
      vsapi->setError(out, "TemporalSoften: radius must be between 1 and 7 (inclusive)");
      vsapi->freeNode(d.node);
      return;
   }

   if (d.luma_threshold < 0 || d.luma_threshold > 255) {
      vsapi->setError(out, "TemporalSoften: luma_threshold must be between 0 and 255 (inclusive)");
      vsapi->freeNode(d.node);
      return;
   }

   if (d.chroma_threshold < 0 || d.chroma_threshold > 255) {
      vsapi->setError(out, "TemporalSoften: chroma_threshold must be between 0 and 255 (inclusive)");
      vsapi->freeNode(d.node);
      return;
   }

   // With both thresholds at 0 TemporalSoften would do nothing to the frames.
   if (d.luma_threshold == 0 && d.chroma_threshold == 0) {
      vsapi->setError(out, "TemporalSoften: luma_threshold and chroma_threshold can't both be 0");
      vsapi->freeNode(d.node);
      return;
   }

   if (d.scenechange < 0 || d.scenechange > 254) {
      vsapi->setError(out, "TemporalSoften: scenechange must be between 0 and 254 (inclusive)");
      vsapi->freeNode(d.node);
      return;
   }

   if (d.mode != 2) {
      vsapi->setError(out, "TemporalSoften: mode must be 2. mode 1 is not implemented");
      vsapi->freeNode(d.node);
      return;
   }


   data = malloc(sizeof(d));
   *data = d;

   cref = vsapi->createFilter(in, out, "TemporalSoften", temporalSoftenInit, temporalSoftenGetFrame, temporalSoftenFree, fmParallel, 0, data, core);
   vsapi->propSetNode(out, "clip", cref, 0);
   vsapi->freeNode(cref);
   return;
}


void VS_CC VapourSynthPluginInit(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin *plugin) {
   configFunc("com.focus.temporalsoften", "focus", "VapourSynth TemporalSoften Filter", VAPOURSYNTH_API_VERSION, 1, plugin);
   registerFunc("TemporalSoften", "clip:clip;radius:int;luma_threshold:int;chroma_threshold:int;scenechange:int:opt;mode:int:opt", temporalSoftenCreate, 0, plugin);
}

