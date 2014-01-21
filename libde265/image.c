/*
 * H.265 video codec.
 * Copyright (c) 2013 StrukturAG, Dirk Farin, <farin@struktur.de>
 *
 * This file is part of libde265.
 *
 * libde265 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * libde265 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libde265.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "image.h"
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <assert.h>

#ifdef HAVE___MINGW_ALIGNED_MALLOC
#define ALLOC_ALIGNED(alignment, size)         __mingw_aligned_malloc((size), (alignment))
#define FREE_ALIGNED(mem)                      __mingw_aligned_free((mem))
#elif _WIN32
#define ALLOC_ALIGNED(alignment, size)         _aligned_malloc((size), (alignment))
#define FREE_ALIGNED(mem)                      _aligned_free((mem))
#else
#define ALLOC_ALIGNED(alignment, size)      memalign((alignment), (size))
#define FREE_ALIGNED(mem)                   free((mem))
#endif

#define ALLOC_ALIGNED_16(size)              ALLOC_ALIGNED(16, size)

static const int alignment = 16;


void de265_init_image(de265_image* img) // (optional) init variables, do not alloc image
{
  memset(img, 0, sizeof(de265_image));

  img->picture_order_cnt_lsb = -1; // undefined
  img->PicOrderCntVal = -1; // undefined
  img->PicState = UnusedForReference;

  de265_mutex_init(&img->mutex);
  de265_cond_init(&img->finished_cond);
}



void de265_alloc_image(de265_image* img, int w,int h, enum de265_chroma c,
                       const seq_parameter_set* sps)
{
  const int border=0;  // TODO: remove the border altogether

  // --- allocate image buffer (or reuse old one) ---

  if (img->width != w || img->height != h || img->chroma_format != c || img->border != border) {

    int chroma_width = w;
    int chroma_height= h;

    if (c==de265_chroma_420) {
      chroma_width  = (chroma_width +1)/2;
      chroma_height = (chroma_height+1)/2;
    }

    if (c==de265_chroma_422) {
      chroma_height = (chroma_height+1)/2;
    }

    img->stride        = (w           +2*border+alignment-1) / alignment * alignment;
    img->chroma_stride = (chroma_width+2*border+alignment-1) / alignment * alignment;

    img->width = w;
    img->height= h;
    img->border=border;
    img->chroma_width = chroma_width;
    img->chroma_height= chroma_height;

    img->chroma_format= c;

    FREE_ALIGNED(img->y_mem);
    img->y_mem = (uint8_t *)ALLOC_ALIGNED_16(img->stride * (h+2*border));
    img->y     = img->y_mem + border + 2*border*img->stride;

    if (c != de265_chroma_mono) {
      FREE_ALIGNED(img->cb_mem);
      FREE_ALIGNED(img->cr_mem);
      img->cb_mem = (uint8_t *)ALLOC_ALIGNED_16(img->chroma_stride * (chroma_height+2*border));
      img->cr_mem = (uint8_t *)ALLOC_ALIGNED_16(img->chroma_stride * (chroma_height+2*border));

      img->cb     = img->cb_mem + border + 2*border*img->chroma_stride;
      img->cr     = img->cr_mem + border + 2*border*img->chroma_stride;
    } else {
      img->cb_mem = NULL;
      img->cr_mem = NULL;
      img->cb     = NULL;
      img->cr     = NULL;
    }
  }

  // --- allocate decoding info arrays ---

  if (sps) {
    // intra pred mode

    int intraPredModeSize = sps->PicWidthInMinPUs * sps->PicHeightInMinPUs;
    if (intraPredModeSize != img->intraPredModeSize) {
      img->intraPredModeSize = intraPredModeSize;
      free(img->intraPredMode);
      img->intraPredMode = (uint8_t *) malloc(intraPredModeSize * sizeof(*img->intraPredMode));
    }


    // cb info

    if (img->cb_info_size != sps->PicSizeInMinCbsY ||
        img->cb_info == NULL) {
      img->cb_info_size = sps->PicSizeInMinCbsY;
      free(img->cb_info);
      img->cb_info = (CB_ref_info*)malloc(sizeof(CB_ref_info) * img->cb_info_size);
    }


    // pu info

    int puWidth  = sps->PicWidthInMinCbsY  << (sps->Log2MinCbSizeY -2);
    int puHeight = sps->PicHeightInMinCbsY << (sps->Log2MinCbSizeY -2);

    if (img->pb_info_size != puWidth*puHeight ||
        img->pb_info == NULL) {
      img->pb_info_size   = puWidth*puHeight;
      img->pb_info_stride = puWidth;
      free(img->pb_info);
      img->pb_info = (PB_ref_info*)malloc(sizeof(PB_ref_info) * img->pb_info_size);
      // ctx->img->pb_rootIdx = (int*)malloc(sizeof(int) * ctx->img->pb_info_size);
    }
  }
}


void de265_free_image(de265_image* img)
{
  if (img->y)  FREE_ALIGNED(img->y_mem);
  if (img->cb) FREE_ALIGNED(img->cb_mem);
  if (img->cr) FREE_ALIGNED(img->cr_mem);

  free(img->cb_info);
  free(img->pb_info);
  //free(img->pb_rootIdx);

  free(img->intraPredMode);

  de265_cond_destroy(&img->finished_cond);
  de265_mutex_destroy(&img->mutex);

  memset(img, 0, sizeof(de265_image));
}


void de265_fill_image(de265_image* img, int y,int cb,int cr)
{
  if (y>=0) {
    memset(img->y_mem, y, img->stride * (img->height+2*img->border));
  }

  if (cb>=0) {
    memset(img->cb_mem, cb, img->chroma_stride * (img->chroma_height+2*img->border));
  }

  if (cr>=0) {
    memset(img->cr_mem, cr, img->chroma_stride * (img->chroma_height+2*img->border));
  }
}


void de265_copy_image(de265_image* dest, const de265_image* src)
{
  for (int y=0;y<src->height;y++) {
    memcpy(dest->y+y*dest->stride, src->y+y*src->stride, src->width);
  }

  if (src->chroma_format != de265_chroma_mono) {
    for (int y=0;y<src->chroma_height;y++) {
      memcpy(dest->cb+y*dest->chroma_stride, src->cb+y*src->chroma_stride, src->chroma_width);
      memcpy(dest->cr+y*dest->chroma_stride, src->cr+y*src->chroma_stride, src->chroma_width);
    }
  }
}


void get_image_plane(const de265_image* img, int cIdx, uint8_t** image, int* stride)
{
  switch (cIdx) {
  case 0: *image = img->y;  if (stride) *stride = img->stride; break;
  case 1: *image = img->cb; if (stride) *stride = img->chroma_stride; break;
  case 2: *image = img->cr; if (stride) *stride = img->chroma_stride; break;
  default: *image = NULL; if (stride) *stride = 0; break;
  }
}


void increase_pending_tasks(de265_image* img, int n)
{
  de265_sync_add_and_fetch(&img->tasks_pending, n);
}

void decrease_pending_tasks(de265_image* img, int n)
{
  int pending = de265_sync_sub_and_fetch(&img->tasks_pending, n);

  assert(pending >= 0);

  if (pending==0) {
    de265_cond_broadcast(&img->finished_cond, &img->mutex);
  }
}

void wait_for_completion(de265_image* img)
{
  de265_mutex_lock(&img->mutex);
  while (img->tasks_pending>0) {
    de265_cond_wait(&img->finished_cond, &img->mutex);
  }
  de265_mutex_unlock(&img->mutex);
}
