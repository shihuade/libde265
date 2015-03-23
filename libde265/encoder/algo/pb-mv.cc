/*
 * H.265 video codec.
 * Copyright (c) 2013-2014 struktur AG, Dirk Farin <farin@struktur.de>
 *
 * Authors: struktur AG, Dirk Farin <farin@struktur.de>
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


#include "libde265/encoder/algo/pb-mv.h"
#include "libde265/encoder/algo/coding-options.h"
#include "libde265/encoder/encoder-context.h"
#include <assert.h>
#include <limits>
#include <math.h>



enc_cb* Algo_PB_MV_Test::analyze(encoder_context* ectx,
                                 context_model_table& ctxModel,
                                 enc_cb* cb,
                                 int PBidx, int x,int y,int w,int h)
{
  enum MVTestMode testMode = mParams.testMode();


  MotionVector mvp[2];

  fill_luma_motion_vector_predictors(ectx, ectx->shdr, ectx->img,
                                     cb->x,cb->y,1<<cb->log2Size, x,y,w,h,
                                     0, // l
                                     0, 0, // int refIdx, int partIdx,
                                     mvp);

  //printf("%d/%d: [%d;%d] [%d;%d]\n",cb->x,cb->y, mvp[0].x,mvp[0].y, mvp[1].x,mvp[1].y);


  motion_spec&   spec = cb->inter.pb[PBidx].spec;
  PredVectorInfo& vec = cb->inter.pb[PBidx].motion;

  spec.merge_flag = 0;
  spec.merge_idx  = 0;

  spec.inter_pred_idc = PRED_L0;
  spec.refIdx[0] = vec.refIdx[0] = 0;
  spec.mvp_l0_flag = 0;

  int value = mParams.range();

  switch (testMode) {
  case MVTestMode_Zero:
    spec.mvd[0][0]=0;
    spec.mvd[0][1]=0;
    break;

  case MVTestMode_Random:
    spec.mvd[0][0] = (rand() % (2*value+1)) - value;
    spec.mvd[0][1] = (rand() % (2*value+1)) - value;
    break;

  case MVTestMode_Horizontal:
    spec.mvd[0][0]=value;
    spec.mvd[0][1]=0;
    break;

  case MVTestMode_Vertical:
    spec.mvd[0][0]=0;
    spec.mvd[0][1]=value;
    break;
  }

  spec.mvd[0][0] -= mvp[0].x;
  spec.mvd[0][1] -= mvp[0].y;

  vec.mv[0].x = mvp[0].x + spec.mvd[0][0];
  vec.mv[0].y = mvp[0].y + spec.mvd[0][1];
  vec.predFlag[0] = 1;
  vec.predFlag[1] = 0;

  ectx->img->set_mv_info(x,y,w,h, vec);

  generate_inter_prediction_samples(ectx, ectx->img, ectx->shdr,
                                    cb->x,cb->y, // int xC,int yC,
                                    0,0,         // int xB,int yB,
                                    1<<cb->log2Size, // int nCS,
                                    1<<cb->log2Size,
                                    1<<cb->log2Size, // int nPbW,int nPbH,
                                    &vec);

  // TODO estimate rate for sending MV

  int IntraSplitFlag = 0;
  int MaxTrafoDepth = ectx->sps.max_transform_hierarchy_depth_inter;

  if (mCodeResidual) {
    assert(false);
#if 0
    cb->transform_tree = mTBSplit->analyze(ectx,ctxModel, ectx->imgdata->input, NULL, cb,
                                           cb->x,cb->y,cb->x,cb->y, cb->log2Size,0,
                                           0, MaxTrafoDepth, IntraSplitFlag);

    cb->inter.rqt_root_cbf = ! cb->transform_tree->isZeroBlock();

    cb->distortion = cb->transform_tree->distortion;
    cb->rate       = cb->transform_tree->rate;
#endif
  }
  else {
    const de265_image* input = ectx->imgdata->input;
    de265_image* img   = ectx->img;
    int x0 = cb->x;
    int y0 = cb->y;
    int tbSize = 1<<cb->log2Size;

    cb->distortion = compute_distortion_ssd(input, img, x0,y0, cb->log2Size, 0);
    cb->rate = 5; // fake (MV)

    cb->inter.rqt_root_cbf = 0;
  }

  return cb;
}




int sad(const uint8_t* p1,int stride1,
        const uint8_t* p2,int stride2,
        int w,int h)
{
  int cost=0;

  for (int y=0;y<h;y++) {
    for (int x=0;x<w;x++) {
      cost += abs_value(*p1 - *p2);
      p1++;
      p2++;
    }

    p1 += stride1-w;
    p2 += stride2-w;
  }

  return cost;
}


enc_cb* Algo_PB_MV_Search::analyze(encoder_context* ectx,
                                   context_model_table& ctxModel,
                                   enc_cb* cb,
                                   int PBidx, int x,int y,int pbW,int pbH)
{
  enum MVSearchAlgo searchAlgo = mParams.mvSearchAlgo();


  MotionVector mvp[2];

  fill_luma_motion_vector_predictors(ectx, ectx->shdr, ectx->img,
                                     cb->x,cb->y,1<<cb->log2Size, x,y,pbW,pbH,
                                     0, // l
                                     0, 0, // int refIdx, int partIdx,
                                     mvp);

  motion_spec&   spec = cb->inter.pb[PBidx].spec;
  PredVectorInfo& vec = cb->inter.pb[PBidx].motion;

  spec.merge_flag = 0;
  spec.merge_idx  = 0;

  spec.inter_pred_idc = PRED_L0;
  spec.refIdx[0] = vec.refIdx[0] = 0;
  spec.mvp_l0_flag = 0;

  int hrange = mParams.hrange();
  int vrange = mParams.vrange();

  // previous frame (TODO)
  const de265_image* refimg   = ectx->get_image(ectx->imgdata->frame_number -1);
  const de265_image* inputimg = ectx->imgdata->input;

  int w = refimg->get_width();
  int h = refimg->get_height();

  int mincost = 0x7fffffff;

  for (int my = y-vrange; my<=y+vrange; my++)
    for (int mx = x-hrange; mx<=x+hrange; mx++)
      {
        if (mx<0 || mx+pbW>w || my<0 || my+pbH>h) continue;

        int cost = sad(refimg->get_image_plane_at_pos(0,mx,my),
                       refimg->get_image_stride(0),
                       inputimg->get_image_plane_at_pos(0,x,y),
                       inputimg->get_image_stride(0),
                       pbW,pbH);

        if (cost<mincost) {
          mincost=cost;

          spec.mvd[0][0]=(mx-x)<<2;
          spec.mvd[0][1]=(my-y)<<2;
        }
      }

  spec.mvd[0][0] -= mvp[0].x;
  spec.mvd[0][1] -= mvp[0].y;

  vec.mv[0].x = mvp[0].x + spec.mvd[0][0];
  vec.mv[0].y = mvp[0].y + spec.mvd[0][1];
  vec.predFlag[0] = 1;
  vec.predFlag[1] = 0;

  ectx->img->set_mv_info(x,y,pbW,pbH, vec);

  generate_inter_prediction_samples(ectx, ectx->img, ectx->shdr,
                                    cb->x,cb->y, // int xC,int yC,
                                    0,0,         // int xB,int yB,
                                    1<<cb->log2Size, // int nCS,
                                    1<<cb->log2Size,
                                    1<<cb->log2Size, // int nPbW,int nPbH,
                                    &vec);

  // --- create residual ---



  // TODO estimate rate for sending MV

  int IntraSplitFlag = 0;
  int MaxTrafoDepth = ectx->sps.max_transform_hierarchy_depth_inter;

  if (mCodeResidual) {
    assert(false);
#if 0
    cb->transform_tree = mTBSplit->analyze(ectx,ctxModel, ectx->imgdata->input, NULL, cb,
                                           cb->x,cb->y,cb->x,cb->y, cb->log2Size,0,
                                           0, MaxTrafoDepth, IntraSplitFlag);

    cb->inter.rqt_root_cbf = ! cb->transform_tree->isZeroBlock();

    cb->distortion = cb->transform_tree->distortion;
    cb->rate       = cb->transform_tree->rate;
#endif
  }
  else {
    const de265_image* input = ectx->imgdata->input;
    de265_image* img   = ectx->img;
    int x0 = cb->x;
    int y0 = cb->y;
    int tbSize = 1<<cb->log2Size;

    cb->distortion = compute_distortion_ssd(input, img, x0,y0, cb->log2Size, 0);
    cb->rate = 5; // fake (MV)

    cb->inter.rqt_root_cbf = 0;
  }

  return cb;
}
