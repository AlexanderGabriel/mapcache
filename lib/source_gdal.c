/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching support file: GDAL datasource support (incomplete and disabled)
 * Author:   Thomas Bonfort, Even Rouault and the MapServer team.
 *
 ******************************************************************************
 * Copyright (c) 1996-2011 Regents of the University of Minnesota.
 * Copyright (c) 2004, Frank Warmerdam <warmerdam@pobox.com> (for GDALAutoCreateWarpedVRT who CreateWarpedVRT is derived from)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies of this Software or works derived from this Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *****************************************************************************/

#include "mapcache.h"
#include "ezxml.h"
#include <apr_tables.h>
#include <apr_strings.h>

#ifdef USE_GDAL

#include <gdal.h>
#include <cpl_conv.h>

#include "gdalwarper.h"
#include "cpl_string.h"
#include "ogr_srs_api.h"
#include "gdal_vrt.h"

#define MAPCACHE_DEFAULT_RESAMPLE_ALG           GRA_Bilinear

/* Note: this will also work with GDAL >= 2.0 */
#if GDAL_VERSION_MAJOR < 2
#define USE_PRE_GDAL2_METHOD
#endif
/*#define USE_PRE_GDAL2_METHOD*/

typedef struct mapcache_source_gdal mapcache_source_gdal;

/**\class mapcache_source_gdal
 * \brief GDAL mapcache_source
 * \implements mapcache_source
 */
struct mapcache_source_gdal {
  mapcache_source source;
  const char *datastr; /**< the gdal source string*/
  apr_table_t *gdal_params; /**< GDAL parameters specified in configuration */
  GDALDatasetH hDataset;
  GDALResampleAlg eResampleAlg; /**< resampling algorithm */
  const char *srcOvrLevel; /**< strategy to pickup source overview: AUTO, NONE, 
                                AUTO-xxx, xxxx. See -ovr doc in http://www.gdal.org/gdalwarp.html.
                                Only used for GDAL >= 2.0 (could probably be made to work for USE_PRE_GDAL2_METHOD with more work) */
};

#ifdef USE_PRE_GDAL2_METHOD
/* Creates a (virtual) dataset that matches an overview level of the source
   dataset. This dataset has references on the source dataset, so it should
   be closed before it.
*/
static GDALDatasetH CreateOverviewVRTDataset(GDALDatasetH hSrcDS,
                                             int iOvrLevel)
{
  double adfSrcGeoTransform[6];
  int iBand;
  GDALRasterBandH hFirstBand = GDALGetRasterBand(hSrcDS, 1);
  GDALRasterBandH hOvr = GDALGetOverview( hFirstBand, iOvrLevel );
  int nFullResXSize = GDALGetRasterBandXSize(hFirstBand);
  int nFullResYSize = GDALGetRasterBandYSize(hFirstBand);
  int nVRTXSize = GDALGetRasterBandXSize(hOvr);
  int nVRTYSize = GDALGetRasterBandYSize(hOvr);

  VRTDatasetH hVRTDS = VRTCreate( nVRTXSize, nVRTYSize );

  /* Scale geotransform */
  if( GDALGetGeoTransform( hSrcDS, adfSrcGeoTransform) == CE_None )
  {
    adfSrcGeoTransform[1] *= (double)nFullResXSize / nVRTXSize;
    adfSrcGeoTransform[2] *= (double)nFullResYSize / nVRTYSize;
    adfSrcGeoTransform[4] *= (double)nFullResXSize / nVRTXSize;
    adfSrcGeoTransform[5] *= (double)nFullResYSize / nVRTYSize;
    GDALSetProjection( hVRTDS, GDALGetProjectionRef(hSrcDS) );
    GDALSetGeoTransform( hVRTDS, adfSrcGeoTransform );
  }

  /* Scale GCPs */
  if( GDALGetGCPCount(hSrcDS) > 0 )
  {
    const GDAL_GCP* pasGCPsMain = GDALGetGCPs(hSrcDS);
    int nGCPCount = GDALGetGCPCount(hSrcDS);
    GDAL_GCP* pasGCPList = GDALDuplicateGCPs( nGCPCount, pasGCPsMain );
    int i;
    for(i = 0; i < nGCPCount; i++)
    {
        pasGCPList[i].dfGCPPixel *= (double)nVRTXSize / nFullResXSize;
        pasGCPList[i].dfGCPLine *= (double)nVRTYSize / nFullResYSize;
    }
    GDALSetGCPs( hVRTDS, nGCPCount, pasGCPList, GDALGetGCPProjection(hSrcDS) );
    GDALDeinitGCPs( nGCPCount, pasGCPList );
    CPLFree( pasGCPList );
  }

  /* Create bands */
  for(iBand = 1; iBand <= GDALGetRasterCount(hSrcDS); iBand++ )
  {
    GDALRasterBandH hSrcBand;
    GDALRasterBandH hVRTBand;
    int bNoDataSet = FALSE;
    double dfNoData;

    VRTAddBand( hVRTDS, GDT_Byte, NULL );
    hVRTBand = GDALGetRasterBand(hVRTDS, iBand);
    hSrcBand = GDALGetOverview(GDALGetRasterBand(hSrcDS, iBand), iOvrLevel);
    dfNoData = GDALGetRasterNoDataValue(hSrcBand, &bNoDataSet);
    if( bNoDataSet )
      GDALSetRasterNoDataValue(hVRTBand, dfNoData);

    /* Note: the consumer of this VRT is the warper, which doesn't do any */
    /* subsampled RasterIO requests, so NEAR is fine */
    VRTAddSimpleSource( hVRTBand, hSrcBand,
                        0, 0, nVRTXSize, nVRTYSize,
                        0, 0, nVRTXSize, nVRTYSize,
                        "NEAR", VRT_NODATA_UNSET );
  }

  return hVRTDS;
}
#endif

/* Derived from GDALAutoCreateWarpedVRT(), with various improvements. */
/* Returns a warped VRT that covers the passed extent, in pszDstWKT. */
/* The provided width and height are used, but the size of the returned dataset */
/* may not match those values. In the USE_PRE_GDAL2_METHOD, it should match them. */
/* In the non USE_PRE_GDAL2_METHOD case, it might be a multiple of those values. */
/* phTmpDS is an output parameter (a temporary VRT in the USE_PRE_GDAL2_METHOD case). */
static GDALDatasetH  
CreateWarpedVRT( GDALDatasetH hSrcDS, 
                 const char *pszSrcWKT,
                 const char *pszDstWKT,
                 int width, int height,
                 const mapcache_extent *extent,
                 GDALResampleAlg eResampleAlg, 
                 double dfMaxError, 
                 char** papszWarpOptions,
                 GDALDatasetH *phTmpDS )

{
    int i;
    GDALWarpOptions *psWO;
    double adfDstGeoTransform[6];
    GDALDatasetH hDstDS;
    int    nDstPixels, nDstLines;
    CPLErr eErr;
    int bHaveNodata = FALSE;
    char** papszOptions = NULL;

/* -------------------------------------------------------------------- */
/*      Populate the warp options.                                      */
/* -------------------------------------------------------------------- */

    psWO = GDALCreateWarpOptions();
    psWO->papszWarpOptions = CSLDuplicate(papszWarpOptions);

    psWO->eResampleAlg = eResampleAlg;

    psWO->hSrcDS = hSrcDS;

    psWO->nBandCount = GDALGetRasterCount( hSrcDS );
    if( psWO->nBandCount == 4 )
    {
        psWO->nBandCount = 3;
        psWO->nSrcAlphaBand = 4;
        psWO->nDstAlphaBand = 4;
    }
    /* Due to the reprojection, we might get transparency in the edges */
    else if( psWO->nBandCount == 3 )
        psWO->nDstAlphaBand = 4;

    psWO->panSrcBands = (int*)CPLMalloc( sizeof(int) * psWO->nBandCount );
    psWO->panDstBands = (int*)CPLMalloc( sizeof(int) * psWO->nBandCount );

    for( i = 0; i < psWO->nBandCount; i++ )
    {
        psWO->panSrcBands[i] = i+1;
        psWO->panDstBands[i] = i+1;
    }

/* -------------------------------------------------------------------- */
/*      Set nodata values if existing                                   */
/* -------------------------------------------------------------------- */
    GDALGetRasterNoDataValue( GDALGetRasterBand(hSrcDS, 1), &bHaveNodata);
    if( bHaveNodata )
    {
        psWO->padfSrcNoDataReal = (double *) 
            CPLMalloc(psWO->nBandCount*sizeof(double));
        psWO->padfSrcNoDataImag = (double *) 
            CPLCalloc(psWO->nBandCount, sizeof(double)); /* zero initialized */

        for( i = 0; i < psWO->nBandCount; i++ )
        {
            GDALRasterBandH hBand = GDALGetRasterBand( hSrcDS, i+1 );

            double dfReal = GDALGetRasterNoDataValue( hBand, &bHaveNodata );

            if( bHaveNodata )
            {
                psWO->padfSrcNoDataReal[i] = dfReal;
            }
            else
            {
                psWO->padfSrcNoDataReal[i] = -123456.789;
            }
        }
    }

/* -------------------------------------------------------------------- */
/*      Create the transformer.                                         */
/* -------------------------------------------------------------------- */
    psWO->pfnTransformer = GDALGenImgProjTransform;
    psWO->pTransformerArg = 
        GDALCreateGenImgProjTransformer( psWO->hSrcDS, pszSrcWKT, 
                                         NULL, pszDstWKT,
                                         TRUE, 1.0, 0 );

    if( psWO->pTransformerArg == NULL )
    {
        GDALDestroyWarpOptions( psWO );
        return NULL;
    }

/* -------------------------------------------------------------------- */
/*      Figure out the desired output bounds and resolution.            */
/* -------------------------------------------------------------------- */
    eErr =
        GDALSuggestedWarpOutput( hSrcDS, psWO->pfnTransformer, 
                                 psWO->pTransformerArg, 
                                 adfDstGeoTransform, &nDstPixels, &nDstLines );
    if( eErr != CE_None )
    {
        GDALDestroyTransformer( psWO->pTransformerArg );
        GDALDestroyWarpOptions( psWO );
        return NULL;
    }

/* -------------------------------------------------------------------- */
/*      To minimize the risk of extra resampling done by generic        */
/*      RasterIO itself and maximize resampling done in the wraper,     */
/*      adjust the resolution so that the overview factor of the output */
/*      dataset that will indirectly query matches an exiting overview  */
/*      factor of the input dataset.                                    */
/* -------------------------------------------------------------------- */
    {
        double dfDesiredXRes = (extent->maxx - extent->minx) / width;
        double dfDesiredYRes = (extent->maxy - extent->miny) / height;
        double dfDesiredRes = MIN( dfDesiredXRes, dfDesiredYRes );
        double dfGuessedFullRes = MIN( adfDstGeoTransform[1],
                                   fabs(adfDstGeoTransform[5]) );
        double dfApproxDstOvrRatio = dfDesiredRes / dfGuessedFullRes;

        GDALRasterBandH hFirstBand = GDALGetRasterBand(hSrcDS, 1);
        int nOvrCount = GDALGetOverviewCount(hFirstBand);
        int nSrcXSize = GDALGetRasterBandXSize(hFirstBand);
        int i;
        double dfSrcOvrRatio = 1.0;
        int iSelectedOvr = -1;
        for( i = 0; *phTmpDS == NULL && i < nOvrCount; i ++)
        {
            GDALRasterBandH hOvr = GDALGetOverview(hFirstBand, i);
            int nOvrXSize = GDALGetRasterBandXSize(hOvr);
            double dfCurOvrRatio = (double)nSrcXSize / nOvrXSize;
            if(dfCurOvrRatio > dfApproxDstOvrRatio+0.1 ) /* +0.1 to avoid rounding issues */
            {
                break;
            }
            dfSrcOvrRatio = dfCurOvrRatio;
            iSelectedOvr = i;
        }

#ifdef USE_PRE_GDAL2_METHOD
        if( iSelectedOvr >= 0 )
        {
            GDALDestroyTransformer( psWO->pTransformerArg );
            GDALDestroyWarpOptions( psWO );

            *phTmpDS = CreateOverviewVRTDataset(hSrcDS, iSelectedOvr);
            return CreateWarpedVRT( *phTmpDS,
                                    pszSrcWKT,
                                    pszDstWKT,
                                    width, height,
                                    extent,
                                    eResampleAlg, 
                                    dfMaxError, 
                                    papszWarpOptions,
                                    phTmpDS );
        }
#endif

        adfDstGeoTransform[1] = dfDesiredXRes / dfSrcOvrRatio;
        adfDstGeoTransform[5] = -dfDesiredYRes / dfSrcOvrRatio;
    }

/* -------------------------------------------------------------------- */
/*      Compute geotransform and raster dimension for our extent of     */
/*      interest.                                                       */
/* -------------------------------------------------------------------- */
    adfDstGeoTransform[0] = extent->minx;
    adfDstGeoTransform[2] = 0.0;
    adfDstGeoTransform[3] = extent->maxy;
    adfDstGeoTransform[4] = 0.0;
    nDstPixels = (int)( (extent->maxx - extent->minx) / adfDstGeoTransform[1] + 0.5 );
    nDstLines = (int)( (extent->maxy - extent->miny) / fabs(adfDstGeoTransform[5]) + 0.5 );
    /*printf("nDstPixels=%d nDstLines=%d\n", nDstPixels, nDstLines);*/

/* -------------------------------------------------------------------- */
/*      Update the transformer to include an output geotransform        */
/*      back to pixel/line coordinates.                                 */
/* -------------------------------------------------------------------- */

    GDALSetGenImgProjTransformerDstGeoTransform( 
        psWO->pTransformerArg, adfDstGeoTransform );

/* -------------------------------------------------------------------- */
/*      Do we want to apply an approximating transformation?            */
/* -------------------------------------------------------------------- */
    if( dfMaxError > 0.0 )
    {
        psWO->pTransformerArg = 
            GDALCreateApproxTransformer( psWO->pfnTransformer, 
                                         psWO->pTransformerArg, 
                                         dfMaxError );
        psWO->pfnTransformer = GDALApproxTransform;
        GDALApproxTransformerOwnsSubtransformer(psWO->pTransformerArg, TRUE);
    }

/* -------------------------------------------------------------------- */
/*      Create the VRT file.                                            */
/* -------------------------------------------------------------------- */

    /* We could potentially used GDALCreateWarpedVRT() instead of this logic */
    /* but GDALCreateWarpedVRT() in GDAL < 2.0.1 doesn't create the destination */
    /* alpha band. */

    papszOptions = CSLSetNameValue(NULL, "SUBCLASS", "VRTWarpedDataset");
    hDstDS = GDALCreate( GDALGetDriverByName("VRT"), "", nDstPixels, nDstLines,
                         psWO->nBandCount + ((psWO->nDstAlphaBand != 0) ? 1 : 0),
                         GDT_Byte, papszOptions );
    CSLDestroy(papszOptions);
    if( hDstDS == NULL )
    {
        GDALDestroyWarpOptions( psWO );
        return NULL;
    }

    psWO->hDstDS = hDstDS;

    GDALSetGeoTransform( hDstDS, adfDstGeoTransform );
    if( GDALInitializeWarpedVRT( hDstDS, psWO ) != CE_None )
    {
        GDALClose(hDstDS);
        GDALDestroyWarpOptions( psWO );
        return NULL;
    }

    GDALDestroyWarpOptions( psWO );

    return hDstDS;
}


/**
 * \private \memberof mapcache_source_gdal
 * \sa mapcache_source::render_metatile()
 */
void _mapcache_source_gdal_render_metatile(mapcache_context *ctx, mapcache_map *map)
{
  mapcache_source_gdal *gdal = (mapcache_source_gdal*)map->tileset->source;
  GDALDatasetH  hSrcDS,hDstDS;
  GDALDatasetH hTmpDS = NULL;
  OGRSpatialReferenceH hDstSRS;
  char *src_srs,*dst_srs;
  mapcache_buffer *data;
  unsigned char *rasterdata;
  CPLErr eErr;
  int bands_bgra[] = { 3, 2, 1, 4 }; /* mapcache buffer order is BGRA */

  CPLErrorReset();

  /* -------------------------------------------------------------------- */
  /*      Open source dataset.                                            */
  /* -------------------------------------------------------------------- */
  hSrcDS = GDALOpen( gdal->datastr, GA_ReadOnly );

  if( hSrcDS == NULL ) {
    ctx->set_error(ctx, 500, "Cannot open gdal source for %s .\n", gdal->source.name );
    return;
  }

  /* -------------------------------------------------------------------- */
  /*      Check that there's 3 or 4 raster bands.                         */
  /* -------------------------------------------------------------------- */
  if ( GDALGetRasterCount(hSrcDS) != 3 && GDALGetRasterCount(hSrcDS) != 4) {
    ctx->set_error(ctx, 500, "Input gdal source for %s has %d raster bands, but only 3 or 4 are supported.\n",
                   gdal->source.name, GDALGetRasterCount(hSrcDS) );
    GDALClose(hSrcDS);
    return;
  }

  if( GDALGetProjectionRef( hSrcDS ) != NULL
      && strlen(GDALGetProjectionRef( hSrcDS )) > 0 ) {
    src_srs = apr_pstrdup(ctx->pool,GDALGetProjectionRef( hSrcDS ));
  } else if( GDALGetGCPProjection( hSrcDS ) != NULL
           && strlen(GDALGetGCPProjection(hSrcDS)) > 0
           && GDALGetGCPCount( hSrcDS ) > 1 ) {
    src_srs = apr_pstrdup(ctx->pool,GDALGetGCPProjection( hSrcDS ));
  } else {
    ctx->set_error(ctx, 500, "Input gdal source for %s has no defined SRS\n", gdal->source.name );
    GDALClose(hSrcDS);
    return;
  }


  hDstSRS = OSRNewSpatialReference( NULL );
  if( OSRSetFromUserInput( hDstSRS, map->grid_link->grid->srs ) == OGRERR_NONE ) {
    dst_srs = NULL;
    OSRExportToWkt( hDstSRS, &dst_srs );
  }
  else {
    ctx->set_error(ctx,500,"failed to parse gdal srs %s",map->grid_link->grid->srs);
    GDALClose(hSrcDS);
    return;
  }

  OSRDestroySpatialReference( hDstSRS );

  hDstDS = CreateWarpedVRT( hSrcDS, src_srs, dst_srs,
                            map->width, map->height,
                            &map->extent,
                            gdal->eResampleAlg, 0.125, NULL, &hTmpDS );

  CPLFree(dst_srs);

  if( hDstDS == NULL ) {
    ctx->set_error(ctx, 500,"CreateWarpedVRT() failed");
    GDALClose(hSrcDS);
    return;
  }

  if(GDALGetRasterCount(hDstDS) != 4) {
    ctx->set_error(ctx, 500,"gdal did not create a 4 band image");
    GDALClose(hDstDS); /* close first this one, as it references hSrcDS */
    GDALClose(hSrcDS);
    return;
  }

  data = mapcache_buffer_create(map->height*map->width*4,ctx->pool);
  rasterdata = data->buf;

#if GDAL_VERSION_MAJOR >= 2
  {
    GDALRasterIOExtraArg sExtraArg;
    INIT_RASTERIO_EXTRA_ARG(sExtraArg);
    if( gdal->eResampleAlg == GRA_Bilinear )
      sExtraArg.eResampleAlg = GRIORA_Bilinear;
    else if( gdal->eResampleAlg == GRA_Cubic )
      sExtraArg.eResampleAlg = GRIORA_Cubic;
    else if( gdal->eResampleAlg == GRA_CubicSpline )
      sExtraArg.eResampleAlg = GRIORA_CubicSpline;
    else if( gdal->eResampleAlg == GRA_Lanczos )
      sExtraArg.eResampleAlg = GRIORA_Lanczos;
    else if( gdal->eResampleAlg == GRA_Average )
      sExtraArg.eResampleAlg = GRIORA_Average;

    if( gdal->srcOvrLevel != NULL )
    {
      /* If the user specified a particular strategy to choose the source */
      /* overview level, apply it now */
      GDALSetMetadataItem(hDstDS, "SrcOvrLevel",gdal->srcOvrLevel, NULL);
    }

    /* Hopefully, given how we adjust hDstDS resolution, we should query */
    /* exactly at the resolution of one overview level of hDstDS, and not */
    /* do extra resampling in generic RasterIO, but just in case specify */
    /* the resampling alg in sExtraArg. */
    eErr = GDALDatasetRasterIOEx( hDstDS, GF_Read,0,0,
                                  GDALGetRasterXSize(hDstDS),
                                  GDALGetRasterYSize(hDstDS),
                                  rasterdata,map->width,map->height,GDT_Byte,
                                  4, bands_bgra,
                                  4,4*map->width,1, &sExtraArg );
  }
#else
  eErr = GDALDatasetRasterIO( hDstDS, GF_Read,0,0,
                              GDALGetRasterXSize(hDstDS),
                              GDALGetRasterYSize(hDstDS),
                              rasterdata,map->width,map->height,GDT_Byte,
                              4, bands_bgra,
                              4,4*map->width,1 );
#endif

  if( eErr != CE_None ) {
    ctx->set_error(ctx, 500,"GDAL I/O error occured");
    GDALClose(hDstDS); /* close first this one, as it references hTmpDS or hSrcDS */
    if( hTmpDS )
      GDALClose(hTmpDS); /* references hSrcDS, so close before */
    GDALClose(hSrcDS);
    return;
  }

  map->raw_image = mapcache_image_create(ctx);
  map->raw_image->w = map->width;
  map->raw_image->h = map->height;
  map->raw_image->stride = map->width * 4;
  map->raw_image->data = rasterdata;
  map->raw_image->has_alpha = MC_ALPHA_UNKNOWN;

  GDALClose( hDstDS ); /* close first this one, as it references hTmpDS or hSrcDS */
  if( hTmpDS )
    GDALClose(hTmpDS); /* references hSrcDS, so close before */
  GDALClose( hSrcDS);
}

/**
 * \private \memberof mapcache_source_gdal
 * \sa mapcache_source::configuration_parse()
 */
void _mapcache_source_gdal_configuration_parse(mapcache_context *ctx, ezxml_t node, mapcache_source *source)
{
  ezxml_t cur_node;
  mapcache_source_gdal *src = (mapcache_source_gdal*)source;

  if ((cur_node = ezxml_child(node,"data")) != NULL) {
    src->datastr = apr_pstrdup(ctx->pool,cur_node->txt);
  }

  if ((cur_node = ezxml_child(node,"gdalparams")) != NULL) {
    for(cur_node = cur_node->child; cur_node; cur_node = cur_node->sibling) {
      apr_table_set(src->gdal_params, cur_node->name, cur_node->txt);
    }
  }
}

/**
 * \private \memberof mapcache_source_gdal
 * \sa mapcache_source::configuration_check()
 */
void _mapcache_source_gdal_configuration_check(mapcache_context *ctx, mapcache_cfg *cfg,
    mapcache_source *source)
{
  mapcache_source_gdal *src = (mapcache_source_gdal*)source;
  const char* pszResampleAlg;

  /* check all required parameters are configured */
  if( src->datastr == NULL || !strlen(src->datastr)) {
    ctx->set_error(ctx, 500, "gdal source %s has no data",source->name);
    return;
  }
  src->hDataset = GDALOpen(src->datastr,GA_ReadOnly);
  if( src->hDataset == NULL ) {
    ctx->set_error(ctx, 500, "gdalOpen failed on data %s", src->datastr);
    return;
  }
  GDALClose(src->hDataset);
  src->hDataset = NULL;

  src->eResampleAlg = MAPCACHE_DEFAULT_RESAMPLE_ALG;
  pszResampleAlg = apr_table_get(src->gdal_params,"resampleAlg");
  if( pszResampleAlg != NULL ) {
    if( EQUALN( pszResampleAlg, "NEAR", 4) )
      src->eResampleAlg = GRA_NearestNeighbour;
    else if( EQUAL( pszResampleAlg, "BILINEAR") )
      src->eResampleAlg = GRA_Bilinear;
    else if( EQUAL( pszResampleAlg, "CUBIC") )
      src->eResampleAlg = GRA_Cubic;
    else if( EQUAL( pszResampleAlg, "CUBICSPLINE") )
      src->eResampleAlg = GRA_CubicSpline;
    else if( EQUAL( pszResampleAlg, "LANCZOS") )
      src->eResampleAlg = GRA_Lanczos;
#if GDAL_VERSION_MAJOR >= 2
    else if( EQUAL( pszResampleAlg, "AVERAGE") )
      src->eResampleAlg = GRA_Average;
#endif
    else {
      ctx->set_error(ctx, 500, "unsupported resampleAlg: %s", pszResampleAlg);
      return;
    }
  }

  src->srcOvrLevel = apr_table_get(src->gdal_params,"srcOvrLevel");
}
#endif //USE_GDAL

mapcache_source* mapcache_source_gdal_create(mapcache_context *ctx)
{
#ifdef USE_GDAL
  mapcache_source_gdal *source = apr_pcalloc(ctx->pool, sizeof(mapcache_source_gdal));
  if(!source) {
    ctx->set_error(ctx, 500, "failed to allocate gdal source");
    return NULL;
  }
  mapcache_source_init(ctx, &(source->source));
  source->source.type = MAPCACHE_SOURCE_GDAL;
  source->source.render_map = _mapcache_source_gdal_render_metatile;
  source->source.configuration_check = _mapcache_source_gdal_configuration_check;
  source->source.configuration_parse_xml = _mapcache_source_gdal_configuration_parse;
  source->gdal_params = apr_table_make(ctx->pool,4);
  GDALAllRegister();
  return (mapcache_source*)source;
#else
  ctx->set_error(ctx, 400, "failed to create gdal source, GDAL support is not compiled in this version");
  return NULL;
#endif
}



/* vim: ts=2 sts=2 et sw=2
*/
